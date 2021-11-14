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
#include <ttasrw.h>

#include "waiting_policy.h"
#include "interpose.h"
#include "utils.h"

extern __thread unsigned int cur_thread_id;

ttasrw_mutex_t *ttasrw_mutex_create(const pthread_mutexattr_t *attr) {
    ttasrw_mutex_t *impl =
        (ttasrw_mutex_t *)alloc_cache_align(sizeof(ttasrw_mutex_t));
    impl->spin_lock = UNLOCKED;
#if COND_VAR
    REAL(pthread_mutex_init)(&impl->posix_lock, attr);
#endif

    return impl;
}

int ttasrw_mutex_lock(ttasrw_mutex_t *impl, ttasrw_context_t *UNUSED(me)) {
    while (1) {
        while (impl->spin_lock != UNLOCKED)
            CPU_PAUSE();

        if (l_tas_uint8(&impl->spin_lock) == UNLOCKED) {
            break;
        }
    }
#if COND_VAR
    int ret = REAL(pthread_mutex_lock)(&impl->posix_lock);

    assert(ret == 0);
#endif
    return 0;
}

int ttasrw_mutex_trylock(ttasrw_mutex_t *impl, ttasrw_context_t *UNUSED(me)) {
    if (l_tas_uint8(&impl->spin_lock) == UNLOCKED) {
#if COND_VAR
        int ret = 0;
        while ((ret = REAL(pthread_mutex_trylock)(&impl->posix_lock)) == EBUSY)
            CPU_PAUSE();

        assert(ret == 0);
#endif
        return 0;
    }

    return EBUSY;
}

void __ttasrw_mutex_unlock(ttasrw_mutex_t *impl) {
    COMPILER_BARRIER();
    impl->spin_lock = UNLOCKED;
}

void ttasrw_mutex_unlock(ttasrw_mutex_t *impl, ttasrw_context_t *UNUSED(me)) {
#if COND_VAR
    int ret = REAL(pthread_mutex_unlock)(&impl->posix_lock);
    assert(ret == 0);
#endif
    __ttasrw_mutex_unlock(impl);
}

int ttasrw_mutex_destroy(ttasrw_mutex_t *lock) {
#if COND_VAR
    REAL(pthread_mutex_destroy)(&lock->posix_lock);
#endif
    free(lock);
    lock = NULL;

    return 0;
}

int ttasrw_cond_init(ttasrw_cond_t *cond, const pthread_condattr_t *attr) {
#if COND_VAR
    return REAL(pthread_cond_init)(cond, attr);
#else
    fprintf(stderr, "Error cond_var not supported.");
    assert(0);
#endif
}

int ttasrw_cond_timedwait(ttasrw_cond_t *cond, ttasrw_mutex_t *lock,
                        ttasrw_context_t *me, const struct timespec *ts) {
#if COND_VAR
    int res;

    __ttasrw_mutex_unlock(lock);

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

    ttasrw_mutex_lock(lock, me);

    return res;
#else
    fprintf(stderr, "Error cond_var not supported.");
    assert(0);
#endif
}

int ttasrw_cond_wait(ttasrw_cond_t *cond, ttasrw_mutex_t *lock, ttasrw_context_t *me) {
    return ttasrw_cond_timedwait(cond, lock, me, 0);
}

int ttasrw_cond_signal(ttasrw_cond_t *cond) {
#if COND_VAR
    return REAL(pthread_cond_signal)(cond);
#else
    fprintf(stderr, "Error cond_var not supported.");
    assert(0);
#endif
}

int ttasrw_cond_broadcast(ttasrw_cond_t *cond) {
#if COND_VAR
    return REAL(pthread_cond_broadcast)(cond);
#else
    fprintf(stderr, "Error cond_var not supported.");
    assert(0);
#endif
}

int ttasrw_cond_destroy(ttasrw_cond_t *cond) {
#if COND_VAR
    return REAL(pthread_cond_destroy)(cond);
#else
    fprintf(stderr, "Error cond_var not supported.");
    assert(0);
#endif
}



