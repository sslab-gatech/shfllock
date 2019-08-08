#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/topology.h>
#include <linux/compiler.h>
#include <linux/random.h>
#include <linux/moduleparam.h>
#include <linux/kthread.h>
#include <linux/init.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/smp.h>
#include <linux/cpu.h>
#include <linux/percpu.h>
#include <linux/atomic.h>
#include <asm/byteorder.h>
#include <linux/ktime.h>
#include <linux/vmalloc.h>
#include <linux/sort.h>

#include <asm/uaccess.h>

#include <linux/spinlock.h>
#include <linux/rwsem.h>
#include <linux/mutex.h>
#include <linux/rwlock.h>
#include <linux/percpu-rwsem.h>

#include "cpuseq.h"
#include "stresser.h"
#include "spinlock/qspinlock.h"
#include "spinlock/aqs.h"
#include "spinlock/cmcsmcs.h"
#include "spinlock/ctasmcs.h"
#include "spinlock/cna.h"
#include "rwlock/rwaqs_ntrl.h"
#include "rwlock/rwaqs_rp.h"
#include "rwlock/cmcsmcsrw.h"
#include "mutex/aqm.h"
#include "rwsem/rwaqm.h"

DEFINE_PER_CPU(u64, offpath);
DEFINE_PER_CPU(u64, offpath2);
DEFINE_PER_CPU(u64, critpath);
DEFINE_PER_CPU(u64, fpath);
DEFINE_PER_CPU(u64, spath);


static int dirty_clines = 1;
static int read_clines = 1;
static int threads = -1; /* Number of mixed reader/writer threads; defaults to online CPUs */
static int delay = 0;

static char *stress_type = "qspinlock";
module_param(stress_type, charp, S_IRUGO | S_IWUSR);
module_param(delay, int, S_IRUGO | S_IWUSR);
module_param(dirty_clines, int, S_IRUGO | S_IWUSR);
module_param(read_clines, int, S_IRUGO | S_IWUSR);
module_param(threads, int, 0444);
MODULE_PARM_DESC(threads, "Number of mixed reader/writer threads");

static struct stresser_context ctx = {0, 0,
        ATOMIC_INIT(0), NULL};

static struct task_struct **tasks;
struct thread_stats *thread_stats;
static char *dirty_data;

static inline void _dirty_lines(int clines, bool dirty)
{
	int i;
	int total_bytes = 2 * clines * SMP_CACHE_BYTES;
	for (i = 0; i < total_bytes; i += (2 * SMP_CACHE_BYTES)) {
                int d = READ_ONCE(dirty_data[i]);
		if (dirty)
			WRITE_ONCE(dirty_data[i], d + i % 10);
		else
			(void)READ_ONCE(dirty_data[i]);
	}
	return;
}

static int dirty_cc_ops(void *arg)
{
        struct thread_stats *tstat = arg;
	struct timespec64 start_t, end_t;

        __synchronize_start(tstat->cid, ctx.state);
	ktime_get_raw_ts64(&start_t);
	do {
		ctx.ops->writelock(NULL);
		_dirty_lines(dirty_clines, true);
		ctx.ops->writeunlock(NULL);

		ctx.ops->write_delay();

                tstat->jobs++;

        } while (!kthread_should_stop() && !ctx.stop);
	ktime_get_raw_ts64(&end_t);

        end_t = timespec64_sub(end_t, start_t);
        tstat->time = timespec64_to_ns(&end_t);

        while (!kthread_should_stop())
                schedule_timeout_interruptible(1);

	return 0;
}

static void mmap_ops(void *arg)
{
        ctx.ops->writelock(NULL);
        _dirty_lines(dirty_clines, true);
        ctx.ops->writeunlock(NULL);

        ctx.ops->write_delay();
}
#define munmap_ops mmap_ops

static void pagefault_ops(void *arg)
{
        ctx.ops->readlock(NULL);
        _dirty_lines(read_clines, false);
        ctx.ops->readunlock(NULL);

        ctx.ops->read_delay();
}

