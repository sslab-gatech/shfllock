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
 *
 *
 * John M. Mellor-Crummey and Michael L. Scott. 1991.
 * Algorithms for scalable synchronization on shared-memory multiprocessors.
 * ACM Trans. Comput. Syst. 9, 1 (February 1991).
 *
 * Lock design summary:
 * The AQSCNA lock is one of the most known multicore locks.
 * Its main goal is to avoid all threads spinning on the same memory address as
 * it induces contention due to the cache coherency protocol.
 * The lock is organized as a FIFO list: this ensures total fairness.
 * Each thread as its own context, which is a node that the thread will put into
 * the linked list (representing the list of threads waiting for the lock) when
 * it tries to grab the lock.
 * The lock is a linked-list composed of a pointer to the tail of the list.
 * - On lock: the thread does an atomic xchg to put itself to the end of the
 * linked list and get the previous tail of the list.
 *   If there was no other thread waiting, then the thread has the lock.
 * Otherwise, the thread spins on a memory address contained in its context.
 * - On unlock: if there is any thread, we just wake the next thread on the
 * waiting list. Otherwise we set the tail of the queue to NULL.
 */
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <sys/mman.h>
#include <pthread.h>
#include <assert.h>
#include <aqscna.h>

#include "waiting_policy.h"
#include "interpose.h"
#include "utils.h"

#include <assert.h>

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

#define THRESHOLD (0xff)
#ifndef UNLOCK_COUNT_THRESHOLD
#define UNLOCK_COUNT_THRESHOLD 1024
#endif

