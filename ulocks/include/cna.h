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
#ifndef __CNA_H__
#define __CNA_H__

#include "padding.h"
#define LOCK_ALGORITHM "CNA"
#define NEED_CONTEXT 1
#define SUPPORT_WAITING 1

typedef struct cna_node {
    struct cna_node *volatile next;
    /* char __pad[pad_to_cache_line(sizeof(struct cna_node *))]; */

    struct cna_node *volatile secTail;
    /* char __pad2[pad_to_cache_line(sizeof(struct cna_node *))]; */

    volatile uintptr_t spin __attribute__((aligned(L_CACHE_LINE_SIZE)));

    int socket;
} cna_node_t __attribute__((aligned(L_CACHE_LINE_SIZE)));

typedef struct cna_mutex {
#if COND_VAR
    pthread_mutex_t posix_lock;
    char __pad[pad_to_cache_line(sizeof(pthread_mutex_t))];
#endif
    /* struct cna_node *volatile tail __attribute__((aligned(L_CACHE_LINE_SIZE))); */
    struct cna_node *volatile tail;
} cna_mutex_t __attribute__((aligned(L_CACHE_LINE_SIZE)));

typedef pthread_cond_t cna_cond_t;
cna_mutex_t *cna_mutex_create(const pthread_mutexattr_t *attr);
int cna_mutex_lock(cna_mutex_t *impl, cna_node_t *me);
int cna_mutex_trylock(cna_mutex_t *impl, cna_node_t *me);
void cna_mutex_unlock(cna_mutex_t *impl, cna_node_t *me);
int cna_mutex_destroy(cna_mutex_t *lock);
int cna_cond_init(cna_cond_t *cond, const pthread_condattr_t *attr);
int cna_cond_timedwait(cna_cond_t *cond, cna_mutex_t *lock, cna_node_t *me,
                       const struct timespec *ts);
int cna_cond_wait(cna_cond_t *cond, cna_mutex_t *lock, cna_node_t *me);
int cna_cond_signal(cna_cond_t *cond);
int cna_cond_broadcast(cna_cond_t *cond);
int cna_cond_destroy(cna_cond_t *cond);
void cna_thread_start(void);
void cna_thread_exit(void);
void cna_application_init(void);
void cna_application_exit(void);
void cna_init_context(cna_mutex_t *impl, cna_node_t *context, int number);

typedef cna_mutex_t lock_mutex_t;
typedef cna_node_t lock_context_t;
typedef cna_cond_t lock_cond_t;

#define lock_mutex_create cna_mutex_create
#define lock_mutex_lock cna_mutex_lock
#define lock_mutex_trylock cna_mutex_trylock
#define lock_mutex_unlock cna_mutex_unlock
#define lock_mutex_destroy cna_mutex_destroy
#define lock_cond_init cna_cond_init
#define lock_cond_timedwait cna_cond_timedwait
#define lock_cond_wait cna_cond_wait
#define lock_cond_signal cna_cond_signal
#define lock_cond_broadcast cna_cond_broadcast
#define lock_cond_destroy cna_cond_destroy
#define lock_thread_start cna_thread_start
#define lock_thread_exit cna_thread_exit
#define lock_application_init cna_application_init
#define lock_application_exit cna_application_exit
#define lock_init_context cna_init_context

#endif // __CNA_H__