static int mmap_read_munmap_ops(void *arg)
{
        struct thread_stats *tstat = arg;
        struct timespec64 start_t, end_t;

        __synchronize_start(tstat->cid, ctx.state);
        ktime_get_raw_ts64(&start_t);
        do {
                mmap_ops(tstat->ptr);

                pagefault_ops(tstat->ptr);

                munmap_ops(tstat->ptr);

                tstat->jobs++;

        } while (!kthread_should_stop() && !ctx.stop);
        ktime_get_raw_ts64(&end_t);

        tstat->time = get_cycles_diff(start_t, end_t);

        while (!kthread_should_stop())
                schedule_timeout_interruptible(1);

        return 0;
}

static void delayfn(void)
{
        /* int i; */
        /* for (i = 0; i < delay; ++i) */
        /*         cpu_relax(); */
        u64 k = ktime_to_ns(ktime_get_raw());
        while (ktime_to_ns(ktime_get_raw()) - k <= delay * 1000) {
                cpu_relax();
                if (need_resched())
                        cond_resched();
        }
}

/* This one is for the queued spinlock */
DEFINE_LOCK_STR_OPS(orig_qspinlock, struct orig_qspinlock, DEFINE_QSPINLOCK, dirty_cc_ops,
                    orig_queued_spin_lock, orig_queued_spin_unlock,
                    orig_queued_spin_lock, orig_queued_spin_unlock);


DEFINE_LOCK_STR_OPS(cna, struct orig_qspinlock, DEFINE_QSPINLOCK, dirty_cc_ops,
                    cna_spin_lock, cna_spin_unlock,
                    cna_spin_lock, cna_spin_unlock);

/* Cohort MCS-MCS lock */
DEFINE_LOCK_STR_OPS(cmcsmcs, struct cstmcs_lock,
                    DEFINE_CMCSMCS, dirty_cc_ops,
                    cstmcs_spin_lock, cstmcs_spin_unlock,
                    cstmcs_spin_lock, cstmcs_spin_unlock);

/* Cohort TAS-MCS lock (TAS is the outer one) */
DEFINE_LOCK_STR_OPS(ctasmcs, struct ctasmcs_lock, DEFINE_CTASMCS, dirty_cc_ops,
                    ctasmcsspin_lock, ctasmcsspin_unlock,
                    ctasmcsspin_lock, ctasmcsspin_unlock);

/* Adaptive queued spinlock design */
DEFINE_LOCK_STR_OPS(aqs, struct aqs_lock, DEFINE_AQSLOCK, dirty_cc_ops,
                    __aqs_acquire, __aqs_release,
                    __aqs_acquire, __aqs_release);

/* Kernel rwlock */
DEFINE_LOCK_STR_OPS(qrwlock, rwlock_t, DEFINE_RWLOCK, mmap_read_munmap_ops,
                    write_lock, write_unlock,
                    read_lock, read_unlock);

/* Our neutral rwlock algorithm based on aqs */
DEFINE_LOCK_STR_OPS(aqs_rwlock_ntrl, struct aqs_rwlock_ntrl,
                    DEFINE_AQS_RWLOCK_NTRL, mmap_read_munmap_ops,
                    aqs_write_lock_ntrl, aqs_write_unlock_ntrl,
                    aqs_read_lock_ntrl, aqs_read_unlock_ntrl);

/* Our neutral rwlock algorithm based on aqs */
DEFINE_LOCK_STR_OPS(aqs_rwlock_rp, struct aqs_rwlock_rp,
                    DEFINE_AQS_RWLOCK_RP, mmap_read_munmap_ops,
                    aqs_write_lock_rp, aqs_write_unlock_rp,
                    aqs_read_lock_rp, aqs_read_unlock_rp);

