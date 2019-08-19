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
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdio.h>
#include <signal.h>
#include <pthread.h>
#include <dlfcn.h>
#include <assert.h>
#include <atomic_ops.h>
#include <clht.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#ifdef MCS
#include <mcs.h>
#elif defined(CNA)
#include <cna.h>
#elif defined(CNAF)
#include <cnaf.h>
#elif defined(AQS)
#include <aqs.h>
#elif defined(AQSWONODE)
#include <aqswonode.h>
#elif defined(AQSF)
#include <aqsf.h>
#elif defined(AQM)
#include <aqm.h>
#elif defined(AQMWONODE)
#include <aqmwonode.h>
#elif defined(MCSTP)
#include <mcstp.h>
#elif defined(SPINLOCK)
#include <spinlock.h>
#elif defined(MALTHUSIAN)
#include <malthusian.h>
#elif defined(MALTHUSIANF)
#include <malthusianf.h>
#elif defined(TTAS)
#include <ttas.h>
#elif defined(TICKET)
#include <ticket.h>
#elif defined(CLH)
#include <clh.h>
#elif defined(BACKOFF)
#include <backoff.h>
#elif defined(PTHREADCACHEALIGNED)
#include <pthreadcachealigned.h>
#elif defined(PTHREADINTERPOSE)
#include <pthreadinterpose.h>
#elif defined(PTHREADADAPTIVE)
#include <pthreadadaptive.h>
#elif defined(EMPTY)
#include <empty.h>
#elif defined(CONCURRENCY)
#include <concurrency.h>
#elif defined(MCSEPFL)
#include <mcsepfl.h>
#elif defined(SPINLOCKEPFL)
#include <spinlockepfl.h>
#elif defined(TTASEPFL)
#include <ttasepfl.h>
#elif defined(TICKETEPFL)
#include <ticketepfl.h>
#elif defined(CLHEPFL)
#include <clhepfl.h>
#elif defined(HTLOCKEPFL)
#include <htlockepfl.h>
#elif defined(ALOCKEPFL)
#include <alockepfl.h>
#elif defined(HMCS)
#include <hmcs.h>
#elif defined(HYSHMCS)
#include <hyshmcs.h>
#elif defined(CBOMCS)
#include <cbomcs.h>
#elif defined(CPTLTKT)
#include <cptltkt.h>
#elif defined(CTKTTKT)
#include <ctkttkt.h>
#elif defined(PARTITIONED)
#include <partitioned.h>
#elif defined(MUTEXEE)
#include <mutexee.h>
#elif defined(TTASRW)
#include <ttasrw.h>
#elif defined(HMCSRW)
#include <hmcsrw.h>
#elif defined(AQS)
#include <aqs.h>
#elif defined(AQSCNA)
#include <aqscna.h>
#elif defined(AQSCNAF)
#include <aqscnaf.h>
#else
#error "No lock algorithm known"
#endif

#include "waiting_policy.h"
#include "utils.h"
#include "interpose.h"
#include <string.h>

// The NO_INDIRECTION flag allows disabling the pthread-to-lock hash table
// and directly calling the specific lock function
// See empty.c for example.

unsigned int last_thread_id;
__thread unsigned int cur_thread_id;
__thread struct t_info tinfo;
__thread uint8_t lock_level;

unsigned long wake_up_lkholder[MAX_THREADS] = {0};
unsigned long wake_up_shuffler[MAX_THREADS] = {0};
unsigned long node_st_shuffler[MAX_THREADS] = {0};
#if defined(HMCSRW)
__thread unsigned int lock_status;
#endif

/* #define ACCOUNTING 1 */

#define BEFORE_ENTER_CS 1
#define AFTER_ENTER_CS 	2
#define BEFORE_EXIT_CS 	3
#define AFTER_EXIT_CS   4

#define PHASE_LOCK 	    1
#define PHASE_TRYLOCK 	2
#define PHASE_UNLOCK 	3
#define PHASE_RD_LOCK   4
#define PHASE_RD_TRYLOCK 5
#define PHASE_RD_UNLOCK 6

#define MAX_COUNT (1 << 25)

struct cstime {
    uint8_t phase;
    uint8_t csphase;
    uint8_t level;
    uint8_t active;
    unsigned long logtime;
    unsigned long addr;
};

typedef struct acq_rel_info {
    int done;
    unsigned long count;
    struct cstime *cstime;
} linfo_t;

#if !NO_INDIRECTION
#if ACCOUNTING
linfo_t tlinfo[MAX_THREADS];
int logging_done = 0;
int active[MAX_THREADS] = { 0};
#endif
#endif

#if !NO_INDIRECTION
typedef struct {
    lock_mutex_t *lock_lock;
    char __pad[pad_to_cache_line(sizeof(lock_mutex_t *))];
#if NEED_CONTEXT
    lock_context_t *lock_node;
    /* lock_context_t lock_node[MAX_THREADS]; */
#endif
} lock_transparent_mutex_t;

// pthread-to-lock htable (using CLHT)
static clht_t *pthread_to_lock;
#endif

struct routine {
    void *(*fct)(void *);
    void *arg;
};

// With this flag enabled, the mutex_destroy function will be called on each
// alive lock
// at application exit (e.g., for printing statistics about a lock -- see
// src/concurrency.c)
#ifndef DESTROY_ON_EXIT
#define DESTROY_ON_EXIT 0
#endif

// With this flag enabled, SIGINT and SIGTERM are caught to call the destructor
// of the library (see interpose_exit below)
#ifndef CLEANUP_ON_SIGNAL
#define CLEANUP_ON_SIGNAL 0
#endif

