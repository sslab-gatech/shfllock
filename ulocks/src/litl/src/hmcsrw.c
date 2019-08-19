/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2016 Hugo Guiroux <hugo.guiroux at gmail dot com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of his software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 *
 * T. E. Anderson. 1990.
 * The Performance of Spin Lock Alternatives for Shared-Memory Multiprocessors.
 * IEEE Trans. Parallel Distrib. Syst. 1, 1 (January 1990).
 *
 * Lock design summary:
 * This is just a test and set on the same memory location.
 * However, instead of doing an atomic operation for each loop iteration when
 * trying to grab the lock, the thread first tries to check if the lock is
 * taken with a regular memory access.
 * This avoid useless cache invalidations.
 */
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <sys/mman.h>
#include <pthread.h>
#include <assert.h>
#include <hmcsrw.h>

#include "waiting_policy.h"
#include "interpose.h"
#include "utils.h"

extern __thread unsigned int cur_thread_id;
extern __thread unsigned int lock_status;

#define WRITE_STATE     1
#define READ_STATE      2

#define COHORT_START 1
#define ACQUIRE_PARENT (UINT64_MAX - 1)
#define WAIT UINT64_MAX

static inline int current_numa_node() {
    unsigned long a, d, c;
    int core;
    __asm__ volatile("rdtscp" : "=a"(a), "=d"(d), "=c"(c));
    core = c & 0xFFF;
    return core / (CPU_NUMBER / NUMA_NODES);
}

hmcsrw_rwlock_t *hmcsrw_mutex_create(const pthread_mutexattr_t *attr) {
    hmcsrw_rwlock_t *impl =
        (hmcsrw_rwlock_t *)alloc_cache_align(sizeof(hmcsrw_rwlock_t));

    // Link local nodes to parent
    uint8_t i;
    for (i = 0; i < NUMA_NODES; i++) {
        impl->local[i].parent = &impl->global;
        impl->local[i].tail   = NULL;
    }

    // Init the parent
    impl->global.parent = NULL;
    impl->global.tail   = NULL;

    return impl;
}


static inline int __hmcsrw_rwlock_global_lock(hmcsrw_hnode_t *impl,
                                           hmcsrw_qnode_t *me) {
    hmcsrw_qnode_t *tail;

    me->next   = 0;
    me->status = LOCKED;

    tail = xchg_64((void *)&impl->tail, (void *)me);

    /* No one there? */
    if (!tail) {
        me->status = UNLOCKED;
        DEBUG("[%2d] Locking global %p\n", cur_thread_id, impl);
        return 0;
    }

    /* Someone there, need to link in */
    COMPILER_BARRIER();
    tail->next = me;

    while (me->status == LOCKED)
        CPU_PAUSE();

    DEBUG("[%2d] Locking global %p\n", cur_thread_id, impl);
    return 0;
}

static inline int __hmcsrw_rwlock_local_lock(hmcsrw_hnode_t *impl,
                                          hmcsrw_qnode_t *me) {
    hmcsrw_qnode_t *tail;

    // Prepare the node for use
    me->next   = 0;
    me->status = WAIT;

    // printf("[%2d] Enqueing %p on %p\n", cur_thread_id, me);
    tail = xchg_64((void *)&impl->tail, (void *)me);

    if (tail) {
        tail->next = me;
        uint64_t cur_status;

        DEBUG("[%2d] There was someone (%p)...\n", cur_thread_id, tail);

        COMPILER_BARRIER();
        while ((cur_status = me->status) == WAIT)
            CPU_PAUSE();

        // Acquired, enter CS
        if (cur_status < ACQUIRE_PARENT) {
            DEBUG("[%2d] Locking local without locking global %p\n",
                  cur_thread_id, impl);
            return 0;
        }
    }

    DEBUG("[%2d] Locking local %p\n", cur_thread_id, impl);
    me->status = COHORT_START;
    int ret    = __hmcsrw_rwlock_global_lock(impl->parent, &impl->node);
    return ret;
}

static inline void __hmcsrw_release_helper(hmcsrw_hnode_t *impl, hmcsrw_qnode_t *me,
                                         uint64_t val) {
    /* No successor yet? */
    if (!me->next) {
        /* Try to atomically unlock */
        if (__sync_val_compare_and_swap(&impl->tail, me, 0) == me)
            return;

        /* Wait for successor to appear */
        while (!me->next)
            CPU_PAUSE();
    }

    // Pass lock
    me->next->status = val;
    MEMORY_BARRIER();
}

static inline int __hmcsrw_rwlock_global_trylock(hmcsrw_hnode_t *impl,
                                              hmcsrw_qnode_t *me) {
    hmcsrw_qnode_t *tail;

    me->next   = 0;
    me->status = LOCKED;

    tail = __sync_val_compare_and_swap(&impl->tail, NULL, me);
    if (tail == NULL) {
        me->status = UNLOCKED;
        return 0;
    }

    return EBUSY;
}

