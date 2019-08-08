#include <linux/module.h>
#include <linux/smp.h>
#include <linux/bug.h>
#include <linux/percpu.h>
#include <linux/hardirq.h>
#include <linux/prefetch.h>
#include <linux/atomic.h>
#include <linux/sched/wake_q.h>
#include <linux/sched/task.h>
#include <linux/sched/stat.h>
#include <asm/byteorder.h>
#include <linux/random.h>

#ifdef KERNEL_SYNCSTRESS
#include "mutex/aqm.h"
static DEFINE_PER_CPU_ALIGNED(struct aqm_node, aqm_node);

static DEFINE_PER_CPU(u32, seed);

/*
 * Controls the probability for intra-socket lock hand-off. It can be
 * tuned and depend, e.g., on the number of CPUs per socket. For now,
 * choose a value that provides reasonable long-term fairness without
 * sacrificing performance compared to a version that does not have any
 * fairness guarantees.
 */
#define INTRA_SOCKET_HANDOFF_PROB_ARG  0x10000

#define get_curr_node_ptr() this_cpu_ptr(&aqm_node)
#else
#include <linux/aqm.h>
#define get_curr_node_ptr() (&((current)->aqm_node))
#endif

#define THRESHOLD (0xffff)
#ifndef UNLOCK_COUNT_THRESHOLD
#define UNLOCK_COUNT_THRESHOLD 1024
#endif

static inline uint32_t xor_random(void)
{
        u32 v = this_cpu_read(seed);

        if (v == 0)
		get_random_bytes(&v, sizeof(u32));

        v ^= v << 6;
        v ^= (u32)(v) >> 21;
        v ^= v << 7;
        this_cpu_write(seed, v);

        return v & (UNLOCK_COUNT_THRESHOLD - 1);
	/* return v; */
}

static inline bool probably(unsigned int range)
{
        return xor_random() & (range - 1);
}

static inline void enable_stealing(struct aqm_mutex *lock)
{
        atomic_andnot(_AQ_MCS_NOSTEAL_VAL, &lock->val);
}

static inline void unlock_and_enable_stealing(struct aqm_mutex *lock)
{
        WRITE_ONCE(lock->locked_no_stealing, 0);
}

static inline void disable_stealing(struct aqm_mutex *lock)
{
        atomic_fetch_or_acquire(_AQ_MCS_NOSTEAL_VAL, &lock->val);
}

static inline u8 is_stealing_disabled(struct aqm_mutex *lock)
{
        return smp_load_acquire(&lock->no_stealing);
}

static inline void set_sleader(struct aqm_node *node, struct aqm_node *qend)
{
        smp_store_release(&node->sleader, 1);
        if (qend != node)
                smp_store_release(&node->last_visited, qend);
}

static inline void clear_sleader(struct aqm_node *node)
{
        node->sleader = 0;
}

static inline void set_waitcount(struct aqm_node *node, int count)
{
        smp_store_release(&node->wcount, count);
}

static void wake_up_waiter(struct aqm_node *node)
{
        struct task_struct *task = node->task;
#ifdef KERNEL_SYNCSTRESS
        get_task_struct(task);
        wake_up_process(task);
        put_task_struct(task);
#else

        DEFINE_WAKE_Q(wake_q);

        wake_q_add(&wake_q, task);
        wake_up_q(&wake_q);
#endif
}

static void schedule_out_curr_task(void)
{
#ifdef KERNEL_SYNCSTRESS
        preempt_enable();
        schedule();
        preempt_disable();
#else
        schedule_preempt_disabled();
#endif
}