/* Our neutral rwlock algorithm based on aqs */
DEFINE_LOCK_STR_OPS(aqs_rwlock_rp_x, struct aqs_rwlock_rp,
                    DEFINE_AQS_RWLOCK_RP, mmap_read_munmap_ops,
                    aqs_write_lock_rp, aqs_write_unlock_rp,
                    aqs_read_lock_rp, aqs_read_unlock_rp);


DEFINE_LOCK_STR_OPS(cmcsmcsrw, struct cstmcsrw_lock,
                    DEFINE_CMCSMCSRW, mmap_read_munmap_ops,
                    cstmcsrw_write_lock, cstmcsrw_write_unlock,
                    cstmcsrw_read_lock, cstmcsrw_read_unlock);

DEFINE_LOCK_STR_W_NODE_OPS(aqm_fp, struct aqm_mutex, DEFINE_AQM, dirty_cc_ops,
                           aqm_lock_w_node, aqm_unlock,
                           aqm_lock_w_node, aqm_unlock, 0);

DEFINE_LOCK_STR_W_NODE_OPS(aqm_lnuma, struct aqm_mutex, DEFINE_AQM, dirty_cc_ops,
                           aqm_lock_w_node, aqm_unlock,
                           aqm_lock_w_node, aqm_unlock, 1);

DEFINE_LOCK_STR_W_NODE_OPS(aqm_rnuma, struct aqm_mutex, DEFINE_AQM, dirty_cc_ops,
                           aqm_lock_w_node, aqm_unlock,
                           aqm_lock_w_node, aqm_unlock, 2);

DEFINE_LOCK_STR_W_NODE_OPS(aqm_nfp, struct aqm_mutex, DEFINE_AQM, dirty_cc_ops,
                           aqm_lock_w_node, aqm_unlock,
                           aqm_lock_w_node, aqm_unlock, 3);

DEFINE_LOCK_STR_OPS(mutex, struct mutex, DEFINE_MUTEX, dirty_cc_ops,
                    mutex_lock, mutex_unlock,
                    mutex_lock, mutex_unlock);

DEFINE_LOCK_STR_OPS(rwsem, struct rw_semaphore, DECLARE_RWSEM, mmap_read_munmap_ops,
		    down_write, up_write, down_read, up_read);

DEFINE_LOCK_STR_OPS(rwaqm, struct rwaqm, DECLARE_RWAQM, mmap_read_munmap_ops,
		    rwaqm_down_write, rwaqm_up_write,
		    rwaqm_down_read, rwaqm_up_read);

static int cmp_tput(const void *ja, const void *jb)
{
        const struct thread_stats *a, *b;
        a = ja;
        b = jb;
        return (a->jobs == b->jobs)? 0 : (a->jobs > b->jobs)? 1 : -1;
}

/* #define PRINT_THREAD_STATS */
static void stress_print_stats(void)
{
	int i;
	struct thread_stats s = {};
        u64 min_job = thread_stats[0].jobs, max_job = thread_stats[0].jobs;
        u64 up = thread_stats[0].jobs, down = thread_stats[0].jobs;

	if (!thread_stats) {
		printk(KERN_ALERT "stress stats unavailable\n");
		return;
	}

        ctx.stop = 1;
	for (i = 0; i < threads; i++) {
#ifdef PRINT_THREAD_STATS
                printk(KERN_ALERT "[%d]: jobs: %llu\n",
                       i, thread_stats[i].jobs);
#endif
                s.jobs += thread_stats[i].jobs;
                if (s.time < thread_stats[i].time)
                        s.time = thread_stats[i].time;
	}

        if (threads > 1) {
                u64 up_sum = 0, down_sum = 0;
                sort(thread_stats, threads, sizeof(thread_stats[0]), cmp_tput, NULL);

                min_job = thread_stats[0].jobs;
                max_job = thread_stats[threads - 1].jobs;

                for (i = 0; i < threads; ++i) {
                        if (i < threads / 2)
                                up_sum += thread_stats[i].jobs;
                        else
                                down_sum += thread_stats[i].jobs;
                }
                up = up_sum;
                down = down_sum;
        }

	printk(KERN_ALERT "summary: threads=%d type=%s\n"
	       KERN_ALERT "summary: jobs: %llu (%llu) time: %llu min: %llu "
               "max: %llu ( %llu / %llu )\n",
	       threads, stress_type, s.jobs, s.jobs / 1000000, s.time,
               min_job, max_job, up, down);
}