static inline int __hmcsrw_rwlock_local_trylock(hmcsrw_hnode_t *impl,
                                             hmcsrw_qnode_t *me) {
    hmcsrw_qnode_t *tail;

    // Prepare the node for use
    me->next   = 0;
    me->status = WAIT;

    tail = __sync_val_compare_and_swap(&impl->tail, NULL, me);

    if (tail != NULL) {
        return EBUSY;
    }

    me->status = COHORT_START;
    int ret    = __hmcsrw_rwlock_global_trylock(impl->parent, &impl->node);

    // Unable to get the global, release the local and fail
    if (ret == EBUSY) {
        // Unlock and ask the successor to get the global lock if it is here
        __hmcsrw_release_helper(impl, me, ACQUIRE_PARENT);
    }

    return ret;
}

static inline int __hmcsrw_rwlock_global_unlock(hmcsrw_hnode_t *impl,
                                                hmcsrw_qnode_t *me) {
    DEBUG("[%2d] Unlocking global %p\n", cur_thread_id, impl);
    __hmcsrw_release_helper(impl, me, UNLOCKED);
    return 0;
}

static inline int __hmcsrw_rwlock_local_unlock(hmcsrw_hnode_t *impl,
                                               hmcsrw_qnode_t *me) {
    uint64_t cur_count = me->status;

    DEBUG("[%2d] Unlocking local %p\n", cur_thread_id, impl);

    // Lower level release
    if (cur_count == RELEASE_THRESHOLD) {
        DEBUG("[%2d] Threshold reached\n", cur_thread_id);
        // Reached threshold, release the next level (suppose 2-level)
        __hmcsrw_rwlock_global_unlock(impl->parent, &impl->node);

        // Ask successor to acquire next-level lock
        __hmcsrw_release_helper(impl, me, ACQUIRE_PARENT);
        return 0;
    }

    // Not reached threshold
    hmcsrw_qnode_t *succ = me->next;
    if (succ) {
        DEBUG("[%2d] Successor is here\n", cur_thread_id);
        succ->status = cur_count + 1;
        return 0;
    }

    // No known successor, release to parent
    __hmcsrw_rwlock_global_unlock(impl->parent, &impl->node);

    // Ask successor to acquire next-level lock
    __hmcsrw_release_helper(impl, me, ACQUIRE_PARENT);
    return 0;
}


int hmcsrw_rwlock_lock(hmcsrw_rwlock_t *impl, hmcsrw_qnode_t *me) {
    hmcsrw_hnode_t *local = &impl->local[current_numa_node()];

    // Must remember the last local node for release
    me->last_local = local;

    DEBUG("[%2d] Waiting for local lock %p\n", cur_thread_id, local);
    int ret = __hmcsrw_rwlock_local_lock(local, me);
    assert(ret == 0);
    DEBUG("[%2d]\tLock acquired posix=%p\n", cur_thread_id, &impl->posix_lock);
    return ret;
}

int hmcsrw_rwlock_trylock(hmcsrw_rwlock_t *impl, hmcsrw_qnode_t *me) {
    hmcsrw_hnode_t *local = &impl->local[current_numa_node()];

    // Must remember the last local node for release
    me->last_local = local;

    int ret = __hmcsrw_rwlock_local_trylock(local, me);

    return ret;
}

int hmcsrw_rwlock_destroy(hmcsrw_rwlock_t *lock) {
    free(lock);
    lock = NULL;

    return 0;
}

int hmcsrw_cond_init(hmcsrw_cond_t *cond, const pthread_condattr_t *attr) {
    fprintf(stderr, "Error cond_var not supported.");
    assert(0);
}

int hmcsrw_cond_timedwait(hmcsrw_cond_t *cond, hmcsrw_rwlock_t *lock,
                        hmcsrw_qnode_t *me, const struct timespec *ts) {
    fprintf(stderr, "Error cond_var not supported.");
    assert(0);
}

int hmcsrw_cond_wait(hmcsrw_cond_t *cond, hmcsrw_rwlock_t *lock, hmcsrw_qnode_t *me) {
    return hmcsrw_cond_timedwait(cond, lock, me, 0);
}

int hmcsrw_cond_signal(hmcsrw_cond_t *cond) {
    fprintf(stderr, "Error cond_var not supported.");
    assert(0);
}

int hmcsrw_cond_broadcast(hmcsrw_cond_t *cond) {
    fprintf(stderr, "Error cond_var not supported.");
    assert(0);
}

int hmcsrw_cond_destroy(hmcsrw_cond_t *cond) {
    fprintf(stderr, "Error cond_var not supported.");
    assert(0);
}