#if 0
static inline int force_update_node(struct aqm_node *node, u8 state)
{
        u8 lstatus = cmpxchg(&node->lstatus, _AQ_MCS_STATUS_PWAIT,
                    state);

        if (lstatus)
                return lstatus;

        /**
         * OUCH: That is going to hurt as I am doing two
         * atomic instructions just for the sake of avoiding
         * the wakeup in the worst case scenario i.e., waking
         * up the VERY NEXT WAITER.
         **/
        if (cmpxchg(&node->lstatus, _AQ_MCS_STATUS_PARKED,
                    state) == _AQ_MCS_STATUS_PARKED) {
                wake_up_waiter(node);
                return false;
        }
        return true;
}
#endif
static inline int force_update_node(struct aqm_node *node, u8 state)
{
        if (cmpxchg(&node->lstatus, _AQ_MCS_STATUS_PWAIT,
                    state) == _AQ_MCS_STATUS_PWAIT)
		goto out;
        /**
         * OUCH: That is going to hurt as I am doing two
         * atomic instructions just for the sake of avoiding
         * the wakeup in the worst case scenario i.e., waking
         * up the VERY NEXT WAITER.
         **/
        if (cmpxchg(&node->lstatus, _AQ_MCS_STATUS_PARKED,
                    state) == _AQ_MCS_STATUS_PARKED) {
		this_cpu_inc(offpath);
                wake_up_waiter(node);
                return true;
        }

     out:
	this_cpu_inc(offpath2);
        return false;
}

static void park_waiter(struct aqm_node *node, long state)
{

        if (cmpxchg(&node->lstatus, _AQ_MCS_STATUS_PWAIT,
                    _AQ_MCS_STATUS_PARKED) != _AQ_MCS_STATUS_PWAIT)
                goto out_acquired;

        set_current_state(state);
        schedule_out_curr_task();

     out_acquired:
        set_current_state(TASK_RUNNING);
}

void __aqm_init(struct aqm_mutex *lock, const char *name,
		struct lock_class_key *key)
{
        atomic_set(&lock->val, 0);
        lock->tail = NULL;

        smp_mb();
}

