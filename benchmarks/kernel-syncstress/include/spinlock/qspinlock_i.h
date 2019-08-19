#ifndef __QUEUED_SPINLOCK_INTERNAL_H__
#define __QUEUED_SPINLOCK_INTERNAL_H__

#include <linux/smp.h>
#include <linux/bug.h>
#include <linux/percpu.h>
#include <linux/hardirq.h>
#include <linux/prefetch.h>
#include <linux/atomic.h>
#include <asm/byteorder.h>
#include "mcs_spinlock.h"

#ifndef CONFIG_PARAVIRT
#include <linux/types.h>
#include <linux/atomic.h>
#endif

/**
 * smp_cond_load_relaxed() - (Spin) wait for cond with no ordering guarantees
 * @ptr: pointer to the variable to wait on
 * @cond: boolean expression to wait for
 *
 * Equivalent to using READ_ONCE() on the condition variable.
 *
 * Due to C lacking lambda expressions we load the value of *ptr into a
 * pre-named variable @VAL to be used in @cond.
 */
#ifndef smp_cond_load_relaxed
#define smp_cond_load_relaxed(ptr, cond_expr) ({		\
	typeof(ptr) __PTR = (ptr);				\
	typeof(*ptr) VAL;					\
	for (;;) {						\
		VAL = READ_ONCE(*__PTR);			\
		if (cond_expr)					\
			break;					\
		cpu_relax();					\
	}							\
	VAL;							\
})
#endif

#ifndef atomic_cond_read_relaxed
#define atomic_cond_read_relaxed(v, c)	smp_cond_load_relaxed(&(v)->counter, (c))
#endif

struct orig_qspinlock {
	union {
		atomic_t val;

		/*
		 * By using the whole 2nd least significant byte for the
		 * pending bit, we can allow better optimization of the lock
		 * acquisition for the pending bit holder.
		 */
#ifdef __LITTLE_ENDIAN
		struct {
			u8	locked;
			u8	pending;
		};
		struct {
			u16	locked_pending;
			u16	tail;
		};
#else
		struct {
			u16	tail;
			u16	locked_pending;
		};
		struct {
			u8	reserved[2];
			u8	pending;
			u8	locked;
		};
#endif
	};
};

/*
 * Initializier
 */
#ifndef __ORIG_QSPIN_LOCK_UNLOCKED
#define	__ORIG_QSPIN_LOCK_UNLOCKED	{ .val = ATOMIC_INIT(0) }
#endif

/*
 * Bitfields in the atomic value:
 *
 * When NR_CPUS < 16K
 *  0- 7: locked byte
 *     8: pending
 *  9-15: not used
 * 16-17: tail index
 * 18-31: tail cpu (+1)
 *
 * When NR_CPUS >= 16K
 *  0- 7: locked byte
 *     8: pending
 *  9-10: tail index
 * 11-31: tail cpu (+1)
 */
#define	_Q_SET_MASK(type)	(((1U << _Q_ ## type ## _BITS) - 1)\
				      << _Q_ ## type ## _OFFSET)
#define _Q_LOCKED_OFFSET	0
#define _Q_LOCKED_BITS		8
#define _Q_LOCKED_MASK		_Q_SET_MASK(LOCKED)

#define _Q_PENDING_OFFSET	(_Q_LOCKED_OFFSET + _Q_LOCKED_BITS)
#if CONFIG_NR_CPUS < (1U << 14)
#define _Q_PENDING_BITS		8
#else
#define _Q_PENDING_BITS		1
#endif
#define _Q_PENDING_MASK		_Q_SET_MASK(PENDING)

#define _Q_TAIL_IDX_OFFSET	(_Q_PENDING_OFFSET + _Q_PENDING_BITS)
#define _Q_TAIL_IDX_BITS	2
#define _Q_TAIL_IDX_MASK	_Q_SET_MASK(TAIL_IDX)

#define _Q_TAIL_CPU_OFFSET	(_Q_TAIL_IDX_OFFSET + _Q_TAIL_IDX_BITS)
#define _Q_TAIL_CPU_BITS	(32 - _Q_TAIL_CPU_OFFSET)
#define _Q_TAIL_CPU_MASK	_Q_SET_MASK(TAIL_CPU)

#define _Q_TAIL_OFFSET		_Q_TAIL_IDX_OFFSET
#define _Q_TAIL_MASK		(_Q_TAIL_IDX_MASK | _Q_TAIL_CPU_MASK)

#define _Q_LOCKED_VAL		(1U << _Q_LOCKED_OFFSET)
#define _Q_PENDING_VAL		(1U << _Q_PENDING_OFFSET)

/*
 * Bitfields in the non-atomic socket_and_count value:
 * 0- 1: queue node index (always < 4)
 * 2-31: socket id
 */
#define _Q_IDX_OFFSET           0
#define _Q_IDX_BITS             2
#define _Q_IDX_MASK             _Q_SET_MASK(IDX)
#define _Q_NODE_OFFSET          (_Q_IDX_OFFSET + _Q_IDX_BITS)

#endif /* __QUEUED_SPINLOCK_INTERNAL_H__ */