// rwlock
hmcsrw_rwlock_t *hmcsrw_rwlock_create(const pthread_rwlockattr_t *attr) {
    hmcsrw_rwlock_t *impl =
        (hmcsrw_rwlock_t *)alloc_cache_align(sizeof(hmcsrw_rwlock_t));

    // Link local nodes to parent
    uint8_t i;
    for (i = 0; i < NUMA_NODES; i++) {
        impl->local[i].parent = &impl->global;
        impl->local[i].tail   = NULL;
        impl->local[i].active_readers = 0;
    }

    // Init the parent
    impl->global.parent = NULL;
    impl->global.tail   = NULL;

    return impl;
}

int hmcsrw_rwlock_rdlock(hmcsrw_rwlock_t *impl, hmcsrw_qnode_t *me) {
    hmcsrw_hnode_t *local = &impl->local[current_numa_node()];

    me->last_local = local;

    for (;;) {
        while (impl->global.tail)
            CPU_PAUSE();

        __sync_fetch_and_add(&local->active_readers, 1);
        if (!impl->global.tail)
            break;
        __sync_fetch_and_sub(&local->active_readers, 1);
    }
    lock_status = READ_STATE;
    me->lock_status = READ_STATE;
    /* printf("%s[%d]: lock_state: %d\n", __func__, cur_thread_id, me->lock_status); */
    return 0;
}

static inline void check_active_readers(hmcsrw_rwlock_t *impl)
{
    int i;
    for (i = 0; i < NUMA_NODES; ++i) {
        while (impl->local[i].active_readers != 0)
            CPU_PAUSE();
    }
}

int hmcsrw_rwlock_wrlock(hmcsrw_rwlock_t *impl, hmcsrw_qnode_t *me) {
    hmcsrw_hnode_t *local = &impl->local[current_numa_node()];

    // Must remember the last local node for release
    me->last_local = local;

    DEBUG("[%2d] Waiting for local lock %p\n", cur_thread_id, local);
    int ret = __hmcsrw_rwlock_local_lock(local, me);
    assert(ret == 0);
    DEBUG("[%2d]\tLock acquired posix=%p\n", cur_thread_id, &impl->posix_lock);
    check_active_readers(impl);
    lock_status = WRITE_STATE;
    me->lock_status = WRITE_STATE;
    /* printf("%s[%d]: lock_state: %d\n", __func__, cur_thread_id, me->lock_status); */
    return ret;
}

int hmcsrw_rwlock_tryrdlock(hmcsrw_rwlock_t *impl, hmcsrw_qnode_t *me) {
    hmcsrw_hnode_t *local = &impl->local[current_numa_node()];

    __sync_fetch_and_add(&local->active_readers, 1);

    if (impl->global.tail) {
        __sync_fetch_and_sub(&local->active_readers, 1);
        return EBUSY;
    }

    me->last_local = local;
    lock_status = READ_STATE;
    me->lock_status = READ_STATE;
    /* printf("%s[%d]: lock_state: %d\n", __func__, cur_thread_id, me->lock_status); */
    return 0;
}

static inline int active_readers(hmcsrw_rwlock_t *impl)
{
    int i;
    for (i = 0; i < NUMA_NODES; ++i)
        if (impl->local[i].active_readers)
            return EBUSY;

    return 0;
}

int hmcsrw_rwlock_trywrlock(hmcsrw_rwlock_t *impl, hmcsrw_qnode_t *me) {
    hmcsrw_hnode_t *local = &impl->local[current_numa_node()];

    if (active_readers(impl))
        return EBUSY;

    // Must remember the last local node for release
    me->last_local = local;

    int ret = __hmcsrw_rwlock_local_trylock(local, me);
    if (ret)
        return ret;

    check_active_readers(impl);
    lock_status = WRITE_STATE;
    me->lock_status = WRITE_STATE;
    /* printf("%s[%d]: lock_state: %d\n", __func__, cur_thread_id, me->lock_status); */
    return 0;
}

static void __hmcsrw_rwlock_rdunlock(hmcsrw_hnode_t *local) {

    __sync_fetch_and_sub(&local->active_readers, 1);
}

static void __hmcsrw_rwlock_wrunlock(hmcsrw_rwlock_t *impl,
                                     hmcsrw_qnode_t *me) {
    __hmcsrw_rwlock_local_unlock(me->last_local, me);
}

int hmcsrw_rwlock_unlock(hmcsrw_rwlock_t *impl, hmcsrw_qnode_t *me) {
    if (me->lock_status == READ_STATE)
        __hmcsrw_rwlock_rdunlock(me->last_local);
    else
        __hmcsrw_rwlock_wrunlock(impl, me);
    /* printf("%s[%d]: lock_state: %d\n", __func__, cur_thread_id, me->lock_status); */
    me->lock_status = -1;
    return 0;
}

void hmcsrw_thread_start(void) {
}

void hmcsrw_thread_exit(void) {
}

void hmcsrw_application_init(void) {
}

void hmcsrw_application_exit(void) {
}

void hmcsrw_init_context(lock_mutex_t *UNUSED(impl),
                       lock_context_t *UNUSED(context), int UNUSED(number)) {
}
