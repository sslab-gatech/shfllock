#ifndef _RW_AQS_WRP_RRP_H
#define _RW_AQS_WRP_RRP_H

#include "spinlock/aqs.h"

/*
 * Writer states & reader shift and bias.
 */
#define _AQS_WRP_RRP_R_OFFSET      0
#define _AQS_WRP_RRP_R_BITS        16
#define _AQS_WRP_RRP_R_MASK        _AQS_SET_MASK(WRP_RRP_R)
#define _AQS_WRP_RRP_R_BIAS	    (1U << _AQS_WRP_RRP_R_OFFSET)
#define _AQS_WRP_RRP_RDIMP_OFFSET  (_AQS_WRP_RRP_R_OFFSET + _AQS_WRP_RRP_R_BITS)
#define _AQS_WRP_RRP_RDIMP_BITS    (32 - _AQS_WRP_RRP_R_BITS)
#define _AQS_WRP_RRP_RDIMP_MASK    _AQS_SET_MASK(WRP_RRP_RDIMP)
#define MAX_IMPATEINCE              8
#define ONLINE_RD_RATIO             4

struct aqs_rwlock_wrp_rrp {
        union {
                atomic_t cnts;
#ifdef __LITTLE_ENDIAN
                struct {
                        u8 __lstate[2];
                        u8 rd_imp;
                        u8 rd_prfd;
                };
#else
                struct {
                        u8 rd_prfd;
                        u8 rd_imp;
                        u8 __lstate[2];
                };
#endif
        };
        struct aqs_lock wait_lock;
};

#define __RW_AQS_WRP_RRP_LOCK_UNLOCKED {                        \
        { .cnts = ATOMIC_INIT(0), },                            \
        .wait_lock = __AQS_LOCK_UNLOCKED,                       \
}

#define DEFINE_AQS_RWLOCK_WRP_RRP(x)                            \
        struct aqs_rwlock_wrp_rrp x = __RW_AQS_WRP_RRP_LOCK_UNLOCKED;

static DEFINE_PER_CPU_ALIGNED(struct aqs_rwlock_wrp_rrp *, rwlock_ptr);

/* returns 1 if acquired, 0 if failed */
static inline int aqs_read_trylock_wrp_rrp(struct aqs_rwlock_wrp_rrp *lock)
{
        if (!in_interrupt() && smp_load_acquire(&lock->rd_prfd)) {
                this_cpu_write(rwlock_ptr, lock);
                if (aqs_spin_is_locked(&lock->wait_lock)) {
                        __this_cpu_write(rwlock_ptr, NULL);
                        return 0;
                }
                return 1;
        }

        atomic_add_return_acquire(_AQS_WRP_RRP_R_BIAS, &lock->cnts);

        if (aqs_spin_is_locked(&lock->wait_lock)) {
                atomic_sub(_AQS_WRP_RRP_R_BIAS, &lock->cnts);
                return 0;
        }
        return 1;
}

static inline int aqs_write_trylock_wrp_rrp(struct aqs_rwlock_wrp_rrp *lock)
{
        u32 ret;

        ret = aqs_spin_trylock(&lock->wait_lock);
        if (unlikely(!ret))
                goto out;

        if (!in_interrupt() && smp_load_acquire(&lock->rd_prfd)) {
                int cpu;
                for_each_online_cpu(cpu) {

                        if (cpu == smp_processor_id())
                                continue;

                        if (per_cpu(rwlock_ptr, cpu) == lock) {
                                aqs_spin_unlock(&lock->wait_lock);
                                goto out;
                        }
                }
        } else {
                if (atomic_read(&lock->cnts) & _AQS_WRP_RRP_R_MASK) {
                        aqs_spin_unlock(&lock->wait_lock);
                        goto out;
                }
        }
        return 0;

     out:
        return 1;
}

static inline void aqs_read_lock_wrp_rrp(struct aqs_rwlock_wrp_rrp *lock)
{
        int imp_count = MAX_IMPATEINCE;
        int readers = 0;
        int max_online_cpus = 0;
        int cpus = 0;

     /* recheck: */
        if (!in_interrupt() && smp_load_acquire(&lock->rd_imp)) {
                this_cpu_write(rwlock_ptr, lock);

                while(aqs_spin_is_locked(&lock->wait_lock))
                        cpu_relax();

                return;
        }

        for (;;) {
                // Wait untill the wait lock has been acquired
                while (aqs_spin_is_locked(&lock->wait_lock)) {
                        cpu_relax();
                }

                if (!cpus) {
                        for_each_online_cpu(cpus)
                                max_online_cpus++;
                }
                // Now increment the count
                readers = atomic_add_return_acquire(_AQS_WRP_RRP_R_BIAS, &lock->cnts);

                if (readers > max_online_cpus / ONLINE_RD_RATIO) {
                        WRITE_ONCE(lock->rd_prfd, 1);
                }

                // While incrementing the count, there is a possibility
                // that writer already acquired the lock,
                // so decreament the count and try again!
                if (aqs_spin_is_locked(&lock->wait_lock)) {
                        atomic_sub(_AQS_WRP_RRP_R_BIAS, &lock->cnts);
                        if (imp_count && --imp_count == 0) {
                                WRITE_ONCE(lock->rd_imp, 1);
                        }
                        continue;
                }

                // got the read lock, should break
                break;
        }
        if (!imp_count)
                WRITE_ONCE(lock->rd_imp, 0);
}

static inline void aqs_write_lock_wrp_rrp(struct aqs_rwlock_wrp_rrp *lock)
{
        // got the write lock, now wait for the cnts to become 0
        aqs_spin_lock(&lock->wait_lock);

        if (!in_interrupt() && smp_load_acquire(&lock->rd_imp)) {
                int cpu;
                for_each_online_cpu(cpu) {
                        for (;;) {
                                if (per_cpu(rwlock_ptr, cpu) != lock)
                                        break;
                                cpu_relax();
                        }
                }
                return;
        }

        for (;;) {
                if (!atomic_read(&lock->cnts))
                        break;
                cpu_relax();
        }
}

static inline void aqs_read_unlock_wrp_rrp(struct aqs_rwlock_wrp_rrp *lock)
{
        if (!in_interrupt() && smp_load_acquire(&lock->rd_imp)) {
                this_cpu_write(rwlock_ptr, NULL);
                return;
        }
        (void)atomic_sub_return_release(_AQS_WRP_RRP_R_BIAS, &lock->cnts);
}

static inline void aqs_write_unlock_wrp_rrp(struct aqs_rwlock_wrp_rrp *lock)
{
        aqs_spin_unlock(&lock->wait_lock);
}

#endif /* _RW_AQS_WRP_RRP_H */
