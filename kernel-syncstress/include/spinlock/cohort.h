#ifndef _CHRT_MCSMCS_H_
#define _CHRT_MCSMCS_H_
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

struct mcs_node {
    struct mcs_node *next;
    union {
        u32 locked;
        struct {
            u8 lstatus;
            u8 sleader;
            u16 wcount;
        };
    };

    int nid;
    int cid;
    int count;
};

struct mcs_lock_new {
        union {
                atomic_t val;
#ifdef __LITTLE_ENDIAN
                struct {
                        u8	locked;
                        u8	pending;
                };
                struct {
                        u16     locked_pending;
                        u16     tail;
                };
#else
                struct {
                        u8      reserved[2];
                        u8	pending;
                        u8	locked;
                };
                struct {
                        u16     tail;
                        u16     locked_pending;
                };
#endif
        };
};

#define inc_amcs_stat(v) __this_cpu_inc(amcs_stat.stats[v])
#define read_amcs_stat(v) __this_cpu_read(amcs_stat.stats[v])
#define write_amcs_stat(v, d) __this_cpu_write(amcs_stat.stats[v], d)

typedef enum {
        fastpath = 0,
        slowpath,
        nw_shuffles,
        w_shuffles,
        max_shuffles,
        num_entries,
} astat;

struct amcs_stat {
        u64 stats[num_entries];
};

struct amcs_stat_name {
        astat value;
        char *name;
};

DECLARE_PER_CPU_ALIGNED(struct amcs_stat, amcs_stat);

static __always_inline
int mcs_is_locked_new(struct mcs_lock_new *lock)
{
	return READ_ONCE(lock->locked);
}

extern void _mcs_acquire_new(struct mcs_lock_new *lock);

static __always_inline
void amcs_spin_lock_new(struct mcs_lock_new *lock)
{
        int ret;

        ret = cmpxchg(&lock->locked_pending, 0, 1);
        if (likely(ret == 0)) {
                inc_amcs_stat(fastpath);
                return;
        }
        _mcs_acquire_new(lock);
        inc_amcs_stat(slowpath);
}

static __always_inline
void amcs_spin_unlock_new(struct mcs_lock_new *lock)
{
	/*
	 * smp_mb__before_atomic() in order to guarantee release semantics
	 */
        smp_store_release(&lock->locked, 0);
}

static __always_inline
int amcs_spin_trylock_new(struct mcs_lock_new *lock)
{
        if (!READ_ONCE(lock->locked_pending) &&
            cmpxchg(&lock->locked_pending, 0, 1) == 0)
                return 1;

        return 0;
}
#endif /* _CHRT_MCSMCS_H_ */
