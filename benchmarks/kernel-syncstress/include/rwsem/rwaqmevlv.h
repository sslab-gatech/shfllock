#ifndef __LINUX_RWAQMEVLV_H
#define __LINUX_RWAQMEVLV_H

#include <asm/current.h>
#include <linux/spinlock_types.h>
#include <linux/atomic.h>
#include <asm/processor.h>

#define rd_id(v) ((v) * SMP_CACHE_BYTES)
#ifndef NUMA_NODES
#define NUMA_NODES 8
#endif

#ifndef NUM_CPUS
#define NUM_CPUS 80
#endif

struct socket_table {
    uint64_t lock_addr ____cacheline_aligned;
    uint64_t table[rd_id(NUMA_NODES)];
};

struct cpu_table {
    uint64_t lock_addr ____cacheline_aligned;
    uint64_t table[rd_id(NUM_CPUS)];
};

#define CTABLE_COUNT    32
#define STABLE_COUNT    128

struct readers_tables {
    struct socket_table  *stable;
    struct cpu_table     *ctable;
};

#ifdef EVOLVING_RWSEM
static struct readers_tables r = { 0 };
#endif

/*
 * Writer states & reader shift and bias.
 */
#define RWAQMEVLV_UNLOCKED_VALUE 	0x00000000L
#define	RWAQMEVLV_W_WAITING	        0x100		/* A writer is waiting	   */
#define	RWAQMEVLV_W_LOCKED	        0x0bf		/* A writer holds the lock */
#define	RWAQMEVLV_W_WMASK	        0x1bf		/* Writer mask		   */
#define	RWAQMEVLV_R_SHIFT	        9		/* Reader count shift	   */
#define RWAQMEVLV_R_BIAS	        (1U << RWAQMEVLV_R_SHIFT)

#define RWAQMEVLV_R_CNTR_CTR 0x1         /* Reader is centralized */
#define RWAQMEVLV_R_NUMA_CTR 0x2 		/* Reader is per-socket */
#define RWAQMEVLV_R_PCPU_CTR 0x4 		/* Reader is per-core */
#define RWAQMEVLV_R_WRON_CTR 0x8 		/* All readers behave as writers */

#define RWAQMEVLV_DCTR(v)        (((v) << 8) | (v))
#define RWAQMEVLV_R_CNTR_DCTR    RWAQMEVLV_DCTR(RWAQMEVLV_R_CNTR_CTR)
#define RWAQMEVLV_R_NUMA_DCTR    RWAQMEVLV_DCTR(RWAQMEVLV_R_NUMA_CTR)
#define RWAQMEVLV_R_PCPU_DCTR    RWAQMEVLV_DCTR(RWAQMEVLV_R_PCPU_CTR)
#define RWAQMEVLV_R_WRON_DCTR    RWAQMEVLV_DCTR(RWAQMEVLV_R_WRON_CTR)

#if 0
#define RWAQMEVLV_ACTIVE_MASK 	0xffffffffL
#define RWAQMEVLV_ACTIVE_BIAS 	0x00000001L
#define RWAQMEVLV_WAITING_BIAS 	(-RWAQMEVLV_ACTIVE_MASK-1)
#define RWAQMEVLV_ACTIVE_READ_BIAS 	RWAQMEVLV_ACTIVE_BIAS
#define RWAQMEVLV_ACTIVE_WRITE_BIAS (RWAQMEVLV_WAITING_BIAS + RWAQMEVLV_ACTIVE_BIAS)
#endif

enum rwaqmevlv_waiter_type {
	RWAQMEVLV_NONE,
	RWAQMEVLV_WRITER,
	RWAQMEVLV_READER
};

struct rwaqmevlv_node {
	struct rwaqmevlv_node *next;

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
	struct rwaqmevlv_node *last_visited;
	int type;
	int lock_status;
} ____cacheline_aligned;

struct rwmutexevlv {
	struct rwaqmevlv_node *tail;
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

struct rwaqmevlv {
	union {
		atomic_long_t cnts;
		struct {
			u8 wlocked;
			u8 rcount[7];
		};
	};
#ifdef USE_GLOBAL_RDTABLE
	struct rwmutexevlv wait_lock;
#else
	struct rwmutexevlv wait_lock ____cacheline_aligned;

	uint64_t *skt_readers ____cacheline_aligned;
	uint64_t *cpu_readers ____cacheline_aligned;
#endif
#ifdef SEPARATE_PARKING_LIST
	raw_spinlock_t wait_slock;
	struct rwaqmevlv_node *next;
#endif
};

#define __RWMUTEX_INITIALIZER(lockname) 			\
	{ .val = ATOMIC_INIT(0) 				\
	, .tail = NULL }

#define DEFINE_RWMUTEXEVLV(rwmutexevlvname) \
	struct rwmutexevlv rwmutexevlvname = __RWMUTEX_INITIALIZER(rwmutexevlvname)


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

#define __RWAQMEVLV_INIT_COUNT(name)  				\
	.cnts = ATOMIC_LONG_INIT(RWAQMEVLV_UNLOCKED_VALUE)

#define __RWAQMEVLV_INITIALIZER(name) 				\
	{ __RWAQMEVLV_INIT_COUNT(name)  				\
	, __RWMUTEX_INITIALIZER((name).wait_lock) 		\
	  __INIT_TABLE((name)) 					\
	  __INIT_SEPARATE_PLIST((name)) }

#define DECLARE_RWAQMEVLV(name) \
        struct rwaqmevlv name = __RWAQMEVLV_INITIALIZER(name)

static inline int rwaqmevlv_is_locked(struct rwaqmevlv *sem)
{
	return atomic_long_read(&sem->cnts) != 0;
}

#define init_rwaqmevlv(sem) 					\
do { 								\
	static struct lock_class_key __key; 			\
	__init_rwaqmevlv((sem), #sem, &__key); 			\
} while(0)

static inline int rwaqmevlv_is_contended(struct rwaqmevlv *sem)
{
	return sem->wait_lock.tail != NULL;
}

extern void rwaqmevlv_down_read(struct rwaqmevlv *sem);
extern void __must_check rwaqmevlv_down_read_killable(struct rwaqmevlv *sem);

/*
 * trylock for reading -- returns 1 if successful, 0 if contention
 */
extern int rwaqmevlv_down_read_trylock(struct rwaqmevlv *sem);

/*
 * lock for writing
 */
extern void rwaqmevlv_down_write(struct rwaqmevlv *sem);
extern int __must_check rwaqmevlv_down_write_killable(struct rwaqmevlv *sem);

/*
 * trylock for writing -- returns 1 if successful, 0 if contention
 */
extern int rwaqmevlv_down_write_trylock(struct rwaqmevlv *sem);

/*
 * release a read lock
 */
extern void rwaqmevlv_up_read(struct rwaqmevlv *sem);

/*
 * release a write lock
 */
extern void rwaqmevlv_up_write(struct rwaqmevlv *sem);

/*
 * downgrade write lock to read lock
 */
extern void rwaqmevlv_downgrade_write(struct rwaqmevlv *sem);

#endif /* __LINUX_RWAQMEVLV_H */
