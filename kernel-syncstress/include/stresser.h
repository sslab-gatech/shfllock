#ifndef __STRESSER_H_
#define __STRESSER_H_

#define sync_log(fmt, ...)					\
	do {						        \
		printk(KERN_CRIT "sync:" fmt, ##__VA_ARGS__);	\
	} while (0)

#define sync_print(fmt, ...)					\
	do {							\
		printk(KERN_ALERT "sync:" fmt, ##__VA_ARGS__);	\
	} while (0)

#define MAX_CPU (500)
#define L1D_CACHELINE_BYTES (64)

__cacheline_aligned_in_smp struct stresser_state {
	volatile int start;
        volatile int stop;
	union {
		struct {
			volatile int ready;
		};
		char pad[L1D_CACHELINE_BYTES];
	} cpu[MAX_CPU] __attribute__((aligned(L1D_CACHELINE_BYTES)));
	volatile long n_stresser_fail;
	volatile long n_stresser_wacquired;
	volatile long n_stresser_racquired;
};

struct stresser_ops {
	void (*init)(void);
	void (*writelock)(void *arg);
	void (*write_delay)(void);
	void (*writeunlock)(void *arg);
	void (*readlock)(void *arg);
	void (*read_delay)(void);
	void (*readunlock)(void *arg);
	int (*thread_ops)(void *arg);
	void (*stat_ops)(void);
        void (*deinit)(void);
	unsigned long flags;
	const char *name;
};

struct thread_stats {
        u64 time;
        u64 jobs;
        int cid;
        void *ptr;
};

struct stresser_context {
	int threads;
        int stop;
	atomic_t n_stresser_errors;
	struct stresser_ops *ops;
	struct stresser_state state;
};

#define __synchronize_start(i, c)			        \
	do {						        \
		(c).cpu[(i)].ready = 1;			        \
		if ((i))				        \
		while (!((c).start))			        \
		        cpu_relax();				\
		else					        \
		(c).start = 1;				        \
	} while(0)

#define __synchronize_wait(i, c)			        \
	do {						        \
		while((c).cpu[(i)].nsecs == 0)		        \
                        cpu_relax();			        \
	} while(0)

#define get_cycles_diff(s, e)                                   \
        (((e).tv_sec - (s).tv_sec) * 1000000000 +               \
		                (e).tv_nsec - (s).tv_nsec       \
)

#define get_per_thread_cycles(s, e, i, c)		        \
	do {						        \
		(c).cpu[(i)].nsecs = ((e).tv_sec - (s).tv_sec)  \
                                                 * 1000000000 + \
		                (e).tv_nsec - (s).tv_nsec;      \
	} while(0)

#define DEFINE_LOCK_STR_OPS(l, type, linit, tops,               \
                            wl, wul, rl, rul)                   \
                                                                \
static __cacheline_aligned_in_smp   linit(l);                   \
                                                                \
static inline void stress_ ## l ## _wlock(void *args)           \
{                                                               \
        wl(&l);                                                 \
}                                                               \
                                                                \
static inline void stress_ ## l ## _wunlock(void *args)         \
{                                                               \
        wul(&l);                                                \
}                                                               \
                                                                \
static inline void stress_ ## l ## _rlock(void *args)           \
{                                                               \
        rl(&l);                                                 \
}                                                               \
                                                                \
static inline void stress_ ## l ## _runlock(void *args)         \
{                                                               \
        rul(&l);                                                \
}                                                               \
                                                                \
static struct stresser_ops stress_ ## l ## _ops = {             \
        .init           = NULL,                                 \
        .writelock      = stress_ ## l ## _wlock,               \
        .writeunlock    = stress_ ## l ## _wunlock,             \
        .readlock       = stress_ ## l ## _rlock,               \
        .readunlock     = stress_ ## l ## _runlock,             \
        .write_delay    = delayfn,                              \
        .read_delay     = delayfn,                              \
        .name           = #l,                                   \
        .thread_ops     = tops,                                 \
};                                                              \


#define DEFINE_LOCK_STR_W_NODE_OPS(l, type, linit, tops,        \
                            wl, wul, rl, rul, t)                \
                                                                \
static __cacheline_aligned_in_smp   linit(l);                   \
                                                                \
static inline void stress_ ## l ## _wlock(void *args)           \
{                                                               \
        struct aqm_node node;                                   \
        wl(&l, &node, t);                                       \
}                                                               \
                                                                \
static inline void stress_ ## l ## _wunlock(void *args)         \
{                                                               \
        wul(&l);                                                \
}                                                               \
                                                                \
static inline void stress_ ## l ## _rlock(void *args)           \
{                                                               \
        struct aqm_node node;                                   \
        rl(&l, &node, t);                                       \
}                                                               \
                                                                \
static inline void stress_ ## l ## _runlock(void *args)         \
{                                                               \
        rul(&l);                                                \
}                                                               \
                                                                \
static struct stresser_ops stress_ ## l ## _ops = {             \
        .init           = NULL,                                 \
        .writelock      = stress_ ## l ## _wlock,               \
        .writeunlock    = stress_ ## l ## _wunlock,             \
        .readlock       = stress_ ## l ## _rlock,               \
        .readunlock     = stress_ ## l ## _runlock,             \
        .write_delay    = delayfn,                              \
        .read_delay     = delayfn,                              \
        .name           = #l,                                   \
        .thread_ops     = tops,                                 \
};                                                              \


#endif /* __STRESSER_H_ */
