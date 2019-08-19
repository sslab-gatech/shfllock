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
#ifndef __TTASRW_H__
#define __TTASRW_H__

#include <stdint.h>
#include "padding.h"
#define LOCK_ALGORITHM "TTASRW"
#define NEED_CONTEXT 0
#define SUPPORT_WAITING 0

typedef struct ttasrw_mutex {
    volatile uint8_t spin_lock __attribute__((aligned(L_CACHE_LINE_SIZE)));
    char __pad[pad_to_cache_line(sizeof(uint8_t))];
#if COND_VAR
    pthread_mutex_t posix_lock;
#endif
} ttasrw_mutex_t __attribute__((aligned(L_CACHE_LINE_SIZE)));

typedef pthread_cond_t ttasrw_cond_t;
typedef void *ttasrw_context_t; // Unused, take the less space as possible

// rwlock type
#define MAX_RW UINT8_MAX
#define W_MASK 0x100
typedef uint8_t rw_data_t;
typedef uint16_t all_data_t;

typedef struct ttasrw_rwlock_data {
    volatile rw_data_t read_lock;
    volatile rw_data_t write_lock;
} ttasrw_rwlock_data;

typedef struct ttasrw_rwlock {
    union {
        ttasrw_rwlock_data rw;
        volatile all_data_t lock_data;
    };
    char __pad[pad_to_cache_line(sizeof(all_data_t))];
#if COND_VAR
    pthread_rwlock_t posix_lock;
#endif
} ttasrw_rwlock_t __attribute__((aligned(L_CACHE_LINE_SIZE)));


ttasrw_mutex_t *ttasrw_mutex_create(const pthread_mutexattr_t *attr);
int ttasrw_mutex_lock(ttasrw_mutex_t *impl, ttasrw_context_t *me);
int ttasrw_mutex_trylock(ttasrw_mutex_t *impl, ttasrw_context_t *me);
void ttasrw_mutex_unlock(ttasrw_mutex_t *impl, ttasrw_context_t *me);
int ttasrw_mutex_destroy(ttasrw_mutex_t *lock);
int ttasrw_cond_init(ttasrw_cond_t *cond, const pthread_condattr_t *attr);
int ttasrw_cond_timedwait(ttasrw_cond_t *cond, ttasrw_mutex_t *lock,
                        ttasrw_context_t *me, const struct timespec *ts);
int ttasrw_cond_wait(ttasrw_cond_t *cond, ttasrw_mutex_t *lock, ttasrw_context_t *me);
int ttasrw_cond_signal(ttasrw_cond_t *cond);
int ttasrw_cond_broadcast(ttasrw_cond_t *cond);
int ttasrw_cond_destroy(ttasrw_cond_t *cond);
void ttasrw_thread_start(void);
void ttasrw_thread_exit(void);
void ttasrw_application_init(void);
void ttasrw_application_exit(void);
void ttasrw_init_context(ttasrw_mutex_t *impl, ttasrw_context_t *context, int number);

// rwlock method
ttasrw_rwlock_t *ttasrw_rwlock_create(const pthread_rwlockattr_t *attr);
int ttasrw_rwlock_rdlock(ttasrw_rwlock_t *impl, ttasrw_context_t *me);
int ttasrw_rwlock_wrlock(ttasrw_rwlock_t *impl, ttasrw_context_t *me);
int ttasrw_rwlock_tryrdlock(ttasrw_rwlock_t *impl, ttasrw_context_t *me);
int ttasrw_rwlock_trywrlock(ttasrw_rwlock_t *impl, ttasrw_context_t *me);
int ttasrw_rwlock_unlock(ttasrw_rwlock_t *impl, ttasrw_context_t *me);
int ttasrw_rwlock_destroy(ttasrw_rwlock_t *lock);



typedef ttasrw_mutex_t lock_mutex_t;
typedef ttasrw_context_t lock_context_t;
typedef ttasrw_cond_t lock_cond_t;
typedef ttasrw_rwlock_t lock_rwlock_t;

// Define library function ptr
#define lock_mutex_create ttasrw_mutex_create
#define lock_mutex_lock ttasrw_mutex_lock
#define lock_mutex_trylock ttasrw_mutex_trylock
#define lock_mutex_unlock ttasrw_mutex_unlock
#define lock_mutex_destroy ttasrw_mutex_destroy
#define lock_cond_init ttasrw_cond_init
#define lock_cond_timedwait ttasrw_cond_timedwait
#define lock_cond_wait ttasrw_cond_wait
#define lock_cond_signal ttasrw_cond_signal
#define lock_cond_broadcast ttasrw_cond_broadcast
#define lock_cond_destroy ttasrw_cond_destroy
#define lock_thread_start ttasrw_thread_start
#define lock_thread_exit ttasrw_thread_exit
#define lock_application_init ttasrw_application_init
#define lock_application_exit ttasrw_application_exit
#define lock_init_context ttasrw_init_context

// rwlock method define
#define lock_rwlock_create ttasrw_rwlock_create
#define lock_rwlock_rdlock ttasrw_rwlock_rdlock
#define lock_rwlock_wrlock ttasrw_rwlock_wrlock
#define lock_rwlock_tryrdlock ttasrw_rwlock_tryrdlock
#define lock_rwlock_trywrlock ttasrw_rwlock_trywrlock
#define lock_rwlock_unlock ttasrw_rwlock_unlock
#define lock_rwlock_destroy ttasrw_rwlock_destroy


#endif // __TTASRW_H__
