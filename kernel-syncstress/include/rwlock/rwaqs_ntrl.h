#ifndef _RW_AQS_NTRL_H
#define _RW_AQS_NTRL_H

#include "spinlock/aqs.h"

/*
 * Writer states & reader shift and bias.
 */
#define	_AQS_W_WAITING	0x100		/* A writer is waiting	   */
#define	_AQS_W_LOCKED	0x0ff		/* A writer holds the lock */
#define	_AQS_W_WMASK	0x1ff		/* Writer mask		   */
#define	_AQS_R_SHIFT	9		/* Reader count shift	   */
#define _AQS_R_BIAS	(1U << _AQS_R_SHIFT)


struct aqs_rwlock_ntrl {
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

#define __RW_AQS_NTRL_LOCK_UNLOCKED {                           \
        { .cnts = ATOMIC_INIT(0) },                             \
        .wait_lock = __AQS_LOCK_UNLOCKED,                       \
        }

#define DEFINE_AQS_RWLOCK_NTRL(x) \
        struct aqs_rwlock_ntrl x = __RW_AQS_NTRL_LOCK_UNLOCKED;

extern void aqs_read_lock_slowpath_ntrl(struct aqs_rwlock_ntrl *lock);
extern void aqs_write_lock_slowpath_ntrl(struct aqs_rwlock_ntrl *lock);

/* returns 1 if acquired, 0 if failed */
static inline int aqs_read_trylock_ntrl(struct aqs_rwlock_ntrl *lock)
{
        u32 cnts;

        cnts = atomic_read(&lock->cnts);
        if (likely(!(cnts & _AQS_W_WMASK))) {
                cnts = (u32)atomic_add_return_acquire(_AQS_R_BIAS, &lock->cnts);
                if (likely(!(cnts & _AQS_W_WMASK)))
                        return 1;
                atomic_sub(_AQS_R_BIAS, &lock->cnts);
        }
        return 0;
}

static inline int aqs_write_trylock_ntrl(struct aqs_rwlock_ntrl *lock)
{
        u32 cnts;

        cnts = atomic_read(&lock->cnts);
        if (unlikely(cnts))
                return 0;

        return likely(atomic_cmpxchg_acquire(&lock->cnts,
                                    cnts, cnts | _AQS_W_LOCKED) == cnts);
}

static inline void aqs_read_lock_ntrl(struct aqs_rwlock_ntrl *lock)
{
        u32 cnts;

        cnts = atomic_add_return_acquire(_AQS_R_BIAS, &lock->cnts);
        if (likely(!(cnts & _AQS_W_WMASK)))
                return;

        aqs_read_lock_slowpath_ntrl(lock);
}

static inline void aqs_write_lock_ntrl(struct aqs_rwlock_ntrl *lock)
{
        if (atomic_cmpxchg_acquire(&lock->cnts, 0, _AQS_W_LOCKED) == 0)
                return;

        aqs_write_lock_slowpath_ntrl(lock);
}

static inline void aqs_read_unlock_ntrl(struct aqs_rwlock_ntrl *lock)
{
        (void)atomic_sub_return_release(_AQS_R_BIAS, &lock->cnts);
}

static inline void aqs_write_unlock_ntrl(struct aqs_rwlock_ntrl *lock)
{
        smp_store_release(&lock->wlocked, 0);
}

#endif /* _RW_AQS_NTRL_H */
