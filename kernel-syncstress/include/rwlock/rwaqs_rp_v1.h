
#ifndef _RW_AQS_V1_RP_H
#define _RW_AQS_V1_RP_H

#include "spinlock/aqs.h"

/*
 * Writer states & reader shift and bias.
 */
#define	_AQS_V1_RP_W_WAITING	0x100		/* A writer is waiting	   */
#define	_AQS_V1_RP_W_LOCKED	0x0fc		/* A writer holds the lock */
#define	_AQS_V1_RP_W_WMASK	0x1fc		/* Writer mask		   */
#define	_AQS_V1_RP_R_SHIFT	9		/* Reader count shift	   */
#define _AQS_V1_RP_R_BIAS	(1U << _AQS_V1_RP_R_SHIFT)
#define _AQS_V1_RP_R_RPFD 	0x1


struct aqs_v1_rwlock_rp {
	union {
		atomic_t cnts;
		struct {
#ifdef __LITTLE_ENDIAN
			u8 wlocked;
			u8 __lstate[3];
#else
			u8 __lstate[3];
			u8 wlocked;
#endif
		};
	};
	struct aqs_lock wait_lock;
};

#define __RW_AQS_V1_RP_LOCK_UNLOCKED {                             \
	{ .cnts = ATOMIC_INIT(0) }                              \
	, .wait_lock = __AQS_LOCK_UNLOCKED,                     \
	}

#define DEFINE_AQS_V1_RWLOCK_RP(x) \
	struct aqs_v1_rwlock_rp x = __RW_AQS_V1_RP_LOCK_UNLOCKED;

DECLARE_PER_CPU_ALIGNED(struct aqs_v1_rwlock_rp *, _aqs_v1_rcount);

extern void aqs_v1_read_lock_slowpath_rp(struct aqs_v1_rwlock_rp *lock, bool dcount);
extern void aqs_v1_write_lock_slowpath_rp(struct aqs_v1_rwlock_rp *lock, bool dcount);

/* returns 1 if acquired, 0 if failed */
static inline int aqs_v1_read_trylock_rp(struct aqs_v1_rwlock_rp *lock)
{
	u32 cnts;

	cnts = atomic_read(&lock->cnts);
	if (likely(!(cnts & _AQS_V1_RP_W_WMASK))) {
		if (likely(cnts & _AQS_V1_RP_R_RPFD)) {
			/* this one is per-cpu reader preferred */
			this_cpu_write(_aqs_v1_rcount, lock);
			if (likely(!(atomic_read(&lock->cnts) & _AQS_V1_RP_W_WMASK)))
				return 1;
			this_cpu_write(_aqs_v1_rcount, NULL);
			goto out;
		}

		cnts = (u32)atomic_add_return_acquire(_AQS_V1_RP_R_BIAS, &lock->cnts);
		if (likely(!(cnts & _AQS_V1_RP_W_WMASK)))
			return 1;
		atomic_sub(_AQS_V1_RP_R_BIAS, &lock->cnts);
	}
     out:
	return 0;
}

static inline int __check_active_readers_v1(struct aqs_v1_rwlock_rp *lock)
{
	int cpu;

	for_each_online_cpu(cpu) {
		if (per_cpu(_aqs_v1_rcount, cpu) == lock)
			return true;
	}
	return false;
}

static inline int aqs_v1_write_trylock_rp(struct aqs_v1_rwlock_rp *lock)
{
	u32 cnts;
	int ret = 0;

	cnts = atomic_read(&lock->cnts);
	if (unlikely(cnts & ~(_AQS_V1_RP_R_RPFD)))
		return 0;

	ret = atomic_cmpxchg_acquire(&lock->cnts,
				     cnts, cnts | _AQS_V1_RP_W_LOCKED);
	if (likely(ret == cnts)) {

		if (!(cnts & _AQS_V1_RP_R_RPFD))
			return 1;

		if (!__check_active_readers_v1(lock))
			return 1;
		else {
			/* We have to revert it */
			smp_store_release(&lock->wlocked, _AQS_V1_RP_R_RPFD);
		}
	}
	return 0;
}

static inline void aqs_v1_read_lock_rp(struct aqs_v1_rwlock_rp *lock)
{
	u32 cnts;
	u8 wlocked;

	wlocked = READ_ONCE(lock->wlocked);
	if (wlocked & _AQS_V1_RP_R_RPFD) {

		this_cpu_write(_aqs_v1_rcount, lock);

		if (likely(!((wlocked & _AQS_V1_RP_W_WMASK) >> _AQS_V1_RP_R_RPFD)))
			return;

		aqs_v1_read_lock_slowpath_rp(lock, true);
		return;
	}

	cnts = atomic_add_return_acquire(_AQS_V1_RP_R_BIAS, &lock->cnts);
	if (likely(!(cnts & _AQS_V1_RP_W_WMASK)))
		return;

	aqs_v1_read_lock_slowpath_rp(lock, false);
}

static void wait_for_active_readers_v1(struct aqs_v1_rwlock_rp *lock)
{
	int cpu;

	for_each_online_cpu(cpu) {
		while (per_cpu(_aqs_v1_rcount, cpu) == lock)
			cpu_relax();
                cpu_relax();
	}
}

static inline void aqs_v1_write_lock_rp(struct aqs_v1_rwlock_rp *lock)
{
	u32 cnts;

	cnts = atomic_read(&lock->cnts);
	if (cnts & _AQS_V1_RP_R_RPFD) {
		if (atomic_cmpxchg_acquire(&lock->cnts, cnts,
					   cnts | _AQS_V1_RP_W_LOCKED) == cnts) {
			wait_for_active_readers_v1(lock);
			return;
		}
		aqs_v1_write_lock_slowpath_rp(lock, true);
		return;
	}

	if (atomic_cmpxchg_acquire(&lock->cnts, 0, _AQS_V1_RP_W_LOCKED) == 0)
		return;

	aqs_v1_write_lock_slowpath_rp(lock, false);
}

static inline void aqs_v1_read_unlock_rp(struct aqs_v1_rwlock_rp *lock)
{
	if (this_cpu_read(_aqs_v1_rcount) == lock) {
		this_cpu_write(_aqs_v1_rcount, NULL);
		return;
	}
	(void)atomic_sub_return_release(_AQS_V1_RP_R_BIAS, &lock->cnts);
}

static inline void aqs_v1_write_unlock_rp(struct aqs_v1_rwlock_rp *lock)
{
	if (READ_ONCE(lock->wlocked) & _AQS_V1_RP_R_RPFD) {
		smp_store_release(&lock->wlocked, _AQS_V1_RP_R_RPFD);
        } else
		smp_store_release(&lock->wlocked, 0);
}

static inline void aqs_v1_set_distributed_rp(struct aqs_v1_rwlock_rp *lock)
{
        smp_store_release(&lock->wlocked, _AQS_V1_RP_R_RPFD);
}

#endif /* _RW_AQS_V1_RP_H */