#ifdef WAITER_DEBUG
static inline void traverse_noes(aqs_mutex_t *lock, aqs_node_t *node)
{
    aqs_node_t *curr = node;
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

extern __thread unsigned int cur_thread_id;

static inline int current_numa_node() {
    unsigned long a, d, c;
    int core;
    __asm__ volatile("rdtscp" : "=a"(a), "=d"(d), "=c"(c));
    core = c & 0xFFF;
    return core / (CPU_NUMBER / NUMA_NODES);
}

/* static inline void enable_stealing(aqs_mutex_t *lock) */
/* { */
/*         atomic_andnot(_AQS_NOSTEAL_VAL, &lock->val); */
/* } */

/* static inline void disable_stealing(aqs_mutex_t *lock) */
/* { */
/*         atomic_fetch_or_acquire(_AQS_NOSTEAL_VAL, &lock->val); */
/* } */

/* static inline uint8_t is_stealing_disabled(aqs_mutex_t *lock) */
/* { */
/*         return READ_ONCE(lock->no_stealing); */
/* } */

static inline void set_sleader(struct aqs_node *node, struct aqs_node *qend)
{
        WRITE_ONCE(node->sleader, 1);
        if (qend != node)
                WRITE_ONCE(node->last_visited, qend);
}

static inline void clear_sleader(struct aqs_node *node)
{
        node->sleader = 0;
}

static inline void set_waitcount(struct aqs_node *node, int count)
{
        WRITE_ONCE(node->wcount, count);
}

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

static inline int keep_lock_local(void)
{
    return xor_random() & THRESHOLD;
    static __thread int count = 0;
    if (count++ == 100) {
	    count = 0;
	    return 0;
    }
    return 1;
}


static inline void shuffle_waiters(aqs_mutex_t *lock, aqs_node_t *node, int is_next_waiter){
    aqs_node_t *curr, *prev, *next, *last, *sleader, *qend;
    int nid = node->nid;
    int curr_locked_count = node->wcount;
    int one_shuffle = 0;
    uint32_t lock_ready;

    prev = READ_ONCE(node->last_visited);
    if (!prev)
	    prev = node;
    sleader = NULL;
    last = node;
    curr = NULL;
    next = NULL;
	qend = NULL;

    dprintf("node (%d) with sleader (%d), wcount (%d) and lock->slocked: %d\n",
            node->cid, node->sleader, node->wcount, READ_ONCE(lock->slocked));

    if (curr_locked_count == 0)
	    set_waitcount(node, ++curr_locked_count);

    clear_sleader(node);

    /* if (curr_locked_count >= 100) { */
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

        barrier();

        if (!curr) {
            sleader = last;
			qend = prev;
            break;
        }

        if (curr == READ_ONCE(lock->tail)) {
            sleader = last;
            qend = prev;
            break;
        }

        /* got the current for sure */

        /* Check if curr->nid is same as nid */
        if (curr->nid == nid) {
            if (prev->nid == nid) {
		    set_waitcount(curr, curr_locked_count);

                last = curr;
                prev = curr;
                one_shuffle = 1;
            }
            else {
                next = READ_ONCE(curr->next);
                if (!next) {
                    sleader = last;
		    qend = prev;
                    goto out;
                }

                // lstat_inc(lock_num_shuffles);
                /* WRITE_ONCE(curr->wcount, ++curr_locked_count); */
		set_waitcount(curr, curr_locked_count);

                prev->next = next;
                curr->next = last->next;
                last->next = curr;
                last = curr;
                one_shuffle = 1;
            }
        } else
            prev = curr;

        lock_ready = !READ_ONCE(lock->locked);
        if (one_shuffle && ((is_next_waiter && lock_ready) ||
			    READ_ONCE(node->lstatus))) {
            sleader = last;
	    qend = prev;
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
        /* WRITE_ONCE(sleader->sleader, 1); */
	    set_sleader(sleader, qend);
    }
}

static inline aqs_node_t *find_successor(aqs_node_t *me)
{
    aqs_node_t *next = me->next;
    aqs_node_t *secHead, *secTail, *cur;
    int my_socket = me->nid;

    if (!next)
	    return NULL;

    if (next->nid == my_socket || next->wcount)
        return next;

    secHead = next;
    secTail = next;

    cur = READ_ONCE(next->next);
    while (cur) {
        if (cur->nid == my_socket) {
            if (me->spin > 1)
                ((aqs_node_t *)(me->spin))->secTail->next = secHead;
            else
                me->spin = (uintptr_t)secHead;
            secTail->next = NULL;
            ((aqs_node_t *)(me->spin))->secTail = secTail;
            return cur;
        }
        secTail = cur;
        cur = cur->next;
    }
    return NULL;
}

/* Interpose */
aqs_mutex_t *aqs_mutex_create(const pthread_mutexattr_t *attr) {
    aqs_mutex_t *impl = (aqs_mutex_t *)alloc_cache_align(sizeof(aqs_mutex_t));
    impl->tail = NULL;
    impl->locked = 0;
#ifdef WAITER_CORRECTNESS
    impl->slocked = 0;
#endif
#if COND_VAR
    REAL(pthread_mutex_init)(&impl->posix_lock, attr);
    DEBUG("Mutex init lock=%p posix_lock=%p\n", impl, &impl->posix_lock);
#endif

    barrier();
    return impl;
}

enum {
    false,
    true,
};

static int __aqs_mutex_lock(aqs_mutex_t *impl, aqs_node_t *me) {

    if (smp_cas(&impl->locked, 0, 1) == 0) {
        dprintf("acquired in fastpath\n");
        return 0;
    }

    dprintf("acquiring in the slowpath\n");
    me->cid = cur_thread_id;
    me->next = NULL;
    me->locked = _AQ_MCS_STATUS_WAIT;
    me->spin = 0;
    me->last_visited = NULL;
    me->nid = current_numa_node();
    // lstat_inc(lock_slowpath);

    aqs_node_t *pred = smp_swap(&impl->tail, me);

    if (pred) {
        /* int flag = false; */
        dprintf("my pred is %d\n", pred->cid);
        WRITE_ONCE(pred->next, me);

        for (;;) {
            if (READ_ONCE(me->lstatus) != 0)
                break;

            if (READ_ONCE(me->sleader))
                shuffle_waiters(impl, me, 0);
            CPU_PAUSE();
        }

        dprintf("I am the very next waiter\n");
        for (;;) {
            if (!READ_ONCE(impl->locked))
                break;

            if (!READ_ONCE(me->wcount) ||
                (READ_ONCE(me->wcount) && READ_ONCE(me->sleader))) {
                /* if (!flag && smp_cas(&impl->no_stealing, 0, 1) == 0) { */
                    dprintf("shuffle waiter (%d) the VERY next waiter\n", me->cid);
                    // lstat_inc(lock_next_waiter_shuffler);
                    shuffle_waiters(impl, me, 1);
                    /* impl->no_stealing = 0; */
                    /* flag = true; */
                /* } */
            }
        }
    }
    else {
        dprintf("I have not predecessor\n");
        me->spin = 1;
    }

    dprintf("waiting for the lock to be released\n");
    for (;;) {
        if (smp_cas(&impl->locked, 0, 1) == 0)
            break;
        CPU_PAUSE();
    }

    dprintf("locked acquired\n");

    /*
     * Unlock phase here
     */
    if (!READ_ONCE(me->next)) {
        if (me->spin == 1) {
            if (smp_cas(&impl->tail, me, NULL) == me)
                goto out;
        } else {
            aqs_node_t *secHead = (aqs_node_t *)me->spin;
            if (smp_cas(&impl->tail, me, secHead->secTail) == me) {
                secHead->spin = 1;
                WRITE_ONCE(secHead->lstatus, 1);
                goto out;
            }
        }

        while (!READ_ONCE(me->next))
            CPU_PAUSE();
    }

    aqs_node_t *succ = NULL;
    /* if (!impl->no_stealing) */
    /*     succ = find_successor(me); */
    /* if (!READ_ONCE(impl->no_stealing) && smp_cas(&impl->no_stealing, 0, 1) == 0) */
        /* succ = NULL; */
    /* if (keep_lock_local()) */
    /* succ = find_successor(me); */
    if (succ) {
        WRITE_ONCE(succ->spin, me->spin);
        WRITE_ONCE(succ->lstatus, 1);
    } else if (me->spin > 1) {
        succ = (aqs_node_t *)me->spin;
        succ->secTail->next = me->next;
        WRITE_ONCE(succ->spin, 1);
        WRITE_ONCE(succ->lstatus, 1);
    } else {
        WRITE_ONCE(me->next->spin, 1);
        WRITE_ONCE(me->next->lstatus, 1);
    }

    dprintf("notifying the very next waiter (%d) to be ready\n", succ->cid);
 out:
    return 0;
}

int aqs_mutex_lock(aqs_mutex_t *impl, aqs_node_t *me) {
    int ret = __aqs_mutex_lock(impl, me);
    assert(ret == 0);
#if COND_VAR
    if (ret == 0) {
        DEBUG_PTHREAD("[%d] Lock posix=%p\n", cur_thread_id, &impl->posix_lock);
        assert(REAL(pthread_mutex_lock)(&impl->posix_lock) == 0);
    }
#endif
    DEBUG("[%d] Lock acquired posix=%p\n", cur_thread_id, &impl->posix_lock);
    return ret;
}

int aqs_mutex_trylock(aqs_mutex_t *impl, aqs_node_t *me) {
    if (smp_cas(&impl->locked, 0, 1) == 0) {
#if COND_VAR
        DEBUG_PTHREAD("[%d] Lock posix=%p\n", cur_thread_id, &impl->posix_lock);
        int ret = 0;
        while ((ret = REAL(pthread_mutex_trylock)(&impl->posix_lock)) == EBUSY)
            ;
        assert(ret == 0);
#endif
        return 0;
    }
    return EBUSY;
}

static void __aqs_mutex_unlock(aqs_mutex_t *impl, aqs_node_t *me) {
    dprintf("releasing the lock\n");

    WRITE_ONCE(impl->locked, 0);
}

void aqs_mutex_unlock(aqs_mutex_t *impl, aqs_node_t *me) {
#if COND_VAR
    DEBUG_PTHREAD("[%d] Unlock posix=%p\n", cur_thread_id, &impl->posix_lock);
    assert(REAL(pthread_mutex_unlock)(&impl->posix_lock) == 0);
#endif
    __aqs_mutex_unlock(impl, me);
}

int aqs_mutex_destroy(aqs_mutex_t *lock) {
#if COND_VAR
    REAL(pthread_mutex_destroy)(&lock->posix_lock);
#endif
    free(lock);
    lock = NULL;

    return 0;
}

int aqs_cond_init(aqs_cond_t *cond, const pthread_condattr_t *attr) {
#if COND_VAR
    return REAL(pthread_cond_init)(cond, attr);
#else
    fprintf(stderr, "Error cond_var not supported.");
    assert(0);
#endif
}

int aqs_cond_timedwait(aqs_cond_t *cond, aqs_mutex_t *lock, aqs_node_t *me,
                       const struct timespec *ts) {
#if COND_VAR
    int res;

    __aqs_mutex_unlock(lock, me);
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

    aqs_mutex_lock(lock, me);

    return res;
#else
    fprintf(stderr, "Error cond_var not supported.");
    assert(0);
#endif
}

int aqs_cond_wait(aqs_cond_t *cond, aqs_mutex_t *lock, aqs_node_t *me) {
    return aqs_cond_timedwait(cond, lock, me, 0);
}

int aqs_cond_signal(aqs_cond_t *cond) {
#if COND_VAR
    return REAL(pthread_cond_signal)(cond);
#else
    fprintf(stderr, "Error cond_var not supported.");
    assert(0);
#endif
}

int aqs_cond_broadcast(aqs_cond_t *cond) {
#if COND_VAR
    DEBUG("[%d] Broadcast cond=%p\n", cur_thread_id, cond);
    return REAL(pthread_cond_broadcast)(cond);
#else
    fprintf(stderr, "Error cond_var not supported.");
    assert(0);
#endif
}

int aqs_cond_destroy(aqs_cond_t *cond) {
#if COND_VAR
    return REAL(pthread_cond_destroy)(cond);
#else
    fprintf(stderr, "Error cond_var not supported.");
    assert(0);
#endif
}

void aqs_thread_start(void) {
}

void aqs_thread_exit(void) {
}

void aqs_application_init(void) {
}

void aqs_application_exit(void) {
}
void aqs_init_context(lock_mutex_t *UNUSED(impl),
                      lock_context_t *UNUSED(context), int UNUSED(number)) {
}