#if !NO_INDIRECTION
static lock_transparent_mutex_t *
ht_lock_create(pthread_mutex_t *mutex, const pthread_mutexattr_t *attr) {
    lock_transparent_mutex_t *impl = alloc_cache_align(sizeof *impl);
    impl->lock_lock                = lock_mutex_create(attr);
#if NEED_CONTEXT
    impl->lock_node = alloc_cache_align(MAX_THREADS * sizeof(lock_context_t));
    memset(impl->lock_node, 0, MAX_THREADS * sizeof(lock_context_t));
    lock_init_context(impl->lock_lock, impl->lock_node, MAX_THREADS);
#endif

    // If a lock is initialized statically and two threads acquire the locks at
    // the same time, then only one call to clht_put will succeed.
    // For the failing thread, we free the previously allocated mutex data
    // structure and do a lookup to retrieve the ones inserted by the successful
    // thread.
    if (clht_put(pthread_to_lock, (clht_addr_t)mutex, (clht_val_t)impl) == 0) {
        free(impl);
        return (lock_transparent_mutex_t *)clht_get(pthread_to_lock->ht,
                                                    (clht_val_t)mutex);
    }
    return impl;
}

static lock_transparent_mutex_t *ht_lock_get(pthread_mutex_t *mutex) {
    lock_transparent_mutex_t *impl = (lock_transparent_mutex_t *)clht_get(
        pthread_to_lock->ht, (clht_val_t)mutex);
    if (impl == NULL) {
        impl = ht_lock_create(mutex, NULL);
    }

    return impl;
}
#endif

int (*REAL(pthread_mutex_init))(pthread_mutex_t *mutex,
                                const pthread_mutexattr_t *attr)
    __attribute__((aligned(L_CACHE_LINE_SIZE)));
int (*REAL(pthread_mutex_destroy))(pthread_mutex_t *mutex)
    __attribute__((aligned(L_CACHE_LINE_SIZE)));
int (*REAL(pthread_mutex_lock))(pthread_mutex_t *mutex)
	__attribute__((aligned(L_CACHE_LINE_SIZE)));
int (*REAL(pthread_mutex_timedlock))(pthread_mutex_t *mutex,
									 const struct timespec *abstime)
	__attribute__((aligned(L_CACHE_LINE_SIZE)));
int (*REAL(pthread_mutex_trylock))(pthread_mutex_t *mutex)
	__attribute__((aligned(L_CACHE_LINE_SIZE)));
int (*REAL(pthread_mutex_unlock))(pthread_mutex_t *mutex)
	__attribute__((aligned(L_CACHE_LINE_SIZE)));
int (*REAL(pthread_create))(pthread_t *thread, const pthread_attr_t *attr,
							void *(*start_routine)(void *), void *arg)
	__attribute__((aligned(L_CACHE_LINE_SIZE)));
int (*REAL(pthread_cond_init))(pthread_cond_t *cond,
							   const pthread_condattr_t *attr)
	__attribute__((aligned(L_CACHE_LINE_SIZE)));
int (*REAL(pthread_cond_destroy))(pthread_cond_t *cond)
	__attribute__((aligned(L_CACHE_LINE_SIZE)));
int (*REAL(pthread_cond_timedwait))(pthread_cond_t *cond,
									pthread_mutex_t *mutex,
									const struct timespec *abstime)
	__attribute__((aligned(L_CACHE_LINE_SIZE)));
int (*REAL(pthread_cond_wait))(pthread_cond_t *cond, pthread_mutex_t *mutex)
	__attribute__((aligned(L_CACHE_LINE_SIZE)));
int (*REAL(pthread_cond_signal))(pthread_cond_t *cond)
	__attribute__((aligned(L_CACHE_LINE_SIZE)));
int (*REAL(pthread_cond_broadcast))(pthread_cond_t *cond)
	__attribute__((aligned(L_CACHE_LINE_SIZE)));

// spin locks
int (*REAL(pthread_spin_init))(pthread_spinlock_t *lock, int pshared)
	__attribute__((aligned(L_CACHE_LINE_SIZE)));
int (*REAL(pthread_spin_destroy))(pthread_spinlock_t *lock)
	__attribute__((aligned(L_CACHE_LINE_SIZE)));
int (*REAL(pthread_spin_lock))(pthread_spinlock_t *lock)
	__attribute__((aligned(L_CACHE_LINE_SIZE)));
int (*REAL(pthread_spin_trylock))(pthread_spinlock_t *lock)
	__attribute__((aligned(L_CACHE_LINE_SIZE)));
int (*REAL(pthread_spin_unlock))(pthread_spinlock_t *lock)
	__attribute__((aligned(L_CACHE_LINE_SIZE)));

// rw locks
int (*REAL(pthread_rwlock_init))(pthread_rwlock_t *lock, const pthread_rwlockattr_t *attr)
	__attribute__((aligned(L_CACHE_LINE_SIZE)));
int (*REAL(pthread_rwlock_destroy))(pthread_rwlock_t *lock)
	__attribute__((aligned(L_CACHE_LINE_SIZE)));
int (*REAL(pthread_rwlock_rdlock))(pthread_rwlock_t *lock)
	__attribute__((aligned(L_CACHE_LINE_SIZE)));
int (*REAL(pthread_rwlock_wrlock))(pthread_rwlock_t *lock)
	__attribute__((aligned(L_CACHE_LINE_SIZE)));
int (*REAL(pthread_rwlock_timedrdlock))(pthread_rwlock_t *lock,
									 const struct timespec *abstime)
	__attribute__((aligned(L_CACHE_LINE_SIZE)));
int (*REAL(pthread_rwlock_timedwrlock))(pthread_rwlock_t *lock,
									 const struct timespec *abstime)
	__attribute__((aligned(L_CACHE_LINE_SIZE)));
