
#include <linux/atomic.h>
#include <linux/compiler.h>
#include <linux/topology.h>
#include "rwlock/rwaqs_rp_v1.h"

DEFINE_PER_CPU_ALIGNED(struct aqs_v1_rwlock_rp *, _aqs_v1_rcount);
EXPORT_SYMBOL(_aqs_v1_rcount);

void aqs_v1_read_lock_slowpath_rp(struct aqs_v1_rwlock_rp *lock, bool dcount)
{

	if (dcount) {

		this_cpu_write(_aqs_v1_rcount, NULL);
		for (;;) {
			atomic_cond_read_acquire(&lock->cnts, !((VAL & _AQS_V1_RP_W_LOCKED) & ~(_AQS_V1_RP_R_RPFD)));

			this_cpu_write(_aqs_v1_rcount, lock);

			if (!(READ_ONCE(lock->wlocked) & ~(_AQS_V1_RP_R_RPFD)))
				return;

			this_cpu_write(_aqs_v1_rcount, NULL);
			cpu_relax();
		}
	}

	if (unlikely(in_interrupt())) {

		atomic_cond_read_acquire(&lock->cnts, !(VAL & _AQS_V1_RP_W_LOCKED));
		return;
	}
	atomic_sub(_AQS_V1_RP_R_BIAS, &lock->cnts);

	aqs_spin_lock(&lock->wait_lock);
	atomic_add(_AQS_V1_RP_R_BIAS, &lock->cnts);

	atomic_cond_read_acquire(&lock->cnts, !(VAL & _AQS_V1_RP_W_LOCKED));

	aqs_spin_unlock(&lock->wait_lock);
}
EXPORT_SYMBOL(aqs_v1_read_lock_slowpath_rp);

void aqs_v1_write_lock_slowpath_rp(struct aqs_v1_rwlock_rp *lock, bool dcount)
{
	if (dcount) {
		aqs_spin_lock(&lock->wait_lock);

		if (!(atomic_read(&lock->cnts) & ~(_AQS_V1_RP_R_RPFD)) &&
		    (atomic_cmpxchg_acquire(&lock->cnts, _AQS_V1_RP_R_RPFD,
					    _AQS_V1_RP_W_LOCKED | _AQS_V1_RP_R_RPFD) == _AQS_V1_RP_R_RPFD))
			goto unlock;

		atomic_or(_AQS_V1_RP_W_WAITING, &lock->cnts);

		do {
			atomic_cond_read_acquire(&lock->cnts,
				VAL == (_AQS_V1_RP_W_WAITING | _AQS_V1_RP_R_RPFD));
		} while (atomic_cmpxchg_relaxed(&lock->cnts,
						(_AQS_V1_RP_W_WAITING | _AQS_V1_RP_R_RPFD),
						(_AQS_V1_RP_W_LOCKED | _AQS_V1_RP_R_RPFD)) !=
			 			(_AQS_V1_RP_W_WAITING | _AQS_V1_RP_R_RPFD));
		goto unlock;
	}

	aqs_spin_lock(&lock->wait_lock);

	if (!atomic_read(&lock->cnts) &&
	    (atomic_cmpxchg_acquire(&lock->cnts, 0, _AQS_V1_RP_W_LOCKED) == 0))
		goto unlock;

	atomic_add(_AQS_V1_RP_W_WAITING, &lock->cnts);

	do {
		atomic_cond_read_acquire(&lock->cnts, VAL == _AQS_V1_RP_W_WAITING);
	} while (atomic_cmpxchg_relaxed(&lock->cnts, _AQS_V1_RP_W_WAITING,
					_AQS_V1_RP_W_LOCKED) != _AQS_V1_RP_W_WAITING);

     unlock:
	aqs_spin_unlock(&lock->wait_lock);
}
EXPORT_SYMBOL(aqs_v1_write_lock_slowpath_rp);