static void shuffle_waiters(struct aqm_mutex *lock, struct aqm_node *node,
                            int is_next_waiter)
{
        struct aqm_node *curr, *prev, *next, *last, *sleader, *qend;
        int nid;
        int curr_locked_count;
        int one_shuffle = false;
        int shuffled_count = 0;
        int woke_up_one = false;

        prev = smp_load_acquire(&node->last_visited);
        if (!prev)
                prev = node;
        last = node;
        curr = NULL;
        next = NULL;
        sleader = NULL;
        qend = NULL;

        nid = node->nid;
        curr_locked_count = node->wcount;

        barrier();

        cmpxchg(&node->lstatus, _AQ_MCS_STATUS_PWAIT, _AQ_MCS_STATUS_UNPWAIT);

        /*
         * If the wait count is 0, then increase node->wcount
         * to 1 to avoid coming it again.
         */
        if (curr_locked_count == 0) {
                set_waitcount(node, ++curr_locked_count);
        }

        /*
         * Our constraint is that we will reset every shuffle
         * leader and the new one will be selected at the end,
         * if any.
         *
         * This one here is to avoid the confusion of having
         * multiple shuffling leaders.
         */
        clear_sleader(node);

        /*
         * In case the curr_locked_count has crossed a
         * threshold, which is certainly impossible in this
         * design, then load the very next of the node and pass
         * the shuffling responsibility to that @next.
         */
        /* if (curr_locked_count >= _AQ_MAX_LOCK_COUNT) { */
	if (!probably(INTRA_SOCKET_HANDOFF_PROB_ARG)) {
                sleader = READ_ONCE(node->next);
                /* sleader = node->next; */
                goto out;
        }

        /*
         * In this loop, we try to shuffle the wait queue at
         * least once to allow waiters from the same socket to
         * have no cache-line bouncing. This shuffling is
         * associated in two aspects:
         * 1) when both adjacent nodes belong to the same socket
         * 2) when there is an actual shuffling that happens.
         *
         * Currently, the approach is very conservative. If we
         * miss any of the elements while traversing, we return
         * back.
         *
         * TODO: We can come up with some aggressive strategy to
         * form long chains, which we are yet to explore
         *
         * The way the algorithm works is that it tries to have
         * at least two pointers: pred and curr, in which
         * curr = pred->next. If curr and pred are in the same
         * socket, then no need to shuffle, just update pred to
         * point to curr.
         * If that is not the case, then try to find the curr
         * whose node id is same as the @node's node id. On
         * finding that, we also try to get the @next, which is
         * next = curr->next; which we use all of them to
         * shuffle them wrt @last.
         * @last holds the latest shuffled element in the wait
         * queue, which is updated on each shuffle and is most
         * likely going to be next shuffle leader.
         */
        for (;;) {
                /*
                 * Get the curr first
                 */
                curr = READ_ONCE(prev->next);

                /*
                 * Now, right away we can quit the loop if curr
                 * is NULL or is at the end of the wait queue
                 * and choose @last as the sleader.
                 */
                if (!curr) {
                        sleader = last;
                        qend = prev; /* Until prev, I am updated */
                        break;
                }

                /*
                 * If we are the last one in the tail, then
                 * we cannot do anything, we should return back
                 * while selecting the next sleader as the last one
                 */
                if (curr == READ_ONCE(lock->tail)) {
                        sleader = last;
                        qend = prev;
                        break;
                }

                /* got the current for sure */

                /* Check if curr->nid is same as nid */
                if (curr->nid == nid) {

                        /*
                         * if prev->nid == curr->nid, then
                         * just update the last and prev
                         * and proceed forward
                         */
                        if (prev->nid == nid) {
                                set_waitcount(curr, curr_locked_count);

                                last = curr;
                                prev = curr;
                                one_shuffle = true;
                                shuffled_count++;

                                woke_up_one = !force_update_node(curr,
                                                        _AQ_MCS_STATUS_UNPWAIT);

                        } else {
                                /* prev->nid is not same, then we need
                                 * to find next and move @curr to
                                 * last->next, while linking @prev->next
                                 * to next.
                                 *
                                 * NOTE: We do not update @prev here
                                 * because @curr has been already moved
                                 * out.
                                 */

                                next = READ_ONCE(curr->next);
                                if (!next) {
                                        sleader = last;
                                        break;
                                }

                                /*
                                 * Since, we have curr and next,
                                 * we mark the curr that it has been
                                 * shuffled and shuffle the queue
                                 */
                                set_waitcount(curr, curr_locked_count);

/*
 *                                                 (1)
 *                                    (3)       ----------
 *                          -------------------|--\      |
 *                        /                    |   v     v
 *   ----          ----   |  ----        ----/   ----   ----
 *  | SL | -> ... |Last| -> | X  |....->|Prev|->|Curr|->|Next|->....
 *   ----          ----  ->  ----        ----    ----  | ----
 *                      /          (2)                /
 *                      -----------------------------
 *                              |
 *                              V
 *   ----          ----      ----      ----        ----    ----
 *  | SL | -> ... |Last| -> |Curr| -> | X  |....->|Prev|->|Next|->....
 *   ----          ----      ----      ----        ----    ----
 *
 */
                                prev->next = next;
                                curr->next = last->next;
                                last->next = curr;

                                woke_up_one = !force_update_node(curr,
                                                        _AQ_MCS_STATUS_UNPWAIT);
                                last = curr;
                                curr = next;
                                one_shuffle = true;
                                shuffled_count++;

                        }
                } else
                        prev = curr;

                if (one_shuffle && ((is_next_waiter && !READ_ONCE(lock->locked)) ||
                                    smp_load_acquire(&node->lstatus))) {
                        sleader = last;
                        qend = prev;
                        break;
                }
        }

     out:
        if (sleader) {
                set_sleader(sleader, qend);
        }
}