int (*REAL(pthread_rwlock_tryrdlock))(pthread_rwlock_t *lock)
	__attribute__((aligned(L_CACHE_LINE_SIZE)));
int (*REAL(pthread_rwlock_trywrlock))(pthread_rwlock_t *lock)
	__attribute__((aligned(L_CACHE_LINE_SIZE)));
int (*REAL(pthread_rwlock_unlock))(pthread_rwlock_t *lock)
	__attribute__((aligned(L_CACHE_LINE_SIZE)));

#if CLEANUP_ON_SIGNAL
static void signal_exit(int signo);
#endif

volatile uint8_t init_spinlock = 0;

static void __attribute__((constructor)) REAL(interpose_init)(void) {
#if !(SUPPORT_WAITING) && !(defined(WAITING_ORIGINAL))
#error "Trying to compile a lock algorithm with a generic waiting policy."
#endif

    // Init once, other concurrent threads wait
    // 0 = not initiated
    // 1 = initializing
    // 2 = already initiated
    uint8_t cur_init = __sync_val_compare_and_swap(&init_spinlock, 0, 1);
    if (cur_init == 1) {
        while (init_spinlock == 1) {
            CPU_PAUSE();
        }
        return;
    } else if (cur_init == 2) {
        return;
    }

    printf("Using Lib%s with waiting %s\n", LOCK_ALGORITHM, WAITING_POLICY);
#if !NO_INDIRECTION
    pthread_to_lock = clht_create(NUM_BUCKETS);
    assert(pthread_to_lock != NULL);
#endif

    // The main thread should also have an ID
    cur_thread_id = __sync_fetch_and_add(&last_thread_id, 1);
    if (cur_thread_id >= MAX_THREADS) {
        fprintf(stderr, "Maximum number of threads reached. Consider raising "
                        "MAX_THREADS in interpose.c\n");
        exit(-1);
    }
    tinfo.tid = -1;
#if !NO_INDIRECTION
    clht_gc_thread_init(pthread_to_lock, cur_thread_id);
#endif

    lock_application_init();

#if CLEANUP_ON_SIGNAL
    // Signal handler for destroying locks at then end
    // We can't batch the registrations of the handler with a single syscall
    if (signal(SIGINT, signal_exit) == SIG_ERR) {
        fprintf(stderr, "Unable to install signal handler to catch SIGINT\n");
        abort();
    }

    if (signal(SIGTERM, signal_exit) == SIG_ERR) {
        fprintf(stderr, "Unable to install signal handler to catch SIGTERM\n");
        abort();
    }
#endif

    LOAD_FUNC(pthread_mutex_init, 1, FCT_LINK_SUFFIX);
    LOAD_FUNC(pthread_mutex_destroy, 1, FCT_LINK_SUFFIX);
    LOAD_FUNC(pthread_mutex_lock, 1, FCT_LINK_SUFFIX);
    LOAD_FUNC(pthread_mutex_timedlock, 1, FCT_LINK_SUFFIX);
    LOAD_FUNC(pthread_mutex_trylock, 1, FCT_LINK_SUFFIX);
    LOAD_FUNC(pthread_mutex_unlock, 1, FCT_LINK_SUFFIX);
    LOAD_FUNC_VERSIONED(pthread_cond_timedwait, 1, GLIBC_2_3_2,
                        FCT_LINK_SUFFIX);
    LOAD_FUNC_VERSIONED(pthread_cond_wait, 1, GLIBC_2_3_2, FCT_LINK_SUFFIX);
    LOAD_FUNC_VERSIONED(pthread_cond_broadcast, 1, GLIBC_2_3_2,
                        FCT_LINK_SUFFIX);
    LOAD_FUNC_VERSIONED(pthread_cond_destroy, 1, GLIBC_2_3_2, FCT_LINK_SUFFIX);
    LOAD_FUNC_VERSIONED(pthread_cond_init, 1, GLIBC_2_3_2, FCT_LINK_SUFFIX);
    LOAD_FUNC_VERSIONED(pthread_cond_signal, 1, GLIBC_2_3_2, FCT_LINK_SUFFIX);
    LOAD_FUNC(pthread_create, 1, FCT_LINK_SUFFIX);

    LOAD_FUNC(pthread_spin_init, 1, FCT_LINK_SUFFIX);
    LOAD_FUNC(pthread_spin_destroy, 1, FCT_LINK_SUFFIX);
    LOAD_FUNC(pthread_spin_lock, 1, FCT_LINK_SUFFIX);
    LOAD_FUNC(pthread_spin_trylock, 1, FCT_LINK_SUFFIX);
    LOAD_FUNC(pthread_spin_unlock, 1, FCT_LINK_SUFFIX);
    LOAD_FUNC(pthread_rwlock_init, 1, FCT_LINK_SUFFIX);
    LOAD_FUNC(pthread_rwlock_destroy, 1, FCT_LINK_SUFFIX);
    LOAD_FUNC(pthread_rwlock_rdlock, 1, FCT_LINK_SUFFIX);
    LOAD_FUNC(pthread_rwlock_wrlock, 1, FCT_LINK_SUFFIX);
    LOAD_FUNC(pthread_rwlock_timedrdlock, 1, FCT_LINK_SUFFIX);
    LOAD_FUNC(pthread_rwlock_timedwrlock, 1, FCT_LINK_SUFFIX);
    LOAD_FUNC(pthread_rwlock_tryrdlock, 1, FCT_LINK_SUFFIX);
    LOAD_FUNC(pthread_rwlock_trywrlock, 1, FCT_LINK_SUFFIX);
    LOAD_FUNC(pthread_rwlock_unlock, 1, FCT_LINK_SUFFIX);

    __sync_synchronize();
    init_spinlock = 2;
}
#if !NO_INDIRECTION
static inline lock_context_t *get_node(lock_transparent_mutex_t *impl) {
#if NEED_CONTEXT
    return &impl->lock_node[cur_thread_id];
#else
    return NULL;
#endif
}

