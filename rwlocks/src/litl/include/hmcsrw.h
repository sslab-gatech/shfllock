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
 */
#ifndef __HMCSRW_H__
#define __HMCSRW_H__

#include <stdint.h>
#include "padding.h"
#define LOCK_ALGORITHM "HMCSRW"
#define NEED_CONTEXT 1
#define SUPPORT_WAITING 0

#define ____cacheline_aligned  __attribute__ ((aligned (L_CACHE_LINE_SIZE)))
#define RELEASE_THRESHOLD 100 // Same as cohort for comparison

struct hmcsrw_hnode;
typedef struct hmcsrw_qnode {
    struct hmcsrw_qnode *volatile next;
    char __pad[pad_to_cache_line(sizeof(struct hmcsrw_qnode *))];
    volatile uint64_t status __attribute__((aligned(L_CACHE_LINE_SIZE)));
    char __pad2[pad_to_cache_line(sizeof(uint64_t))];

    struct hmcsrw_hnode *last_local;
    int lock_status ____cacheline_aligned;
} hmcsrw_qnode_t __attribute__((aligned(L_CACHE_LINE_SIZE)));

typedef struct hmcsrw_hnode {
    struct hmcsrw_hnode *parent __attribute__((aligned(L_CACHE_LINE_SIZE)));
    struct hmcsrw_qnode *volatile tail;
    char __pad[pad_to_cache_line(sizeof(struct hmcsrw_qnode *) +
                                 sizeof(struct hmcsrw_hnode *))];
    hmcsrw_qnode_t node ____cacheline_aligned;

    volatile uint32_t active_readers ____cacheline_aligned;
} hmcsrw_hnode_t __attribute__((aligned(L_CACHE_LINE_SIZE)));

typedef pthread_cond_t hmcsrw_cond_t;

// rwlock type
typedef struct hmcsrw_rwlock {
    hmcsrw_hnode_t global;
    hmcsrw_hnode_t local[NUMA_NODES];
} hmcsrw_rwlock_t __attribute__((aligned(L_CACHE_LINE_SIZE)));


hmcsrw_rwlock_t *hmcsrw_mutex_create(const pthread_mutexattr_t *attr);
int hmcsrw_rwlock_lock(hmcsrw_rwlock_t *impl, hmcsrw_qnode_t *me);
int hmcsrw_rwlock_trylock(hmcsrw_rwlock_t *impl, hmcsrw_qnode_t *me);
int hmcsrw_rwlock_destroy(hmcsrw_rwlock_t *lock);
int hmcsrw_cond_init(hmcsrw_cond_t *cond, const pthread_condattr_t *attr);
int hmcsrw_cond_timedwait(hmcsrw_cond_t *cond, hmcsrw_rwlock_t *lock,
                        hmcsrw_qnode_t *me, const struct timespec *ts);
int hmcsrw_cond_wait(hmcsrw_cond_t *cond, hmcsrw_rwlock_t *lock, hmcsrw_qnode_t *me);
int hmcsrw_cond_signal(hmcsrw_cond_t *cond);
int hmcsrw_cond_broadcast(hmcsrw_cond_t *cond);
int hmcsrw_cond_destroy(hmcsrw_cond_t *cond);
void hmcsrw_thread_start(void);
void hmcsrw_thread_exit(void);
void hmcsrw_application_init(void);
void hmcsrw_application_exit(void);
void hmcsrw_init_context(hmcsrw_rwlock_t *impl, hmcsrw_qnode_t *context, int number);

// rwlock method
hmcsrw_rwlock_t *hmcsrw_rwlock_create(const pthread_rwlockattr_t *attr);
int hmcsrw_rwlock_rdlock(hmcsrw_rwlock_t *impl, hmcsrw_qnode_t *me);
int hmcsrw_rwlock_wrlock(hmcsrw_rwlock_t *impl, hmcsrw_qnode_t *me);
int hmcsrw_rwlock_tryrdlock(hmcsrw_rwlock_t *impl, hmcsrw_qnode_t *me);
int hmcsrw_rwlock_trywrlock(hmcsrw_rwlock_t *impl, hmcsrw_qnode_t *me);
int hmcsrw_rwlock_unlock(hmcsrw_rwlock_t *impl, hmcsrw_qnode_t *me);
int hmcsrw_rwlock_destroy(hmcsrw_rwlock_t *lock);

typedef hmcsrw_rwlock_t lock_mutex_t;
typedef hmcsrw_qnode_t lock_context_t;
typedef hmcsrw_cond_t lock_cond_t;
typedef hmcsrw_rwlock_t lock_rwlock_t;

// Define library function ptr
#define lock_mutex_create hmcsrw_mutex_create
#define lock_mutex_lock hmcsrw_rwlock_lock
#define lock_mutex_trylock hmcsrw_rwlock_trylock
#define lock_mutex_unlock hmcsrw_rwlock_unlock
#define lock_mutex_destroy hmcsrw_rwlock_destroy
#define lock_cond_init hmcsrw_cond_init
#define lock_cond_timedwait hmcsrw_cond_timedwait
#define lock_cond_wait hmcsrw_cond_wait
#define lock_cond_signal hmcsrw_cond_signal
#define lock_cond_broadcast hmcsrw_cond_broadcast
#define lock_cond_destroy hmcsrw_cond_destroy
#define lock_thread_start hmcsrw_thread_start
#define lock_thread_exit hmcsrw_thread_exit
#define lock_application_init hmcsrw_application_init
#define lock_application_exit hmcsrw_application_exit
#define lock_init_context hmcsrw_init_context

// rwlock method define
#define lock_rwlock_create hmcsrw_rwlock_create
#define lock_rwlock_rdlock hmcsrw_rwlock_rdlock
#define lock_rwlock_wrlock hmcsrw_rwlock_wrlock
#define lock_rwlock_tryrdlock hmcsrw_rwlock_tryrdlock
#define lock_rwlock_trywrlock hmcsrw_rwlock_trywrlock
#define lock_rwlock_unlock hmcsrw_rwlock_unlock
#define lock_rwlock_destroy hmcsrw_rwlock_destroy


#endif // __HMCSRW_H__
