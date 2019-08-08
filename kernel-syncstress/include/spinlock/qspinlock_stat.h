/*
 * When queued spinlock statistical counters are enabled, the following
 * debugfs files will be created for reporting the counter values:
 *
 * <debugfs>/qlockstat/
 *   pv_hash_hops	- average # of hops per hashing operation
 *   pv_kick_unlock	- # of vCPU kicks issued at unlock time
 *   pv_kick_wake	- # of vCPU kicks used for computing pv_latency_wake
 *   pv_latency_kick	- average latency (ns) of vCPU kick operation
 *   pv_latency_wake	- average latency (ns) from vCPU kick to wakeup
 *   pv_lock_stealing	- # of lock stealing operations
 *   pv_spurious_wakeup	- # of spurious wakeups in non-head vCPUs
 *   pv_wait_again	- # of wait's after a queue head vCPU kick
 *   pv_wait_early	- # of early vCPU wait's
 *   pv_wait_head	- # of vCPU wait's at the queue head
 *   pv_wait_node	- # of vCPU wait's at a non-head queue node
 *   lock_pending	- # of locking operations via pending code
 *   lock_slowpath	- # of locking operations via MCS lock queue
 *
 * Writing to the "reset_counters" file will reset all the above counter
 * values.
 *
 * These statistical counters are implemented as per-cpu variables which are
 * summed and computed whenever the corresponding debugfs files are read. This
 * minimizes added overhead making the counters usable even in a production
 * environment.
 *
 * There may be slight difference between pv_kick_wake and pv_kick_unlock.
 */
enum qlock_stats {
	qstat_pv_hash_hops,
	qstat_pv_kick_unlock,
	qstat_pv_kick_wake,
	qstat_pv_latency_kick,
	qstat_pv_latency_wake,
	qstat_pv_lock_stealing,
	qstat_pv_spurious_wakeup,
	qstat_pv_wait_again,
	qstat_pv_wait_early,
	qstat_pv_wait_head,
	qstat_pv_wait_node,
	qstat_lock_pending,
	qstat_lock_slowpath,
	qstat_num,	/* Total number of statistical counters */
	qstat_reset_cnts = qstat_num,
};


static inline void qstat_inc(enum qlock_stats stat, bool cond)	{ }
static inline void qstat_hop(int hopcnt)			{ }