#if ACCOUNTING
#endif

#if ACCOUNTING
static inline int __get_active_threads(void)
{
    int i;
    int sum = 0;
    for (i = 0; i < last_thread_id; ++i)
        if (active[i])
            sum += 1;
    return sum;
}

static inline void cs_log_phase(void *impl, uint8_t csphase, uint8_t phase)
{
    if (csphase == BEFORE_ENTER_CS &&
        (phase == PHASE_LOCK || phase == PHASE_TRYLOCK))
        active[cur_thread_id] = 1;
    else if (csphase == AFTER_EXIT_CS && phase == PHASE_UNLOCK)
        active[cur_thread_id] = 0;

    if (csphase == AFTER_ENTER_CS)
        lock_level++;

    if (tlinfo[cur_thread_id].cstime) {
        if (tlinfo[cur_thread_id].count < MAX_COUNT) {
            tlinfo[cur_thread_id].cstime[tlinfo[cur_thread_id].count].addr = (unsigned long)impl;
            tlinfo[cur_thread_id].cstime[tlinfo[cur_thread_id].count].logtime = getticks();
            tlinfo[cur_thread_id].cstime[tlinfo[cur_thread_id].count].csphase = csphase;
            tlinfo[cur_thread_id].cstime[tlinfo[cur_thread_id].count].level = lock_level;
            tlinfo[cur_thread_id].cstime[tlinfo[cur_thread_id].count].active = __get_active_threads();
            tlinfo[cur_thread_id].cstime[tlinfo[cur_thread_id].count++].phase = phase;
        }
    }

    if (csphase == AFTER_EXIT_CS)
        lock_level--;
}
#else
#define cs_log_phase(i, c, p) do { } while(0)
#endif
#endif

static void __attribute__((destructor)) REAL(interpose_exit)(void) {
#if DESTROY_ON_EXIT
    // TODO: modify CLHT to do that
    uint64_t num_buckets = pthread_to_lock->ht->num_buckets;
    volatile bucket_t *bucket;

    uint64_t bin;
    for (bin = 0; bin < num_buckets; bin++) {
        bucket = pthread_to_lock->ht->table + bin;

        uint32_t j;
        do {
            for (j = 0; j < ENTRIES_PER_BUCKET; j++) {
                if (bucket->key[j]) {
                    lock_transparent_mutex_t *lock =
                        (lock_transparent_mutex_t *)bucket->val[j];
                    fprintf(stderr, "\n%p,%lu,%f\n", lock->lock_lock,
                            lock->lock_lock->max, lock->lock_lock->mean);
                    // Do not destroy the lock if concurrent accesses
                    // concurrency_mutex_destroy(lock->lock_lock);
                }
            }

            bucket = bucket->padding;
        } while (bucket != NULL);
    }
#endif
    // Do not destroy the hashtable. If we shutdown the application via
    // signal, some threads might still be running and accessing the hashmap
    // concurrently. (Anyway, the kernel will clean this)

    // clht_gc_destroy(pthread_to_lock);
    // pthread_to_lock = NULL;
    //
#if !NO_INDIRECTION
#if ACCOUNTING
    /* while (logging_done != cur_thread_id) */
    /*     CPU_PAUSE(); */
    /* cs_print_info(NULL); */
    /* printf("logging: %d\n", logging_done); */
    /* logging_done += 1; */
#endif
#endif
    lock_application_exit();
}


#if CLEANUP_ON_SIGNAL
static void signal_exit(int UNUSED(signo)) {
    fprintf(stderr, "Signal received\n");
    exit(-1);
}
#endif

static void *lp_start_routine(void *_arg) {
    struct routine *r = _arg;
    void *(*fct)(void *) = r->fct;
    void *arg = r->arg;
    void *res;
    free(r);

    cur_thread_id = __sync_fetch_and_add(&last_thread_id, 1);
    tinfo.tid = -1;
    if (cur_thread_id >= MAX_THREADS) {
        fprintf(stderr, "Maximum number of threads reached. Consider raising "
                        "MAX_THREADS in interpose.c (current = %u)\n",
                MAX_THREADS);
        exit(-1);
    }

#if !NO_INDIRECTION
#if ACCOUNTING
    tlinfo[cur_thread_id].count = 0;
    tlinfo[cur_thread_id].cstime = malloc(sizeof(struct cstime) * MAX_COUNT);
    if (tlinfo[cur_thread_id].cstime == NULL) {
        exit(-1);
    }
    memset(tlinfo[cur_thread_id].cstime, 0, sizeof(struct cstime) * MAX_COUNT);
#endif
#endif

#if !NO_INDIRECTION
    clht_gc_thread_init(pthread_to_lock, cur_thread_id);
#endif
    lock_thread_start();
    res = fct(arg);
    lock_thread_exit();

    return res;
}

int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                   void *(*start_routine)(void *), void *arg) {
    DEBUG_PTHREAD("[p] pthread_create\n");
    struct routine *r = malloc(sizeof(struct routine));

    r->fct = start_routine;
    r->arg = arg;

    return REAL(pthread_create)(thread, attr, lp_start_routine, r);
}