static void stress_exit(void)
{
	unsigned long i;
	int ret;

	if (tasks) {
		for (i = 0; i < threads; i++)
			if (tasks[i]) {
				ret = kthread_stop(tasks[i]);
				if(ret)
					printk(KERN_ALERT "stress task returned error %d\n", ret);
			}
		kfree(tasks);
	}

        stress_print_stats();
	kfree(thread_stats);
        vfree(dirty_data);
	printk(KERN_ALERT "stress done\n");
}

static __init int stress_init(void)
{
	int ret;
	u32 i;

        static struct stresser_ops *stress_ops[] = {
		&stress_orig_qspinlock_ops,
                &stress_cmcsmcs_ops,
                &stress_ctasmcs_ops,
                &stress_aqs_ops,
                &stress_qrwlock_ops,
                &stress_cmcsmcsrw_ops,
                &stress_aqs_rwlock_ntrl_ops,
		&stress_aqs_rwlock_rp_ops,
                &stress_aqs_rwlock_rp_x_ops,
                &stress_cna_ops,
                &stress_mutex_ops,
                &stress_aqm_fp_ops,
                &stress_aqm_nfp_ops,
                &stress_aqm_lnuma_ops,
                &stress_aqm_rnuma_ops,
		&stress_rwsem_ops,
		&stress_rwaqm_ops,
	};

        aqs_set_distributed_rp(&aqs_rwlock_rp_x);
	for(i = 0; i < ARRAY_SIZE(stress_ops); i++) {
		ctx.ops = stress_ops[i];
		if (strcmp(stress_type, ctx.ops->name) == 0)
			break;
	}

	if (i == ARRAY_SIZE(stress_ops)) {
		sync_log("invalid sync primitives specified");
		return -EINVAL;
	}

	if (ctx.ops->thread_ops == NULL)
		return -EINVAL;


        dirty_data = vzalloc(SMP_CACHE_BYTES * 2 * (dirty_clines + 1));
        if (!dirty_data) {
                return -ENOMEM;
        }

	if (ctx.ops->init)
		ctx.ops->init();

	if (threads < 0)
		threads = num_online_cpus();

        if (dirty_clines > 16)
                dirty_clines = 16;

        if (read_clines > 16)
                read_clines = 16;

	thread_stats = kcalloc(threads, sizeof(thread_stats[0]), GFP_KERNEL);
	if (!thread_stats)
		goto enomem;

	tasks = kcalloc(threads, sizeof(tasks[0]), GFP_KERNEL);
	if (!tasks)
		goto enomem;

	printk(KERN_ALERT "stress starting threads\n");

        ctx.stop = 0;
	for (i = 0; i < threads; i++) {
		struct task_struct *task;
                thread_stats[i].cid = cpuseq[i] % online_cpus;
                task = kthread_create(ctx.ops->thread_ops,
                                      &thread_stats[i],
                                      "stress");
		if (IS_ERR(task)) {
			ret = PTR_ERR(task);
			goto error;
		}
		tasks[i] = task;
                /* if (threads <= online_cpus) */
                /*         kthread_bind(tasks[i], cpuseq[i] % online_cpus); */
		wake_up_process(tasks[i]);
	}
	return 0;

     enomem:
	ret = -ENOMEM;
     error:
	stress_exit();
	return ret;
}

module_init(stress_init);
module_exit(stress_exit);

MODULE_AUTHOR("Sanidhya Kashyap <sanidhya@gatech.edu>");
MODULE_DESCRIPTION("Simple stress testing for spinlocks.");
MODULE_LICENSE("GPL");
