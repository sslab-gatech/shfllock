#ifndef __LINUX_RWRWAQM_H
#define __LINUX_RWRWAQM_H

#include <asm/current.h>
#include <linux/spinlock_types.h>
#include <linux/atomic.h>
#include <asm/processor.h>

/*
 * Writer states & reader shift and bias.
 */
#define RWAQM_UNLOCKED_VALUE 	0x00000000L
#define	RWAQM_W_WAITING	        0x100		/* A writer is waiting	   */
#define	RWAQM_W_LOCKED	        0x0bf		/* A writer holds the lock */
#define	RWAQM_W_WMASK	        0x1bf		/* Writer mask		   */
#define	RWAQM_R_SHIFT	        9		/* Reader count shift	   */
#define RWAQM_R_BIAS	        (1U << RWAQM_R_SHIFT)

#define RWAQM_R_CNTR_CTR 0x1         /* Reader is centralized */
#define RWAQM_R_NUMA_CTR 0x2 		/* Reader is per-socket */
#define RWAQM_R_PCPU_CTR 0x4 		/* Reader is per-core */
#define RWAQM_R_WRON_CTR 0x8 		/* All readers behave as writers */

#define RWAQM_DCTR(v)        (((v) << 8) | (v))
#define RWAQM_R_CNTR_DCTR    RWAQM_DCTR(RWAQM_R_CNTR_CTR)
#define RWAQM_R_NUMA_DCTR    RWAQM_DCTR(RWAQM_R_NUMA_CTR)
#define RWAQM_R_PCPU_DCTR    RWAQM_DCTR(RWAQM_R_PCPU_CTR)
#define RWAQM_R_WRON_DCTR    RWAQM_DCTR(RWAQM_R_WRON_CTR)

struct rwaqm_node {
	struct rwaqm_node *next;

	union {
		unsigned int locked;
		struct {
			u8  lstatus;
			u8  sleader;
			u16 wcount;
		};
	};

	int nid;
        struct task_struct *task;
	struct rwaqm_node *last_visited;
	int type;
	int lock_status;
} ____cacheline_aligned;

struct rwmutex {
	struct rwaqm_node *tail;
	union {
		atomic_t val;
#ifdef __LITTLE_ENDIAN
		struct {
			u8 locked;
			u8 no_stealing;
			u8 rtype_cur;
			u8 rtype_new;
		};
		struct {
			u8 locked_no_stealing;
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
	};
};

struct rwaqm {
	union {
		atomic_long_t cnts;
		struct {
			u8 wlocked;
			u8 rcount[7];
		};
	};
	struct rwmutex wait_lock;
};

#define __RWMUTEX_INITIALIZER(lockname) 			\
	{ .val = ATOMIC_INIT(0) 				\
	, .tail = NULL }

#define DEFINE_RWMUTEX(rwmutexname) \
	struct rwmutex rwmutexname = __RWMUTEX_INITIALIZER(rwmutexname)


#ifdef USE_GLOBAL_RDTABLE
#define __INIT_TABLE(name) , .skt_readers = NULL, .cpu_readers = NULL
#else
#define __INIT_TABLE(name)
#endif

#ifdef SEPARATE_PARKING_LIST
#define __INIT_SEPARATE_PLIST(name) 				\
	, .wait_slock = __RAW_SPIN_LOCK_UNLOCKED(name.wait_slock) \
	, .next = NULL
#else
#define __INIT_SEPARATE_PLIST(name)
#endif

#define __RWAQM_INIT_COUNT(name)  				\
	.cnts = ATOMIC_LONG_INIT(RWAQM_UNLOCKED_VALUE)

#define __RWAQM_INITIALIZER(name) 				\
	{ __RWAQM_INIT_COUNT(name)  				\
	, __RWMUTEX_INITIALIZER((name).wait_lock) 		\
	  __INIT_TABLE((name)) 					\
	  __INIT_SEPARATE_PLIST((name)) }

#define DECLARE_RWAQM(name) \
        struct rwaqm name = __RWAQM_INITIALIZER(name)

static inline int rwaqm_is_locked(struct rwaqm *sem)
{
	return atomic_long_read(&sem->cnts) != 0;
}

#define init_rwaqm(sem) 					\
do { 								\
	static struct lock_class_key __key; 			\
	__init_rwaqm((sem), #sem, &__key); 			\
} while(0)

static inline int rwaqm_is_contended(struct rwaqm *sem)
{
	return sem->wait_lock.tail != NULL;
}

extern void rwaqm_down_read(struct rwaqm *sem);
/* extern void __must_check rwaqm_down_read_killable(struct rwaqm *sem); */

/*
 * trylock for reading -- returns 1 if successful, 0 if contention
 */
extern int rwaqm_down_read_trylock(struct rwaqm *sem);

/*
 * lock for writing
 */
extern void rwaqm_down_write(struct rwaqm *sem);
extern int __must_check rwaqm_down_write_killable(struct rwaqm *sem);

/*
 * trylock for writing -- returns 1 if successful, 0 if contention
 */
extern int rwaqm_down_write_trylock(struct rwaqm *sem);

/*
 * release a read lock
 */
extern void rwaqm_up_read(struct rwaqm *sem);

/*
 * release a write lock
 */
extern void rwaqm_up_write(struct rwaqm *sem);

/*
 * downgrade write lock to read lock
 */
extern void rwaqm_downgrade_write(struct rwaqm *sem);

#endif /* __LINUX_RWRWAQM_H */