int pthread_mutex_init(pthread_mutex_t *mutex,
                       const pthread_mutexattr_t *attr) {
    DEBUG_PTHREAD("[p] pthread_mutex_init\n");
#if !NO_INDIRECTION
    ht_lock_create(mutex, attr);
    return 0;
#else
    return REAL(pthread_mutex_init)(mutex, attr);
#endif
}

#if !NO_INDIRECTION
#if ACCOUNTING

static void __write_raw_data(int fd, const void *buf, size_t count)
{
    if (write(fd, buf, count) != count) {
        printf("failed to write data\n");
        exit(-1);
    }
}

#endif
#endif

int pthread_mutex_destroy(pthread_mutex_t *mutex) {
    DEBUG_PTHREAD("[p] pthread_mutex_destroy\n");
#if !NO_INDIRECTION
	int i;
#if ACCOUNTING
    if (__sync_bool_compare_and_swap(&logging_done, 0, 1)) {
        char buf[128];
        sprintf(buf, "%u.%s.raw.dat", last_thread_id - 1, LOCK_ALGORITHM);
        printf("writing to file: %s\n", buf);
        int fd = open(buf, O_WRONLY | O_CREAT, 0600);
        if (fd < 0) {
            printf("failed to open the file\n");
            exit(-1);
        }
        __write_raw_data(fd, &last_thread_id, sizeof(last_thread_id));
        for (i = 0; i < last_thread_id; ++i) {
            __write_raw_data(fd, &tlinfo[i].count, sizeof(tlinfo[i].count));
            __write_raw_data(fd, tlinfo[i].cstime,
                             sizeof(struct cstime) * tlinfo[i].count);
        }
        close(fd);
    }
#endif
	unsigned long sum_shuffler = 0, sum_lkholder = 0, sum_st_shuffler = 0;
	for (i = 0; i < MAX_THREADS; ++i) {
		sum_shuffler += wake_up_shuffler[i];
		sum_lkholder += wake_up_lkholder[i];
		sum_st_shuffler += node_st_shuffler[i];
	}

	/* printf("woke up %lu from critical path and %lu from shuffler and changed %lu states\n", */
	/* 	   sum_lkholder, sum_shuffler, sum_st_shuffler); */

    lock_transparent_mutex_t *impl = (lock_transparent_mutex_t *)clht_remove(
        pthread_to_lock, (clht_addr_t)mutex);
    if (impl != NULL) {
        lock_mutex_destroy(impl->lock_lock);
        free(impl);
    }

    /* return REAL(pthread_mutex_destroy)(mutex); */
    return 0;
#else
    return lock_mutex_destroy(mutex);
#endif
}

int pthread_mutex_lock(pthread_mutex_t *mutex) {
	int ret;
    DEBUG_PTHREAD("[p] pthread_mutex_lock\n");
#if !NO_INDIRECTION
    lock_transparent_mutex_t *impl = ht_lock_get(mutex);
    cs_log_phase(mutex, BEFORE_ENTER_CS, PHASE_LOCK);
    ret = lock_mutex_lock(impl->lock_lock, get_node(impl));
    cs_log_phase(mutex, AFTER_ENTER_CS, PHASE_LOCK);
#else
    ret = lock_mutex_lock(mutex, NULL);
#endif
    return ret;
}

int pthread_mutex_timedlock(pthread_mutex_t *mutex,
                            const struct timespec *abstime) {
    assert(0 && "Timed locks not supported");
}

int pthread_mutex_trylock(pthread_mutex_t *mutex) {
	int ret;

    DEBUG_PTHREAD("[p] pthread_mutex_trylock\n");
#if !NO_INDIRECTION
    lock_transparent_mutex_t *impl = ht_lock_get(mutex);
    cs_log_phase(mutex, BEFORE_ENTER_CS, PHASE_TRYLOCK);
    ret = lock_mutex_trylock(impl->lock_lock, get_node(impl));
    cs_log_phase(mutex, AFTER_ENTER_CS, PHASE_TRYLOCK);
#else
    ret = lock_mutex_trylock(mutex, NULL);
#endif
	return ret;
}

int pthread_mutex_unlock(pthread_mutex_t *mutex) {
    DEBUG_PTHREAD("[p] pthread_mutex_unlock\n");
#if !NO_INDIRECTION
    lock_transparent_mutex_t *impl = ht_lock_get(mutex);
    cs_log_phase(mutex, BEFORE_EXIT_CS, PHASE_UNLOCK);
    lock_mutex_unlock(impl->lock_lock, get_node(impl));
    cs_log_phase(mutex, AFTER_EXIT_CS, PHASE_UNLOCK);
#else
    lock_mutex_unlock(mutex, NULL);
#endif
	return 0;
}

int __pthread_cond_init(pthread_cond_t *cond, const pthread_condattr_t *attr) {
	int ret;
    DEBUG_PTHREAD("[p] pthread_cond_init\n");
    ret = lock_cond_init(cond, attr);
	return ret;
}
__asm__(".symver __pthread_cond_init,pthread_cond_init@@" GLIBC_2_3_2);

int __pthread_cond_destroy(pthread_cond_t *cond) {
    DEBUG_PTHREAD("[p] pthread_cond_destroy\n");
    return lock_cond_destroy(cond);
}
__asm__(".symver __pthread_cond_destroy,pthread_cond_destroy@@" GLIBC_2_3_2);

int __pthread_cond_timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex,
                             const struct timespec *abstime) {
    DEBUG_PTHREAD("[p] pthread_cond_timedwait\n");
	int ret;
#if !NO_INDIRECTION
    lock_transparent_mutex_t *impl = ht_lock_get(mutex);
    ret = lock_cond_timedwait(cond, impl->lock_lock, get_node(impl), abstime);
#else
    ret = lock_cond_timedwait(cond, mutex, NULL, abstime);
#endif
	return ret;
}
__asm__(
    ".symver __pthread_cond_timedwait,pthread_cond_timedwait@@" GLIBC_2_3_2);

