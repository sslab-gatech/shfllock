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
#ifndef __AQS_H__
#define __AQS_H__

#include <string.h>

#include "padding.h"
#define LOCK_ALGORITHM "AQSWONODE"
#define NEED_CONTEXT 0
#define SUPPORT_WAITING 1

/*
 * Bit manipulation (not used currently)
 * Will use just one variable of 4 byts to enclose the following:
 * 0-7:   locked or unlocked
 * 8-15:  shuffle leader or not
 * 16-31: shuffle count
 */
#define	_AQS_SET_MASK(type)	(((1U << _AQS_ ## type ## _BITS) - 1)\
                                 << _AQS_ ## type ## _OFFSET)

/* This is directly used by the tail bytes (2 bytes) */
#define _AQS_TAIL_IDX_OFFSET	(0)
#define _AQS_TAIL_IDX_BITS	2
#define _AQS_TAIL_IDX_MASK	_AQS_SET_MASK(TAIL_IDX)

#define _AQS_TAIL_CPU_OFFSET	(_AQS_TAIL_IDX_OFFSET + _AQS_TAIL_IDX_BITS)
#define _AQS_TAIL_CPU_BITS	(16 - _AQS_TAIL_CPU_OFFSET)
#define _AQS_TAIL_CPU_MASK	_AQS_SET_MASK(TAIL_CPU)

#define _AQS_TAIL_OFFSET	_AQS_TAIL_IDX_OFFSET
#define _AQS_TAIL_MASK		(_AQS_TAIL_IDX_MASK | _AQS_TAIL_CPU_MASK)

/* Use 1 bit for the NOSTEAL part */
#define _AQS_NOSTEAL_OFFSET     0
#define _AQS_NOSTEAL_BITS       1
#define _AQS_NOSTEAL_MASK       _AQS_SET_MASK(NOSTEAL)

/* We can support up to 127 sockets for NUMA-aware fastpath stealing */
#define _AQS_NUMA_ID_OFFSET     (_AQS_NOSTEAL_OFFSET + _AQS_NOSTEAL_BITS)
#define _AQS_NUMA_ID_BITS       7
#define _AQS_NUMA_ID_MASK       _AQS_SET_MASK(NUMA_ID)
#define _AQS_NUMA_ID_VAL(v)     ((v) & _AQS_NUMA_ID_MASK) >> _AQS_NUMA_ID_OFFSET

#define _AQS_LOCKED_OFFSET              0
#define _AQS_LOCKED_BITS                8
#define _AQS_LOCKED_NOSTEAL_OFFSET      (_AQS_LOCKED_OFFSET + _AQS_LOCKED_BITS)

#define AQS_NOSTEAL_VAL         1
#define AQS_STATUS_WAIT         0
#define AQS_STATUS_LOCKED       1
#define AQS_MAX_LOCK_COUNT      256
#define AQS_SERVE_COUNT         (255) /* max of 8 bits */

/* Arch utility */
static inline void smp_rmb(void)
{
    __asm __volatile("lfence":::"memory");
}

static inline void smp_cmb(void)
{
    __asm __volatile("":::"memory");
}

#define barrier()           smp_cmb()

static inline void __write_once_size(volatile void *p, void *res, int size)
{
        switch(size) {
        case 1: *(volatile uint8_t *)p = *(uint8_t *)res; break;
        case 2: *(volatile uint16_t *)p = *(uint16_t *)res; break;
        case 4: *(volatile uint32_t *)p = *(uint32_t *)res; break;
        case 8: *(volatile uint64_t *)p = *(uint64_t *)res; break;
        default:
                barrier();
                memcpy((void *)p, (const void *)res, size);
                barrier();
        }
}

static inline void __read_once_size(volatile void *p, void *res, int size)
{
        switch(size) {
        case 1: *(uint8_t *)res = *(volatile uint8_t *)p; break;
        case 2: *(uint16_t *)res = *(volatile uint16_t *)p; break;
        case 4: *(uint32_t *)res = *(volatile uint32_t *)p; break;
        case 8: *(uint64_t *)res = *(volatile uint64_t *)p; break;
        default:
                barrier();
                memcpy((void *)res, (const void *)p, size);
                barrier();
        }
}