static void __aqm_lock_slowpath(struct aqm_mutex *lock, struct aqm_node *node)
{
        struct aqm_node *prev, *next;
        u8 plstatus = 8;
        long state = TASK_INTERRUPTIBLE;
	/* int disable_steal = false; */

        preempt_disable();
        node->next = NULL;
        node->last_visited = NULL;
        node->locked = _AQ_MCS_STATUS_PWAIT;
        node->nid = numa_node_id();
        node->task = current;

        /*
         * Ensure that the initialisation of @node is complete before we
         * publish the updated tail via xchg_tail() and potentially link
         * @node into the waitqueue via WRITE_ONCE(prev->next, node) below.
         */
        smp_wmb();

        prev = xchg(&lock->tail, node);
        next = NULL;

        if (prev) {

                WRITE_ONCE(prev->next, node);

                for (;;) {
                        unsigned int val = READ_ONCE(node->locked);

                        if (_AQ_MCS_LOCKED_VAL(val) == _AQ_MCS_STATUS_LOCKED) {
                                break;
                        }

                        if (_AQ_MCS_SLEADER_VAL(val) == 1) {
                                shuffle_waiters(lock, node, false);
                        }
                        if (need_resched()) {
                                if (single_task_running()) {
                                        __set_current_state(TASK_RUNNING);
                                        schedule_out_curr_task();
                                } else {
                                        park_waiter(node, state);
                                }
                        }
                        cpu_relax();
                }
		next = READ_ONCE(node->next);
		prefetch(next);
        }

        /*
         * we are now the very next waiters, all we have to do is
         * to wait for the @lock->locked to become 0, i.e. unlocked.
         * In the meantime, we will try to be shuffle leader if possible
         * and at least find someone in my socket.
         */
        for(;;) {
                unsigned int val = READ_ONCE(node->locked);

                if (!READ_ONCE(lock->locked))
                        break;

                if (!_AQ_MCS_WCOUNT_VAL(val) ||
                    (_AQ_MCS_WCOUNT_VAL(val) && _AQ_MCS_SLEADER_VAL(val))) {
                        shuffle_waiters(lock, node, true);
                        cpu_relax();
                }
        }

	if (next) {
		plstatus = xchg(&next->lstatus, _AQ_MCS_STATUS_UNPWAIT);
		if (plstatus == _AQ_MCS_STATUS_PARKED) {
			wake_up_waiter(next);
		}
	}

        for (;;) {
                /* disable_stealing(lock); */

                if (cmpxchg(&lock->locked, 0, 1) == 0) {
                        lock->nid = numa_node_id();
                        break;
                }

                while (smp_load_acquire(&lock->locked))
                        cpu_relax();

		/* disable_stealing(lock); */
		/* disable_steal = true; */

                cpu_relax();
        }


        next = READ_ONCE(node->next);
        if (!next) {
                if (cmpxchg(&lock->tail, node, NULL) == node) {
			/* if (disable_steal) */
			/* 	enable_stealing(lock); */
                        /* enable_stealing(lock); */
                        goto out;
                }

                for (;;) {
                        next = READ_ONCE(node->next);
                        if (next)
                                break;
                        cpu_relax();
                }
        }

        /*
         * Notify the very next waiter
         */
        plstatus = xchg(&next->lstatus, _AQ_MCS_STATUS_LOCKED);
        if (unlikely(plstatus == _AQ_MCS_STATUS_PARKED)) {
                /* enable_stealing(lock); */
		this_cpu_inc(critpath);
                wake_up_waiter(next);
                /* goto out_ret; */
		goto out;
        }

     out:
	/* if (disable_steal) */
	/* 	enable_stealing(lock); */
     /* out_ret: */
        preempt_enable();
}

void aqm_lock(struct aqm_mutex *lock)
{
        struct aqm_node *node;

        if (likely(cmpxchg(&lock->locked_no_stealing, 0, 1) == 0))
                return;

        node = get_curr_node_ptr();

        __aqm_lock_slowpath(lock, node);
}

void aqm_lock_w_node(struct aqm_mutex *lock, struct aqm_node *node, int type)
{
        if (type == 0) {
                if (likely((!lock->locked_no_stealing) &&
                           (cmpxchg(&lock->locked_no_stealing, 0, 1) == 0)))
			goto out;
        } else if (type == 1) {
                if (likely(lock->nid == numa_node_id() &&
                           cmpxchg(&lock->locked_no_stealing, 0, 1) == 0))
			goto out;
        } else if (type == 2) {
                if (likely(lock->nid != numa_node_id() &&
                           cmpxchg(&lock->locked_no_stealing, 0, 1) == 0))
			goto out;
        }

	this_cpu_inc(spath);
        __aqm_lock_slowpath(lock, node);
	return;

     out:
	this_cpu_inc(fpath);
	return;

}

int aqm_trylock(struct aqm_mutex *lock)
{
        if (atomic_cmpxchg(&lock->val, 0, 1) == 0)
                return 1;
        return 0;
}

int aqm_unlock(struct aqm_mutex *lock)
{
        WRITE_ONCE(lock->locked, 0);
        return 0;
}

/* int atomic_dec_aqm_lock(atomic_t *cnt, struct aqm_mutex *lock); */
