#ifndef _AQM_H_
#define _AQM_H_

/*
 * Bit manipulation (not used currently)
 * Will use just one variable of 4 byts to enclose the following:
 * 0-7:   locked or unlocked
 * 8-15:  shuffle leader or not
 * 16-31: shuffle count
 */
#define _AQ_MCS_SET_MASK(type)  (((1U << _AQ_MCS_ ## type ## _BITS) -1)\
                                 << _AQ_MCS_ ## type ## _OFFSET)
#define _AQ_MCS_GET_VAL(v, type)   (((v) & (_AQ_MCS_ ## type ## _MASK)) >>\
                                    (_AQ_MCS_ ## type ## _OFFSET))
#define _AQ_MCS_LOCKED_OFFSET   0
#define _AQ_MCS_LOCKED_BITS     8
#define _AQ_MCS_LOCKED_MASK     _AQ_MCS_SET_MASK(LOCKED)
#define _AQ_MCS_LOCKED_VAL(v)   _AQ_MCS_GET_VAL(v, LOCKED)

#define _AQ_MCS_SLEADER_OFFSET  (_AQ_MCS_LOCKED_OFFSET + _AQ_MCS_LOCKED_BITS)
#define _AQ_MCS_SLEADER_BITS    8
#define _AQ_MCS_SLEADER_MASK    _AQ_MCS_SET_MASK(SLEADER)
#define _AQ_MCS_SLEADER_VAL(v)  _AQ_MCS_GET_VAL(v, SLEADER)

#define _AQ_MCS_WCOUNT_OFFSET   (_AQ_MCS_SLEADER_OFFSET + _AQ_MCS_SLEADER_BITS)
#define _AQ_MCS_WCOUNT_BITS     16
#define _AQ_MCS_WCOUNT_MASK     _AQ_MCS_SET_MASK(WCOUNT)
#define _AQ_MCS_WCOUNT_VAL(v)   _AQ_MCS_GET_VAL(v, WCOUNT)

#define _AQ_MCS_NOSTEAL_VAL     (1U << (_AQ_MCS_LOCKED_OFFSET + _AQ_MCS_LOCKED_BITS))

#define _AQ_MCS_STATUS_PARKED   0 /* node's status is changed to park */
#define _AQ_MCS_STATUS_PWAIT    1 /* starting point for everyone */
#define _AQ_MCS_STATUS_UNPWAIT  2 /* waiter is never scheduled out in this state */
#define _AQ_MCS_STATUS_LOCKED   4 /* node is now going to be the lock holder */
#define _AQ_MAX_LOCK_COUNT      256u

struct aqm_node {
	struct aqm_node *next;

	union {
		unsigned int locked;
		struct {
			u8  lstatus;
			u8  sleader;
			u16 wcount;
		};
	};

	int nid;
	int cid;
        unsigned int rv;
	struct aqm_node *last_visited;
        struct task_struct *task;
};


struct aqm_mutex {
	struct aqm_node *tail;
	union {
		atomic_t val;
#ifdef __LITTLE_ENDIAN
		struct {
			u8  locked;
			u8  no_stealing;
                        u8  nid;
                        u8  disable_shuffle;
		};
		struct {
			u16 locked_no_stealing;
		};
#else
		struct {
			u8  __unused[2];
			u8  no_stealing;
			u8  locked;
		};
		struct {
			u16 __unused2;
			u16 locked_no_stealing;
		};
#endif
#ifdef CONFIG_DEBUG_MUTEXES
		void *magic;
#endif
#ifdef CONFIG_DEBUG_LOCK_ALLOC
		struct lockdep_map dep_map;
#endif
	};
};


#define __AQM_MUTEX_INITIALIZER(lockname) { 				\
	.tail = NULL, 							\
        .val = ATOMIC_INIT(0), 						\
}

#define DEFINE_AQM(mutexname)                                           \
	struct aqm_mutex mutexname = __AQM_MUTEX_INITIALIZER(mutexname);

extern void __aqm_init(struct aqm_mutex *lock, const char *name,
		       struct lock_class_key *key);

static inline bool aqm_is_locked(struct aqm_mutex *lock)
{
	return READ_ONCE(lock->locked);
}

extern void aqm_lock(struct aqm_mutex *lock);
extern void aqm_lock_w_node(struct aqm_mutex *lock, struct aqm_node *node, int type);
/* extern int __must_check aqm_lock_interruptible(struct aqm_mutex *lock); */
/* extern int __must_check aqm_lock_killable(struct aqm_mutex *lock); */
/* extern void aqm_lock_io(struct aqm_mutex *lock); */

/* # define aqm_lock_nested(lock, subclass) aqm_lock(lock) */
/* # define aqm_lock_interruptible_nested(lock, subclass) aqm_lock_interruptible(lock) */
/* # define aqm_lock_killable_nested(lock, subclass) aqm_lock_killable(lock) */
/* # define aqm_lock_nest_lock(lock, nest_lock) aqm_lock(lock) */
/* # define aqm_lock_io_nested(lock, subclass) aqm_lock(lock) */

extern int aqm_trylock(struct aqm_mutex *lock);
extern int aqm_unlock(struct aqm_mutex *lock);

DECLARE_PER_CPU(u64, offpath);
DECLARE_PER_CPU(u64, offpath2);
DECLARE_PER_CPU(u64, critpath);
DECLARE_PER_CPU(u64, fpath);
DECLARE_PER_CPU(u64, spath);

/* extern int atomic_dec_aqm_lock(atomic_t *cnt, struct aqm_mutex *lock); */

#endif /* _AQM_H_ */