// rwlock
ttasrw_rwlock_t *ttasrw_rwlock_create(const pthread_rwlockattr_t *attr) {
    ttasrw_rwlock_t *impl =
        (ttasrw_rwlock_t *)alloc_cache_align(sizeof(ttasrw_rwlock_t));
    impl->lock_data = 0;
#if COND_VAR
    REAL(pthread_rwlock_init)(&impl->posix_lock, attr);
#endif
    MEMORY_BARRIER();
    return impl;
}

int ttasrw_rwlock_rdlock(ttasrw_rwlock_t *impl, ttasrw_context_t *UNUSED(me)) {
    while (1) {
        rw_data_t aux;
        while ((aux = impl->lock_data) > MAX_RW)
            CPU_PAUSE();

        if (__sync_val_compare_and_swap(&impl->lock_data, aux, aux+1) == aux) {
            break;
        }
    }
#if COND_VAR
    int ret = REAL(pthread_rwlock_rdlock)(&impl->posix_lock);

    assert(ret == 0);
#endif
    return 0;
}

int ttasrw_rwlock_wrlock(ttasrw_rwlock_t *impl, ttasrw_context_t *UNUSED(me)) {
    while (1) {
        while (impl->lock_data != 0)
            CPU_PAUSE();

        if (__sync_val_compare_and_swap(&impl->lock_data, 0, W_MASK) == 0) {
            break;
        }
    }
#if COND_VAR
    int ret = REAL(pthread_rwlock_wrlock)(&impl->posix_lock);

    assert(ret == 0);
#endif
    return 0;
}

int ttasrw_rwlock_tryrdlock(ttasrw_rwlock_t *impl, ttasrw_context_t *UNUSED(me)) {
    rw_data_t aux;

    if ((aux = impl->lock_data) > MAX_RW)
      return EBUSY;

    if (__sync_val_compare_and_swap(&impl->lock_data, aux, aux + 1) == aux) {
#if COND_VAR
        int ret = 0;
        while ((ret = REAL(pthread_rwlock_tryrdlock)(&impl->posix_lock)) == EBUSY)
            CPU_PAUSE();

        assert(ret == 0);
#endif
        return 0;
    }

    return EBUSY;
}

int ttasrw_rwlock_trywrlock(ttasrw_rwlock_t *impl, ttasrw_context_t *UNUSED(me)) {
    if (__sync_val_compare_and_swap(&impl->lock_data, 0, W_MASK) == 0) {
#if COND_VAR
        int ret = 0;
        while ((ret = REAL(pthread_rwlock_trywrlock)(&impl->posix_lock)) == EBUSY)
            CPU_PAUSE();

        assert(ret == 0);
#endif
        return 0;
    }

    return EBUSY;
}

void __ttasrw_rwlock_rdunlock(ttasrw_rwlock_t *impl) {
    COMPILER_BARRIER();
    __sync_fetch_and_sub(&impl->lock_data, (uint16_t)1);
}

void __ttasrw_rwlock_wrunlock(ttasrw_rwlock_t *impl) {
    COMPILER_BARRIER();
    impl->lock_data = 0;
}

int ttasrw_rwlock_unlock(ttasrw_rwlock_t *impl, ttasrw_context_t *UNUSED(me)) {
#if COND_VAR
    int ret = REAL(pthread_rwlock_unlock)(&impl->posix_lock);
    assert(ret == 0);
#endif

    if (impl->lock_data > MAX_RW)
        __ttasrw_rwlock_wrunlock(impl);
    else
        __ttasrw_rwlock_rdunlock(impl);

    return 0;
}

int ttasrw_rwlock_destroy(ttasrw_rwlock_t *lock) {
#if COND_VAR
    REAL(pthread_rwlock_destroy)(&lock->posix_lock);
#endif
    free(lock);
    lock = NULL;

    return 0;
}


void ttasrw_thread_start(void) {
}

void ttasrw_thread_exit(void) {
}

void ttasrw_application_init(void) {
}

void ttasrw_application_exit(void) {
}

void ttasrw_init_context(lock_mutex_t *UNUSED(impl),
                       lock_context_t *UNUSED(context), int UNUSED(number)) {
}
