#ifndef _RW_AQS_WRP_H
#define _RW_AQS_WRP_H

#include "spinlock/aqs.h"

/*
 * Writer states & reader shift and bias.
 */
#define _AQS_WRP_R_OFFSET      0
#define _AQS_WRP_R_BITS        24
#define _AQS_WRP_R_MASK        _AQS_SET_MASK(WRP_R)
#define _AQS_WRP_R_BIAS	        (1U << _AQS_WRP_R_OFFSET)
#define _AQS_WRP_RDIMP_OFFSET  (_AQS_WRP_R_OFFSET + _AQS_WRP_R_BITS)
#define _AQS_WRP_RDIMP_BITS    (32 - _AQS_WRP_R_BITS)
#define _AQS_WRP_RDIMP_MASK    _AQS_SET_MASK(WRP_RDIMP)
#define MAX_IMPATEINCE          8


struct aqs_rwlock_wrp {
        union {
                atomic_t cnts;
#ifdef __LITTLE_ENDIAN
                struct {
                        u8 __lstate[3];
                        u8 rd_imp;
                };
#else
                struct {
                        u8 rd_imp;
                        u8 __lstate[3];
                };
#endif
        };
        struct aqs_lock wait_lock;
};

#define __RW_AQS_WRP_LOCK_UNLOCKED {                            \
        { .cnts = ATOMIC_INIT(0), },                            \
        .wait_lock = __AQS_LOCK_UNLOCKED,                       \
}

#define DEFINE_AQS_RWLOCK_WRP(x)                                \
        struct aqs_rwlock_wrp x = __RW_AQS_WRP_LOCK_UNLOCKED;

/* returns 1 if acquired, 0 if failed */
static inline int aqs_read_trylock_wrp(struct aqs_rwlock_wrp *lock)
{
        atomic_add_return_acquire(_AQS_WRP_R_BIAS, &lock->cnts);

        if (aqs_spin_is_locked(&lock->wait_lock)) {
                atomic_sub(_AQS_WRP_R_BIAS, &lock->cnts);
                return 0;
        }
        return 1;
}

static inline int aqs_write_trylock_wrp(struct aqs_rwlock_wrp *lock)
{
        u32 ret;

        ret = aqs_spin_trylock(&lock->wait_lock);
        if (unlikely(!ret))
                goto out;

        if (atomic_read(&lock->cnts) & _AQS_WRP_R_MASK) {
                aqs_spin_unlock(&lock->wait_lock);
                goto out;
        }
        return 0;

     out:
        return 1;
}

static inline void aqs_read_lock_wrp(struct aqs_rwlock_wrp *lock)
{
        int imp_count = MAX_IMPATEINCE;
        for (;;) {
                // Wait until the wait lock has been acquired
                while (aqs_spin_is_locked(&lock->wait_lock))
                        cpu_relax();

                // Now increment the count
                atomic_add_return_acquire(_AQS_WRP_R_BIAS, &lock->cnts);

                // While incrementing the count, there is a possibility
                // that writer already acquired the lock,
                // so decreament the count and try again!
                /* if (aqs_spin_is_locked(&lock->wait_lock)) { */
                /*         atomic_sub(_AQS_WRP_R_BIAS, &lock->cnts); */
                /*         continue; */
                /* } */

                // got the read lock, should break
                /* break; */
                if (!aqs_spin_is_locked(&lock->wait_lock))
                        return;
                atomic_sub(_AQS_WRP_R_BIAS, &lock->cnts);
        }
        /* if (!imp_count) */
        /*         WRITE_ONCE(lock->rd_imp, 0); */
}

static inline void aqs_write_lock_wrp(struct aqs_rwlock_wrp *lock)
{
        // got the write lock, now wait for the cnts to become 0
        __aqs_acquire(&lock->wait_lock);

        for (;;) {
                if (!atomic_read(&lock->cnts))
                        break;
                cpu_relax();
        }
}

static inline void aqs_read_unlock_wrp(struct aqs_rwlock_wrp *lock)
{
        (void)atomic_sub_return_release(_AQS_WRP_R_BIAS, &lock->cnts);
}

static inline void aqs_write_unlock_wrp(struct aqs_rwlock_wrp *lock)
{
        __aqs_release(&lock->wait_lock);
}

#endif /* _RW_AQS_WRP_H */