int __pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex) {
    DEBUG_PTHREAD("[p] pthread_cond_wait\n");
#if !NO_INDIRECTION
    lock_transparent_mutex_t *impl = ht_lock_get(mutex);
    lock_cond_wait(cond, impl->lock_lock, get_node(impl));
#else
    lock_cond_wait(cond, mutex, NULL);
#endif
	return 0;
}
__asm__(".symver __pthread_cond_wait,pthread_cond_wait@@" GLIBC_2_3_2);

int __pthread_cond_signal(pthread_cond_t *cond) {
    DEBUG_PTHREAD("[p] pthread_cond_signal\n");
    return lock_cond_signal(cond);
}
__asm__(".symver __pthread_cond_signal,pthread_cond_signal@@" GLIBC_2_3_2);

int __pthread_cond_broadcast(pthread_cond_t *cond) {
    DEBUG_PTHREAD("[p] pthread_cond_broadcast\n");
    return lock_cond_broadcast(cond);
}
__asm__(
    ".symver __pthread_cond_broadcast,pthread_cond_broadcast@@" GLIBC_2_3_2);

// Spinlocks
int pthread_spin_init(pthread_spinlock_t *spin,
                      int pshared) {
    DEBUG_PTHREAD("[p] pthread_spin_init\n");
    if (init_spinlock != 2) {
        REAL(interpose_init)();
    }

#if !NO_INDIRECTION
    ht_lock_create((void*)spin, NULL);
    return 0;
#else
    return REAL(pthread_spin_init)(spin, pshared);
#endif
}

int pthread_spin_destroy(pthread_spinlock_t *spin) {
    DEBUG_PTHREAD("[p] pthread_spin_destroy\n");
#if !NO_INDIRECTION
    lock_transparent_mutex_t *impl = (lock_transparent_mutex_t *)clht_remove(
        pthread_to_lock, (clht_addr_t)spin);
    if (impl != NULL) {
        lock_mutex_destroy(impl->lock_lock);
        free(impl);
    }

    return 0;
#else
    assert(0 && "spinlock not supported without indirection");
#endif
}

int pthread_spin_lock(pthread_spinlock_t *spin) {
	int ret;
    DEBUG_PTHREAD("[p] pthread_spin_lock\n");
#if !NO_INDIRECTION
    lock_transparent_mutex_t *impl = ht_lock_get((void*)spin);
    cs_log_phase((void *)spin, BEFORE_ENTER_CS, PHASE_LOCK);
    ret = lock_mutex_lock(impl->lock_lock, get_node(impl));
    cs_log_phase((void *)spin, AFTER_ENTER_CS, PHASE_LOCK);
#else
    assert(0 && "spinlock not supported without indirection");
#endif
	return ret;
}

int pthread_spin_trylock(pthread_spinlock_t *spin) {
	int ret;
    DEBUG_PTHREAD("[p] pthread_spin_trylock\n");
#if !NO_INDIRECTION
    lock_transparent_mutex_t *impl = ht_lock_get((void*)spin);
    cs_log_phase((void *)spin, BEFORE_ENTER_CS, PHASE_TRYLOCK);
    ret = lock_mutex_trylock(impl->lock_lock, get_node(impl));
    cs_log_phase((void *)spin, AFTER_ENTER_CS, PHASE_TRYLOCK);
#else
    assert(0 && "spinlock not supported without indirection");
#endif
	return ret;
}

int pthread_spin_unlock(pthread_spinlock_t *spin) {
    DEBUG_PTHREAD("[p] pthread_spin_unlock\n");
#if !NO_INDIRECTION
    lock_transparent_mutex_t *impl = ht_lock_get((void*)spin);
    cs_log_phase((void *)spin, BEFORE_EXIT_CS, PHASE_UNLOCK);
    lock_mutex_unlock(impl->lock_lock, get_node(impl));
    cs_log_phase((void *)spin, AFTER_EXIT_CS, PHASE_UNLOCK);
#else
    assert(0 && "spinlock not supported without indirection");
#endif
	return 0;
}

#if defined(TTASRW) || defined(HMCSRW)
// interposes rwlock
#if !NO_INDIRECTION
typedef struct {
    lock_rwlock_t *lock_lock;
    char __pad[pad_to_cache_line(sizeof(lock_rwlock_t *))];
#if NEED_CONTEXT
    lock_context_t lock_node[MAX_THREADS];
#endif
} lock_transparent_rwlock_t;

static lock_transparent_rwlock_t *
ht_rwlock_create(pthread_rwlock_t *rwlock, const pthread_rwlockattr_t *attr) {
    lock_transparent_rwlock_t *impl = alloc_cache_align(sizeof *impl);
    impl->lock_lock                = lock_rwlock_create(attr);
#if NEED_CONTEXT
    lock_init_context(impl->lock_lock, impl->lock_node, MAX_THREADS);
#endif

    // If a lock is initialized statically and two threads acquire the locks at
    // the same time, then only one call to clht_put will succeed.
    // For the failing thread, we free the previously allocated mutex data
    // structure and do a lookup to retrieve the ones inserted by the successful
    // thread.
    if (clht_put(pthread_to_lock, (clht_addr_t)rwlock, (clht_val_t)impl) == 0) {
        free(impl);
        return (lock_transparent_rwlock_t *)clht_get(pthread_to_lock->ht,
                                                    (clht_val_t)rwlock);
    }
    return impl;
}

