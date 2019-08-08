
#include <linux/atomic.h>
#include <linux/compiler.h>
#include <linux/topology.h>
#include "rwlock/rwaqs_rp.h"

DEFINE_PER_CPU_ALIGNED(struct aqs_rwlock_rp *, _aqs_rcount);
EXPORT_SYMBOL(_aqs_rcount);

void aqs_read_lock_slowpath_rp(struct aqs_rwlock_rp *lock, bool dcount)
{

	if (dcount) {

		this_cpu_write(_aqs_rcount, NULL);
                for(;;) {
                        while (aqs_spin_is_locked(&lock->wait_lock))
                                cpu_relax();

                        this_cpu_write(_aqs_rcount, lock);

                        if (!aqs_spin_is_locked(&lock->wait_lock))
                                return;

                        __this_cpu_write(_aqs_rcount, NULL);
                }
#if 0
		for (;;) {
			atomic_cond_read_acquire(&lock->cnts, !((VAL & _AQS_RP_W_LOCKED) & ~(_AQS_RP_R_RPFD)));

			this_cpu_write(_aqs_rcount, lock);
			smp_wmb();

			if (!(READ_ONCE(lock->wlocked) & ~(_AQS_RP_R_RPFD)))
				return;
			this_cpu_write(_aqs_rcount, NULL);
			smp_wmb();
			cpu_relax();
		}
#endif
	}

	if (unlikely(in_interrupt())) {

		atomic_cond_read_acquire(&lock->cnts, !(VAL & _AQS_RP_W_LOCKED));
		return;
	}
	atomic_sub(_AQS_RP_R_BIAS, &lock->cnts);

	aqs_spin_lock(&lock->wait_lock);
	atomic_add(_AQS_RP_R_BIAS, &lock->cnts);

	atomic_cond_read_acquire(&lock->cnts, !(VAL & _AQS_RP_W_LOCKED));

	aqs_spin_unlock(&lock->wait_lock);
}
EXPORT_SYMBOL(aqs_read_lock_slowpath_rp);

static void wait_for_active_readers(struct aqs_rwlock_rp *lock)
{
	int cpu;

	for_each_online_cpu(cpu) {
		while (per_cpu(_aqs_rcount, cpu) == lock)
			cpu_relax();
                cpu_relax();
	}
}


void aqs_write_lock_slowpath_rp(struct aqs_rwlock_rp *lock, bool dcount)
{
	if (dcount) {
                aqs_spin_lock(&lock->wait_lock);
                wait_for_active_readers(lock);
                return;
#if 0
		aqs_spin_lock(&lock->wait_lock);

		if (!(atomic_read(&lock->cnts) & ~(_AQS_RP_R_RPFD)) &&
		    (atomic_cmpxchg_acquire(&lock->cnts, _AQS_RP_R_RPFD,
					    _AQS_RP_W_LOCKED | _AQS_RP_R_RPFD) == _AQS_RP_R_RPFD))
			goto unlock;

		atomic_or(_AQS_RP_W_WAITING, &lock->cnts);

		do {
			atomic_cond_read_acquire(&lock->cnts,
				VAL == (_AQS_RP_W_WAITING | _AQS_RP_R_RPFD));
		} while (atomic_cmpxchg_relaxed(&lock->cnts,
						(_AQS_RP_W_WAITING | _AQS_RP_R_RPFD),
						(_AQS_RP_W_LOCKED | _AQS_RP_R_RPFD)) !=
			 			(_AQS_RP_W_WAITING | _AQS_RP_R_RPFD));
		goto unlock;
#endif
	}

	aqs_spin_lock(&lock->wait_lock);

	if (!atomic_read(&lock->cnts) &&
	    (atomic_cmpxchg_acquire(&lock->cnts, 0, _AQS_RP_W_LOCKED) == 0))
		goto unlock;

	atomic_add(_AQS_RP_W_WAITING, &lock->cnts);

	do {
		atomic_cond_read_acquire(&lock->cnts, VAL == _AQS_RP_W_WAITING);
	} while (atomic_cmpxchg_relaxed(&lock->cnts, _AQS_RP_W_WAITING,
					_AQS_RP_W_LOCKED) != _AQS_RP_W_WAITING);

     unlock:
	aqs_spin_unlock(&lock->wait_lock);
}
EXPORT_SYMBOL(aqs_write_lock_slowpath_rp);