#define WRITE_ONCE(x, val)                                      \
        ({                                                      \
         union { typeof(x) __val; char __c[1]; } __u =          \
                { .__val = (typeof(x)) (val) };                 \
        __write_once_size(&(x), __u.__c, sizeof(x));            \
        __u.__val;                                              \
         })

#define READ_ONCE(x)                                            \
        ({                                                      \
         union { typeof(x) __val; char __c[1]; } __u;           \
         __read_once_size(&(x), __u.__c, sizeof(x));            \
         __u.__val;                                             \
         })

#define smp_cas(__ptr, __old_val, __new_val)	\
        __sync_val_compare_and_swap(__ptr, __old_val, __new_val)
#define smp_swap(__ptr, __val)			\
	__sync_lock_test_and_set(__ptr, __val)
#define smp_faa(__ptr, __val)			\
	__sync_fetch_and_add(__ptr, __val)

typedef struct aqs_node {
    struct aqs_node *next;
    union {
        uint32_t locked;
        struct {
            uint8_t lstatus;
            uint8_t sleader;
            uint16_t wcount;
        };
    };
    int nid;
    int cid;
    struct aqs_node *last_visited;

    int lock_status;
    int type;
    char __pad2[pad_to_cache_line(sizeof(uint32_t))];
} aqs_node_t __attribute__((aligned(L_CACHE_LINE_SIZE)));

typedef struct aqs_mutex {
    struct aqs_node *tail;
    union {
        uint32_t val;
        struct {
            uint8_t locked;
            uint8_t no_stealing;
        };
        struct {
            uint16_t locked_no_stealing;
            uint8_t __pad[2];
        };
   };
    char __pad2[pad_to_cache_line(sizeof(uint32_t))];
#if COND_VAR
    pthread_mutex_t posix_lock;
    char __pad3[pad_to_cache_line(sizeof(pthread_mutex_t))];
#endif
} aqs_mutex_t __attribute__((aligned(L_CACHE_LINE_SIZE)));

typedef pthread_cond_t aqs_cond_t;
aqs_mutex_t *aqs_mutex_create(const pthread_mutexattr_t *attr);
int aqs_mutex_lock(aqs_mutex_t *impl, aqs_node_t *me);
int aqs_mutex_trylock(aqs_mutex_t *impl, aqs_node_t *me);
void aqs_mutex_unlock(aqs_mutex_t *impl, aqs_node_t *me);
int aqs_mutex_destroy(aqs_mutex_t *lock);
int aqs_cond_init(aqs_cond_t *cond, const pthread_condattr_t *attr);
int aqs_cond_timedwait(aqs_cond_t *cond, aqs_mutex_t *lock, aqs_node_t *me,
                       const struct timespec *ts);
int aqs_cond_wait(aqs_cond_t *cond, aqs_mutex_t *lock, aqs_node_t *me);
int aqs_cond_signal(aqs_cond_t *cond);
int aqs_cond_broadcast(aqs_cond_t *cond);
int aqs_cond_destroy(aqs_cond_t *cond);
void aqs_thread_start(void);
void aqs_thread_exit(void);
void aqs_application_init(void);
void aqs_application_exit(void);
void aqs_init_context(aqs_mutex_t *impl, aqs_node_t *context, int number);

typedef aqs_mutex_t lock_mutex_t;
typedef aqs_node_t lock_context_t;
typedef aqs_cond_t lock_cond_t;

#define lock_mutex_create aqs_mutex_create
#define lock_mutex_lock aqs_mutex_lock
#define lock_mutex_trylock aqs_mutex_trylock
#define lock_mutex_unlock aqs_mutex_unlock
#define lock_mutex_destroy aqs_mutex_destroy
#define lock_cond_init aqs_cond_init
#define lock_cond_timedwait aqs_cond_timedwait
#define lock_cond_wait aqs_cond_wait
#define lock_cond_signal aqs_cond_signal
#define lock_cond_broadcast aqs_cond_broadcast
#define lock_cond_destroy aqs_cond_destroy
#define lock_thread_start aqs_thread_start
#define lock_thread_exit aqs_thread_exit
#define lock_application_init aqs_application_init
#define lock_application_exit aqs_application_exit
#define lock_init_context aqs_init_context

#endif // __AQS_H__