static lock_transparent_rwlock_t *ht_rwlock_get(pthread_rwlock_t *rwlock) {
    lock_transparent_rwlock_t *impl = (lock_transparent_rwlock_t *)clht_get(
        pthread_to_lock->ht, (clht_val_t)rwlock);
    if (impl == NULL) {
        impl = ht_rwlock_create(rwlock, NULL);
    }

    return impl;
}

static inline lock_context_t *get_rwlock_node(lock_transparent_rwlock_t *impl) {
#if NEED_CONTEXT
    return &impl->lock_node[cur_thread_id];
#else
    return NULL;
#endif
};
#endif /* !NO_INDIRECTION */

int pthread_rwlock_init(pthread_rwlock_t *rwlock,
                        const pthread_rwlockattr_t *attr) {
    DEBUG_PTHREAD("[p] pthread_rwlock_init\n");
    if (init_spinlock != 2) {
        REAL(interpose_init)();
    }

#if !NO_INDIRECTION
    ht_rwlock_create((void*)rwlock, NULL);
    return 0;
#else
    return REAL(pthread_rwlock_init)(rwlock, attr);
#endif
}

int pthread_rwlock_destroy(pthread_rwlock_t *rwlock) {
    DEBUG_PTHREAD("[p] pthread_rwlock_destroy\n");
#if !NO_INDIRECTION
    lock_transparent_rwlock_t *impl = (lock_transparent_rwlock_t *)clht_remove(
        pthread_to_lock, (clht_addr_t)rwlock);
    if (impl != NULL) {
        lock_rwlock_destroy(impl->lock_lock);
        free(impl);
    }
    return 0;
#else
    assert(0 && "rwlock not supported without indirection");
#endif
}

int pthread_rwlock_rdlock(pthread_rwlock_t *rwlock) {
	int ret;
    DEBUG_PTHREAD("[p] pthread_rwlock_rdlock\n");
#if !NO_INDIRECTION
    lock_transparent_rwlock_t *impl = ht_rwlock_get((void*)rwlock);
    cs_log_phase((void *)rwlock, BEFORE_ENTER_CS, PHASE_RD_LOCK);
    ret = lock_rwlock_rdlock(impl->lock_lock, get_rwlock_node(impl));
    cs_log_phase((void *)rwlock, AFTER_ENTER_CS, PHASE_RD_LOCK);
#else
    assert(0 && "rwlock not supported without indirection");
#endif
	return ret;
}

int pthread_rwlock_wrlock(pthread_rwlock_t *rwlock) {
	int ret;
    DEBUG_PTHREAD("[p] pthread_rwlock_wrlock\n");
#if !NO_INDIRECTION
    lock_transparent_rwlock_t *impl = ht_rwlock_get((void*)rwlock);
    cs_log_phase((void *)rwlock, BEFORE_ENTER_CS, PHASE_LOCK);
    ret = lock_rwlock_wrlock(impl->lock_lock, get_rwlock_node(impl));
    cs_log_phase((void *)rwlock, AFTER_ENTER_CS, PHASE_LOCK);
#else
    assert(0 && "rwlock not supported without indirection");
#endif
	return ret;
}

int pthread_rwlock_timedrdlock(pthread_rwlock_t *lcok,
                            const struct timespec *abstime) {
    assert(0 && "Timed locks not supported");
}

int pthread_rwlock_timedwrlock(pthread_rwlock_t *lock,
                            const struct timespec *abstime) {
    assert(0 && "Timed locks not supported");
}

int pthread_rwlock_tryrdlock(pthread_rwlock_t *rwlock) {
	int ret;
    DEBUG_PTHREAD("[p] pthread_rwlock_trylock\n");
#if !NO_INDIRECTION
    lock_transparent_rwlock_t *impl = ht_rwlock_get((void*)rwlock);
    cs_log_phase((void *)rwlock, BEFORE_ENTER_CS, PHASE_RD_TRYLOCK);
    ret = lock_rwlock_tryrdlock(impl->lock_lock, get_rwlock_node(impl));
    cs_log_phase((void *)rwlock, AFTER_ENTER_CS, PHASE_RD_TRYLOCK);
#else
    assert(0 && "rwlock not supported without indirection");
#endif
	return ret;
}

int pthread_rwlock_trywrlock(pthread_rwlock_t *rwlock) {
	int ret;
    DEBUG_PTHREAD("[p] pthread_rwlock_trylock\n");
#if !NO_INDIRECTION
    lock_transparent_rwlock_t *impl = ht_rwlock_get((void*)rwlock);
    cs_log_phase((void *)rwlock, BEFORE_ENTER_CS, PHASE_TRYLOCK);
    ret = lock_rwlock_trywrlock(impl->lock_lock, get_rwlock_node(impl));
    cs_log_phase((void *)rwlock, AFTER_ENTER_CS, PHASE_TRYLOCK);
#else
    assert(0 && "rwlock not supported without indirection");
#endif
	return ret;
}

int pthread_rwlock_unlock(pthread_rwlock_t *rwlock) {
    DEBUG_PTHREAD("[p] pthread_rwlock_unlock\n");
#if !NO_INDIRECTION
    lock_transparent_rwlock_t *impl = ht_rwlock_get((void*)rwlock);
    cs_log_phase((void *)rwlock, BEFORE_EXIT_CS, PHASE_UNLOCK);
    lock_rwlock_unlock(impl->lock_lock, get_rwlock_node(impl));
    cs_log_phase((void *)rwlock, AFTER_EXIT_CS, PHASE_UNLOCK);
#else
    assert(0 && "rwlock not supported without indirection");
#endif
	return 0;
}


