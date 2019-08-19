#include <linux/atomic.h>
#include <linux/compiler.h>
#include <linux/topology.h>
#include "rwlock/rwaqs_ntrl.h"

void aqs_read_lock_slowpath_ntrl(struct aqs_rwlock_ntrl *lock)
{
        if (unlikely(in_interrupt())) {

                atomic_cond_read_acquire(&lock->cnts, !(VAL & _AQS_W_LOCKED));
                return;
        }
        atomic_sub(_AQS_R_BIAS, &lock->cnts);

        aqs_spin_lock(&lock->wait_lock);
        atomic_add(_AQS_R_BIAS, &lock->cnts);

        atomic_cond_read_acquire(&lock->cnts, !(VAL & _AQS_W_LOCKED));

        aqs_spin_unlock(&lock->wait_lock);
}


void aqs_write_lock_slowpath_ntrl(struct aqs_rwlock_ntrl *lock)
{
        aqs_spin_lock(&lock->wait_lock);

        if (!atomic_read(&lock->cnts) &&
            (atomic_cmpxchg_acquire(&lock->cnts, 0, _AQS_W_LOCKED) == 0))
                goto unlock;

        atomic_add(_AQS_W_WAITING, &lock->cnts);

        do {
                atomic_cond_read_acquire(&lock->cnts, VAL == _AQS_W_WAITING);
        } while (atomic_cmpxchg_relaxed(&lock->cnts, _AQS_W_WAITING,
                                        _AQS_W_LOCKED) != _AQS_W_WAITING);

     unlock:
        aqs_spin_unlock(&lock->wait_lock);
}
