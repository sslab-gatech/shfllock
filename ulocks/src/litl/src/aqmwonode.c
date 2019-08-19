/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2016 Hugo Guiroux <hugo.guiroux at gmail dot com>
 *               UPMC, 2010-2011, Jean-Pierre Lozi <jean-pierre.lozi@lip6.fr>
 *                                Gaël Thomas <gael.thomas@lip6.fr>
 *                                Florian David <florian.david@lip6.fr>
 *                                Julia Lawall <julia.lawall@lip6.fr>
 *                                Gilles Muller <gilles.muller@lip6.fr>
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
 */
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <sys/mman.h>
#include <pthread.h>
#include <assert.h>
#include <aqmwonode.h>

#include "waiting_policy.h"
#include "interpose.h"
#include "utils.h"

#include <assert.h>

enum bool {
    false,
    true
};

/* debugging */
#ifdef WAITER_DEBUG
typedef enum {
    RED,
    GREEN,
    BLUE,
    MAGENTA,
    YELLOW,
    CYAN,
    END,
} color_num;

static char colors[END][8] = {
    "\x1B[31m",
    "\x1B[32m",
    "\x1B[34m",
    "\x1B[35m",
    "\x1b[33m",
    "\x1b[36m",
};
static unsigned long counter = 0;

#define dprintf(__fmt, ...)                                                    \
    do {                                                                       \
        smp_faa(&counter, 1);                                                  \
        fprintf(stderr, "%s [DBG:%010lu: %d (%s: %d)]: " __fmt,                \
                colors[__my_cpu_id % END], counter, __my_cpu_id,                         \
                __func__, __LINE__, ##__VA_ARGS__);                            \
    } while(0);
#define dassert(v)      assert((v))
#else
#define dprintf(__fmt, ...) do { } while(0)
#define dassert(v) do { } while(0)
#endif

/* debugging */
#ifdef WAITER_DEBUG
static inline void traverse_nodes(aqm_mutex_t *lock, aqm_node_t *node)
{
    aqm_node_t *curr = node;
    int count = 0;
    printf("my prev: ");
    if (node->prev_shuffler)
        printf("%d\n", node->prev_shuffler->cid);
    else
        printf("no one!\n");
    printf("#coreid[lock-status:shuffle-leader:waiter-count]\n");
    for (;;) {
        if (++count > 200)
            assert(0);
        if (!curr)
            break;

        printf("%d[%d:%d:%d]->", curr->cid, curr->lstatus,
               curr->sleader, curr->wcount);
        curr = READ_ONCE(curr->next);
    }
    printf("\n");
}
#endif

static inline void print_node_state(const char *str, struct aqm_node *node)
{
	return;
    switch(node->lstatus) {
    case _AQ_MCS_STATUS_PWAIT:
        printf("[%s] node(%d)->lstatus: PWAIT\n", str, node->cid);
        break;
    case _AQ_MCS_STATUS_PARKED:
        printf("[%s] node(%d)->lstatus: PARKED\n", str, node->cid);
        break;
    case _AQ_MCS_STATUS_LOCKED:
        printf("[%s] node(%d)->lstatus: LOCKED\n", str, node->cid);
        break;
    case _AQ_MCS_STATUS_UNPWAIT:
        printf("[%s] node(%d)->lstatus: UNPWAIT\n", str, node->cid);
        break;
    }
}

extern __thread unsigned int cur_thread_id;

#define THRESHOLD (0xffff)
#ifndef UNLOCK_COUNT_THRESHOLD
#define UNLOCK_COUNT_THRESHOLD 1024
#endif

static inline uint32_t xor_random() {
    static __thread uint32_t rv = 0;

    if (rv == 0)
        rv = cur_thread_id + 1;

    uint32_t v = rv;
    v ^= v << 6;
    v ^= (uint32_t)(v) >> 21;
    v ^= v << 7;
    rv = v;

    return v & (UNLOCK_COUNT_THRESHOLD - 1);
}

static int keep_lock_local(void)
{
    return xor_random() & THRESHOLD;
}

static inline int current_numa_node() {
    unsigned long a, d, c;
    int core;
    __asm__ volatile("rdtscp" : "=a"(a), "=d"(d), "=c"(c));
    core = c & 0xFFF;
    return core / (CPU_NUMBER / NUMA_NODES);
}

static inline void enable_stealing(aqm_mutex_t *lock)
{
    smp_swap(&lock->no_stealing, 1);
}

static inline void disable_stealing(aqm_mutex_t *lock)
{
    smp_swap(&lock->no_stealing, 0);
}

static inline int is_stealing_disabled(aqm_mutex_t *lock)
{
    return READ_ONCE(lock->no_stealing);
}

static inline void __waiting_policy_wake(volatile int *var) {
    *var    = 1;
    int ret = sys_futex((int *)var, FUTEX_WAKE_PRIVATE, UNLOCKED, NULL, 0, 0);
    if (ret == -1) {
        perror("Unable to futex wake");
        exit(-1);
    }
}

static inline void __waiting_policy_sleep(volatile int *var) {
    if (*var == 1)
        return;

    int ret = 0;
    while ((ret = sys_futex((int *)var, FUTEX_WAIT_PRIVATE, LOCKED, NULL, 0,
                            0)) != 0) {
        if (ret == -1 && errno != EINTR) {
            /**
             * futex returns EAGAIN if *var is not 0 anymore.
             * This can happen when the value of *var is changed by another
             *thread after the spinning loop.
             * Note: FUTEX_WAIT_PRIVATE acts like an atomic operation.
             **/
            if (errno == EAGAIN) {
                DEBUG("[-1] Race\n");
                break;
            }
            perror("Unable to futex wait");
            exit(-1);
        }
    }

    /**
     * *var is not always 1 immediately when the thread wakes up
     * (but eventually it is).
     * Maybe related to memory reordering?
     **/
    while (*var != 1)
        CPU_PAUSE();
}


static inline void __wakeup_waiter(aqm_node_t *node)
{
	__waiting_policy_wake((volatile int *)&node->pstate);
}

static inline int force_update_node(aqm_node_t *node, uint8_t state)
{
    if ((smp_cas(&node->lstatus, _AQ_MCS_STATUS_PARKED, state)
         == _AQ_MCS_STATUS_PARKED)) { // ||
        /*
         * Ouch, we need to explicitly wake up the guy
         */
        __wakeup_waiter(node);
        return true;
    }
    return false;
}

static inline int park_waiter(struct aqm_node *node)
{
    if (smp_cas(&node->lstatus, _AQ_MCS_STATUS_PWAIT,
                _AQ_MCS_STATUS_PARKED) != _AQ_MCS_STATUS_PWAIT)
        goto out_acquired;

    __waiting_policy_sleep((volatile int *)&node->pstate);

 out_acquired:
    return 1;
}

static void shuffle_waiters(aqm_mutex_t *lock, aqm_node_t *node, int is_next_waiter){
    aqm_node_t *curr, *prev, *next, *last, *sleader;
    int nid = node->nid;
    int curr_locked_count = node->wcount;
    int one_shuffle = 0;
    uint32_t lock_ready;

    sleader = NULL;
    prev = node;
    last = node;
    curr = NULL;
    next = NULL;

    dprintf("node (%d) with sleader (%d), wcount (%d) and lock->slocked: %d\n",
            node->cid, node->sleader, node->wcount, READ_ONCE(lock->slocked));

#ifdef WAITER_CORRECTNESS
    if (smp_cas(&lock->slocked, 0, 1) != 0) {
        traverse_nodes(lock, node);
        assert(0);
        return;
    } else {
        WRITE_ONCE(lock->shuffler, node);
    }
#endif

    if (curr_locked_count == 0) {
        // lstat_inc(lock_num_shuffles);
        WRITE_ONCE(node->wcount, ++curr_locked_count);
        dprintf("increaing wcount of %d to %d\n", node->cid, curr_locked_count);
    }

    dprintf("clearing node %d sleader value\n", node->cid);
    WRITE_ONCE(node->sleader, 0);

    /* if (curr_locked_count >= _AQ_MAX_LOCK_COUNT) { */
    /*     sleader = READ_ONCE(node->next); */
    /*     dprintf("1. selecting new shuffler %d\n", sleader->cid); */
    /*     goto out; */
    /* } */

    if (!keep_lock_local()) {
	sleader = READ_ONCE(node->next);
	goto out;
    }

    for (;;) {
        curr = READ_ONCE(prev->next);

        if (!curr) {
            sleader = last;
            break;
        }

        if (curr == READ_ONCE(lock->tail)) {
            sleader = last;
            break;
        }

        /* got the current for sure */

        /* Check if curr->nid is same as nid */
        if (curr->nid == nid) {
            if (prev->nid == nid) {
                // lstat_inc(lock_num_shuffles);
                print_node_state("before", curr);
                force_update_node(curr, _AQ_MCS_STATUS_UNPWAIT);
                print_node_state("after", curr);
                WRITE_ONCE(curr->wcount, curr_locked_count);
                last = curr;
                prev = curr;
                one_shuffle = 1;
            }
            else {
                next = READ_ONCE(curr->next);
                if (!next) {
                    sleader = last;
                    goto out;
                }

                // lstat_inc(lock_num_shuffles);
                print_node_state("start marking shuffled", curr);
                print_node_state("before", curr);
                force_update_node(curr, _AQ_MCS_STATUS_UNPWAIT);
                print_node_state("after", curr);
                print_node_state("end marking shuffled", curr);
                WRITE_ONCE(curr->wcount, curr_locked_count);
                prev->next = next;
                curr->next = last->next;
                last->next = curr;
                last = curr;
                one_shuffle = 1;
            }
        } else
            prev = curr;

        lock_ready = !READ_ONCE(lock->locked);
        if (one_shuffle && is_next_waiter && lock_ready) {
            sleader = last;
            break;
        }
    }

    out:
#ifdef WAITER_CORRECTNESS
    smp_swap(&lock->slocked, 0);
    WRITE_ONCE(lock->shuffler,  NULL);
#endif

    dprintf("time to go out for me (%d)!\n", node->cid);
    if (sleader) {
        WRITE_ONCE(sleader->sleader, 1);
    }
}

/* Interpose */
aqm_mutex_t *aqm_mutex_create(const pthread_mutexattr_t *attr) {
    aqm_mutex_t *lock = (aqm_mutex_t *)alloc_cache_align(sizeof(aqm_mutex_t));
    lock->tail = NULL;
    lock->val = 0;
#ifdef WAITER_CORRECTNESS
    lock->slocked = 0;
#endif
#if COND_VAR
    REAL(pthread_mutex_init)(&lock->posix_lock, attr);
    DEBUG("Mutex init lock=%p posix_lock=%p\n", lock, &lock->posix_lock);
#endif

    barrier();
    return lock;
}

static int __aqm_mutex_lock(aqm_mutex_t *lock, aqm_node_t *node) {

    uint8_t prev_lstatus;

    if (smp_cas(&lock->locked_no_stealing, 0, 1) == 0) {
        // lstat_inc(lock_fastpath);
        dprintf("acquired in fastpath\n");
        return 0;
    }

    dprintf("acquiring in the slowpath\n");
    node->cid = cur_thread_id;
    node->next = NULL;
    node->last_visited = NULL;
    node->locked = _AQ_MCS_STATUS_PWAIT;
    node->nid = current_numa_node();
	node->pstate = 0;

    aqm_node_t *pred = smp_swap(&lock->tail, node);
    aqm_node_t *succ = NULL;

    if (pred) {
        int i;
        int should_park = false;
        int very_next_waiter = false;

        dprintf("my pred is %d\n", pred->cid);

        WRITE_ONCE(pred->next, node);

     retry:
        for (i = 0; i < SPINNING_THRESHOLD; ++i) {
            uint32_t val = READ_ONCE(node->locked);

            if (_AQ_MCS_LOCKED_VAL(val) == _AQ_MCS_STATUS_LOCKED) {
                very_next_waiter = true;
                break;
            }

            if (_AQ_MCS_SLEADER_VAL(val) == 1) {
                dprintf("shuffle waiter (%d) NOT very next waiter\n", node->cid);
                // lstat_inc(lock_waiter_shuffler);
                should_park = true;
                shuffle_waiters(lock, node, 0);
            }
            CPU_PAUSE();
        }

        if (!should_park) {
            park_waiter(node);
        }

        if (!very_next_waiter)
            goto retry;

        dprintf("I am the very next lock waiter\n");
        for (;;) {
            uint32_t val = READ_ONCE(node->locked);

            if (!READ_ONCE(lock->locked))
                break;

            if (!_AQ_MCS_WCOUNT_VAL(val) ||
                (_AQ_MCS_WCOUNT_VAL(val) && _AQ_MCS_SLEADER_VAL(val))) {
                dprintf("shuffle waiter (%d) is the very next lock waiter\n",
                        node->cid);
                shuffle_waiters(lock, node, 1);
            }
        }
    }

    dprintf("waiting for the lock to be released\n");
    for (;;) {

        if (is_stealing_disabled(lock))
            disable_stealing(lock);

        while (READ_ONCE(lock->locked))
            CPU_PAUSE();

        if (smp_cas(&lock->locked, 0, 1) == 0)
            break;

        CPU_PAUSE();
    }

    dprintf("locked acquired\n");
    succ = READ_ONCE(node->next);
    if (!succ) {
        if (smp_cas(&lock->tail, node, NULL) == node) {
            enable_stealing(lock);
            dprintf("I was the last one in the queue\n");
            return 0;
        }

        for (;;) {
            succ = READ_ONCE(node->next);
            if (succ)
                break;
            CPU_PAUSE();
        }
    }

    dprintf("notifying the very next waiter (%d) to be ready\n", succ->cid);
    /* print_node_state("before", succ); */
    /* WRITE_ONCE(succ->lstatus, _AQ_MCS_STATUS_LOCKED); */
    prev_lstatus = smp_swap(&succ->lstatus, _AQ_MCS_STATUS_LOCKED);
    if (prev_lstatus == _AQ_MCS_STATUS_PARKED) {
        __wakeup_waiter(succ);
    }
    /* smp_cas(&succ->lstatus, _AQ_MCS_STATUS_UNPWAIT, _AQ_MCS_STATUS_LOCKED); */
    /* print_node_state("after", succ); */
    /* force_update_node(succ); */
    return 0;
}

int aqm_mutex_lock(aqm_mutex_t *lock, aqm_node_t *UNUSED(me)) {
    aqm_node_t node;
    int ret = __aqm_mutex_lock(lock, &node);
    assert(ret == 0);
#if COND_VAR
    if (ret == 0) {
        DEBUG_PTHREAD("[%d] Lock posix=%p\n", cur_thread_id, &lock->posix_lock);
        assert(REAL(pthread_mutex_lock)(&lock->posix_lock) == 0);
    }
#endif
    DEBUG("[%d] Lock acquired posix=%p\n", cur_thread_id, &lock->posix_lock);
    return ret;
}

int aqm_mutex_trylock(aqm_mutex_t *lock, aqm_node_t *me) {
    if (smp_cas(&lock->val, 0, 1) == 0) {
#if COND_VAR
        DEBUG_PTHREAD("[%d] Lock posix=%p\n", cur_thread_id, &lock->posix_lock);
        int ret = 0;
        while ((ret = REAL(pthread_mutex_trylock)(&lock->posix_lock)) == EBUSY)
            ;
        assert(ret == 0);
#endif
        return 0;
    }
    return EBUSY;
}

static void __aqm_mutex_unlock(aqm_mutex_t *lock) {
    dprintf("releasing the lock\n");
    WRITE_ONCE(lock->locked, 0);
}

void aqm_mutex_unlock(aqm_mutex_t *lock, aqm_node_t *UNUSED(me)) {
#if COND_VAR
    DEBUG_PTHREAD("[%d] Unlock posix=%p\n", cur_thread_id, &lock->posix_lock);
    assert(REAL(pthread_mutex_unlock)(&lock->posix_lock) == 0);
#endif
    __aqm_mutex_unlock(lock);
}

int aqm_mutex_destroy(aqm_mutex_t *lock) {
#if COND_VAR
    REAL(pthread_mutex_destroy)(&lock->posix_lock);
#endif
    free(lock);
    lock = NULL;

    return 0;
}

int aqm_cond_init(aqm_cond_t *cond, const pthread_condattr_t *attr) {
#if COND_VAR
    return REAL(pthread_cond_init)(cond, attr);
#else
    fprintf(stderr, "Error cond_var not supported.");
    assert(0);
#endif
}

int aqm_cond_timedwait(aqm_cond_t *cond, aqm_mutex_t *lock,
                       aqm_node_t *UNUSED(me),
                       const struct timespec *ts) {
#if COND_VAR
    int res;
    aqm_node_t node;

    __aqm_mutex_unlock(lock);
    DEBUG("[%d] Sleep cond=%p lock=%p posix_lock=%p\n", cur_thread_id, cond,
          lock, &(lock->posix_lock));
    DEBUG_PTHREAD("[%d] Cond posix = %p lock = %p\n", cur_thread_id, cond,
                  &lock->posix_lock);

    if (ts)
        res = REAL(pthread_cond_timedwait)(cond, &lock->posix_lock, ts);
    else
        res = REAL(pthread_cond_wait)(cond, &lock->posix_lock);

    if (res != 0 && res != ETIMEDOUT) {
        fprintf(stderr, "Error on cond_{timed,}wait %d\n", res);
        assert(0);
    }

    int ret = 0;
    if ((ret = REAL(pthread_mutex_unlock)(&lock->posix_lock)) != 0) {
        fprintf(stderr, "Error on mutex_unlock %d\n", ret == EPERM);
        assert(0);
    }

    aqm_mutex_lock(lock, &node);

    return res;
#else
    fprintf(stderr, "Error cond_var not supported.");
    assert(0);
#endif
}

int aqm_cond_wait(aqm_cond_t *cond, aqm_mutex_t *lock, aqm_node_t *me) {
    return aqm_cond_timedwait(cond, lock, me, 0);
}

int aqm_cond_signal(aqm_cond_t *cond) {
#if COND_VAR
    return REAL(pthread_cond_signal)(cond);
#else
    fprintf(stderr, "Error cond_var not supported.");
    assert(0);
#endif
}

int aqm_cond_broadcast(aqm_cond_t *cond) {
#if COND_VAR
    DEBUG("[%d] Broadcast cond=%p\n", cur_thread_id, cond);
    return REAL(pthread_cond_broadcast)(cond);
#else
    fprintf(stderr, "Error cond_var not supported.");
    assert(0);
#endif
}

int aqm_cond_destroy(aqm_cond_t *cond) {
#if COND_VAR
    return REAL(pthread_cond_destroy)(cond);
#else
    fprintf(stderr, "Error cond_var not supported.");
    assert(0);
#endif
}

void aqm_thread_start(void) {
}

void aqm_thread_exit(void) {
}

void aqm_application_init(void) {
}

void aqm_application_exit(void) {
}
void aqm_init_context(lock_mutex_t *UNUSED(lock),
                      lock_context_t *UNUSED(context), int UNUSED(number)) {
}