#else /* TTASRW rwlock implementation */
// Rw locks
int pthread_rwlock_init(pthread_rwlock_t *rwlock,
                        const pthread_rwlockattr_t *attr) {
    DEBUG_PTHREAD("[p] pthread_rwlock_init\n");
    if (init_spinlock != 2) {
        REAL(interpose_init)();
    }

#if !NO_INDIRECTION
    ht_lock_create((void*)rwlock, NULL);
    return 0;
#else
    return REAL(pthread_rwlock_init)(rwlock, attr);
#endif
}

int pthread_rwlock_destroy(pthread_rwlock_t *rwlock) {
    DEBUG_PTHREAD("[p] pthread_rwlock_destroy\n");
#if !NO_INDIRECTION
    lock_transparent_mutex_t *impl = (lock_transparent_mutex_t *)clht_remove(
        pthread_to_lock, (clht_addr_t)rwlock);
    if (impl != NULL) {
        lock_mutex_destroy(impl->lock_lock);
        free(impl);
    }
#if ACCOUNTING
    if (__sync_bool_compare_and_swap(&logging_done, 0, 1)) {
        int i;
        char buf[128];
        sprintf(buf, "%u.%s.raw.dat", last_thread_id - 1, LOCK_ALGORITHM);
        printf("writing to file: %s\n", buf);
        int fd = open(buf, O_WRONLY | O_CREAT, 0600);
        if (fd < 0) {
            printf("failed to open the file\n");
            exit(-1);
        }
        __write_raw_data(fd, &last_thread_id, sizeof(last_thread_id));
        for (i = 0; i < last_thread_id; ++i) {
            __write_raw_data(fd, &tlinfo[i].count, sizeof(tlinfo[i].count));
            __write_raw_data(fd, tlinfo[i].cstime,
                             sizeof(struct cstime) * tlinfo[i].count);
        }
        close(fd);
    }
#endif


    return 0;
#else
    assert(0 && "rwlock not supported without indirection");
#endif
}

int pthread_rwlock_rdlock(pthread_rwlock_t *rwlock) {
	int ret;
    DEBUG_PTHREAD("[p] pthread_rwlock_rdlock\n");
#if !NO_INDIRECTION
    lock_transparent_mutex_t *impl = ht_lock_get((void*)rwlock);
    cs_log_phase((void *)rwlock, BEFORE_ENTER_CS, PHASE_RD_LOCK);
    ret = lock_mutex_lock(impl->lock_lock, get_node(impl));
    cs_log_phase((void *)rwlock, AFTER_ENTER_CS, PHASE_RD_LOCK);
#else
    assert(0 && "rwlock not supported without indirection");
#endif
	return ret;
}

int pthread_rwlock_wrlock(pthread_rwlock_t *rwlock) {
	int ret
    DEBUG_PTHREAD("[p] pthread_rwlock_wrlock\n");
#if !NO_INDIRECTION
    lock_transparent_mutex_t *impl = ht_lock_get((void*)rwlock);
    cs_log_phase((void *)rwlock, BEFORE_ENTER_CS, PHASE_LOCK);
    ret = lock_mutex_lock(impl->lock_lock, get_node(impl));
    cs_log_phase((void *)rwlock, AFTER_ENTER_CS, PHASE_LOCK);
#else
    assert(0 && "rwlock not supported without indirection");
#endif
	return ret;
}

int pthread_rwlock_timedrdlock(pthread_rwlock_t *lcok,
                            const struct timespec *abstime) {
    assert(0 && "Timed locks not supported");
}

int pthread_rwlock_timedwrlock(pthread_rwlock_t *lock,
                            const struct timespec *abstime) {
    assert(0 && "Timed locks not supported");
}


int pthread_rwlock_rdtrylock(pthread_rwlock_t *rwlock) {
	int ret;
    DEBUG_PTHREAD("[p] pthread_rwlock_trylock\n");
#if !NO_INDIRECTION
    lock_transparent_mutex_t *impl = ht_lock_get((void*)rwlock);
    cs_log_phase((void *)rwlock, BEFORE_ENTER_CS, PHASE_RD_TRYLOCK);
    ret = lock_mutex_trylock(impl->lock_lock, get_node(impl));
    cs_log_phase((void *)rwlock, AFTER_ENTER_CS, PHASE_RD_TRYLOCK);
#else
    assert(0 && "rwlock not supported without indirection");
#endif
	return ret;
}

int pthread_rwlock_wrtrylock(pthread_rwlock_t *rwlock) {
	int ret;
    DEBUG_PTHREAD("[p] pthread_rwlock_trylock\n");
#if !NO_INDIRECTION
    lock_transparent_mutex_t *impl = ht_lock_get((void*)rwlock);
    cs_log_phase((void *)rwlock, BEFORE_ENTER_CS, PHASE_TRYLOCK);
    ret = lock_mutex_trylock(impl->lock_lock, get_node(impl));
    cs_log_phase((void *)rwlock, AFTER_ENTER_CS, PHASE_TRYLOCK);
#else
    assert(0 && "rwlock not supported without indirection");
#endif
	return ret;
}

int pthread_rwlock_unlock(pthread_rwlock_t *rwlock) {
    DEBUG_PTHREAD("[p] pthread_rwlock_unlock\n");
#if !NO_INDIRECTION
    lock_transparent_mutex_t *impl = ht_lock_get((void*)rwlock);
    cs_log_phase((void *)rwlock, BEFORE_EXIT_CS, PHASE_UNLOCK);
    lock_mutex_unlock(impl->lock_lock, get_node(impl));
    cs_log_phase((void *)rwlock, AFTER_EXIT_CS, PHASE_UNLOCK);
#else
    assert(0 && "rwlock not supported without indirection");
#endif
	return 0;
}

#endif /* TTASRW rwlock implementation */
