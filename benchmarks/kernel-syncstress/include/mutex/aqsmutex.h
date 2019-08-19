#ifndef _AQS_MUTEX_H
#define _AQS_MUTEX_H

#include "include/spinlock/aqs.h"

struct aqs_mutex_node {
        struct aqs_mutex_node *next;
        int lstatus;
        int sleader;
        int wcount;

        int nid;
        int count;

        struct aqs_mutex_node *last_visited;
};

struct aqs_mutex {
        /* These two will be sufficient to design simple aqs lock */
        struct aqs_mutex_node   *tail;
        union {
                atomic_t        state;
#ifdef __LITTLE_ENDIAN
                struct {
                        u8      locked;
                        u8      no_stealing;
                };
                struct {
                        u16     locked_no_stealing;
                };
#else
                struct {
                        u8      reserved0[2];
                        u8      no_stealing;
                        u8      locked;
                };
                struct {
                        u16     reserved1[1];
                        u16 locked_no_stealing;
                };
#endif
        };

        /* The challenge lies in designing waiting part */
        struct aqs_mutex_node   *wtail;
};

#define __AQSMUTEX_INITIALIZER(lockname) \
        { .tail = NULL \
        , .state = ATOMIC_INIT(0) \
        , .wtail = NULL \
        }

#define DEFINE_AQSMUTEX(mname)  \
        struct aqs_mutex mname = __AQSMUTEX_INITIALIZER(mname)

extern void __aqsmutex_init(struct aqs_mutex *lock, const char *name,
                            struct lock_class_key *key);

static inline bool aqsmutex_is_locked(struct aqs_mutex *lock)
{
        return !!READ_ONCE(lock->locked);
}

extern void __aqsmutex_lock(struct aqs_mutex *lock);
extern int __must_check aqsmutex_lock_interruptible(struct aqs_mutex *lock);
extern int __must_check aqsmutex_lock_killable(struct aqs_mutex *lock);
extern void aqsmutex_lock_io(struct aqs_mutex *lock);

void aqsmutex_lock(struct aqs_mutex *lock)
{
        int ret;

        ret = cmpxchg(&lock->locked_no_stealing, 0, 1);
        if (likely(ret == 0))
                return;
        __aqsmutex_lock(lock);
}

#define aqsmutex_lock_nested(lock, subclass) aqsmutex_lock(lock)
#define aqsmutex_lock_interruptible_nested(lock, subclass) aqsmutex_lock_interruptible(lock)
#define aqsmutex_lock_killable_nested(lock, subclass) aqsmutex_lock_killable(lock)
#define aqsmutex_lock_nest_lock(lock, nest_lock) aqsmutex_lock(lock)
#define aqsmutex_lock_io_nested(lock, subclass) aqsmutex_lock(lock)

static int aqsmutex_trylock(struct aqs_mutex *lock)
{
        if (!READ_ONCE(lock->locked_no_stealing) &&
            cmpxchg(&lock->locked_no_stealing, 0, 1) == 0)
                return 1;

        return 0;
}

extern void aqsmutex_unlock(struct aqs_mutex *lock);

extern int atomic_dec_and_aqsmutex_lock(atomic_t *cnt, struct aqs_mutex *lock);

#endif /* _AQS_MUTEX_H */
