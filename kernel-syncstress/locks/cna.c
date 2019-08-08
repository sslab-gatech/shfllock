#include <linux/module.h>
#include <linux/smp.h>
#include <linux/bug.h>
#include <linux/percpu.h>
#include <linux/hardirq.h>
#include <linux/prefetch.h>
#include <linux/atomic.h>
#include <asm/byteorder.h>
#include <linux/random.h>

#include "spinlock/cna.h"
#include "spinlock/qspinlock_stat.h"

#define MAX_NODES	4

/*
 * The pending bit spinning loop count.
 * This heuristic is used to limit the number of lockword accesses
 * made by atomic_cond_read_relaxed when waiting for the lock to
 * transition out of the "== _Q_PENDING_VAL" state. We don't spin
 * indefinitely because there's no guarantee that we'll make forward
 * progress.
 */
#ifndef _Q_PENDING_LOOPS
#define _Q_PENDING_LOOPS	1
#endif

/* Per-CPU pseudo-random number seed */
static DEFINE_PER_CPU(u32, seed);

/*
 * Controls the probability for intra-socket lock hand-off. It can be
 * tuned and depend, e.g., on the number of CPUs per socket. For now,
 * choose a value that provides reasonable long-term fairness without
 * sacrificing performance compared to a version that does not have any
 * fairness guarantees.
 */
#define INTRA_SOCKET_HANDOFF_PROB_ARG  0x10000

/*
 * xorshift function for generating pseudo-random numbers:
 * https://en.wikipedia.org/wiki/Xorshift
 */
static inline u32 xor_random(void)
{
        u32 v;

        v = this_cpu_read(seed);
        if (v == 0)
                get_random_bytes(&v, sizeof(u32));

        v ^= v << 6;
        v ^= v >> 21;
        v ^= v << 7;
        this_cpu_write(seed, v);

        return v;
}

/*
 * Return false with probability 1 / @range.
 * @range must be a power of 2.
 */
static bool probably(unsigned int range)
{
        return xor_random() & (range - 1);
}

/*
 * Per-CPU queue node structures; we can never have more than 4 nested
 * contexts: task, softirq, hardirq, nmi.
 *
 * Exactly fits one 64-byte cacheline on a 64-bit architecture.
 *
 * PV doubles the storage and uses the second cacheline for PV state.
 */
static DEFINE_PER_CPU_ALIGNED(struct mcs_spinlock, mcs_nodes[MAX_NODES]);

#define MCS_NODE(ptr) ((struct mcs_spinlock *)(ptr))

static inline __pure int decode_node(u32 node_and_count)
{
        return (node_and_count >> _Q_NODE_OFFSET) - 1;
}

static inline __pure int decode_count(u32 node_and_count)
{
        return node_and_count & _Q_IDX_MASK;
}

static inline void set_node(struct mcs_spinlock *node, int nid)
{
        u32 val;

        val = (nid + 1) << _Q_NODE_OFFSET;
        val |= decode_count(node->node_and_count);
}

static struct mcs_spinlock *find_successor(struct mcs_spinlock *me,
                                           int cid)
{
        int my_node;
        struct mcs_spinlock *head_other, *tail_other, *cur;

        struct mcs_spinlock *next = me->next;
        /* @next should be set, else we would not be calling this function. */
        WARN_ON_ONCE(next == NULL);

        /* Get socket, which would not be set if we entered an empty queue. */
        my_node = decode_node(me->node_and_count);
        if (my_node == -1)
                my_node = numa_node_id();

        /*
         *  Fast path - check whether the immediate successor runs on
         *  the same socket
         */
        if (decode_node(next->node_and_count) == my_node)
                return next;

        head_other = next;
        tail_other = next;

        /*
         * Traverse the main waiting queue starting from the successor of my
         * successor, and look for a thread running on the same socket.
         */
        cur = READ_ONCE(next->next);
        while (cur) {
                if (decode_node(cur->node_and_count) == my_node) {
                        /*
                         * Found a thread on the same socket. Move threads
                         * between me and that node into the secondary queue.
                         */
                        if (me->locked > 1)
                                MCS_NODE(me->locked)->tail->next = head_other;
                        else
                                me->locked = (uintptr_t)head_other;
                        tail_other->next = NULL;
                        MCS_NODE(me->locked)->tail = tail_other;
                        return cur;
                }
                tail_other = cur;
                cur = READ_ONCE(cur->next);
        }
        return NULL;
}
/*
 * We must be able to distinguish between no-tail and the tail at 0:0,
 * therefore increment the cpu number by one.
 */
static inline __pure u32 encode_tail(int cpu, int idx)
{
	u32 tail;

	BUG_ON(idx > 3);
	tail  = (cpu + 1) << _Q_TAIL_CPU_OFFSET;
	tail |= idx << _Q_TAIL_IDX_OFFSET; /* assume < 4 */

	return tail;
}

static inline __pure struct mcs_spinlock *decode_tail(u32 tail)
{
	int cpu = (tail >> _Q_TAIL_CPU_OFFSET) - 1;
	int idx = (tail &  _Q_TAIL_IDX_MASK) >> _Q_TAIL_IDX_OFFSET;

	return per_cpu_ptr(&mcs_nodes[idx], cpu);
}

#define _Q_LOCKED_PENDING_MASK (_Q_LOCKED_MASK | _Q_PENDING_MASK)

#if _Q_PENDING_BITS == 8
/**
 * clear_pending - clear the pending bit.
 * @lock: Pointer to queued spinlock structure
 *
 * *,1,* -> *,0,*
 */
static __always_inline void clear_pending(struct orig_qspinlock *lock)
{
	WRITE_ONCE(lock->pending, 0);
}

/**
 * clear_pending_set_locked - take ownership and clear the pending bit.
 * @lock: Pointer to queued spinlock structure
 *
 * *,1,0 -> *,0,1
 *
 * Lock stealing is not allowed if this function is used.
 */
static __always_inline void clear_pending_set_locked(struct orig_qspinlock *lock)
{
	WRITE_ONCE(lock->locked_pending, _Q_LOCKED_VAL);
}

/*
 * xchg_tail - Put in the new queue tail code word & retrieve previous one
 * @lock : Pointer to queued spinlock structure
 * @tail : The new queue tail code word
 * Return: The previous queue tail code word
 *
 * xchg(lock, tail), which heads an address dependency
 *
 * p,*,* -> n,*,* ; prev = xchg(lock, node)
 */
static __always_inline u32 xchg_tail(struct orig_qspinlock *lock, u32 tail)
{
	/*
	 * We can use relaxed semantics since the caller ensures that the
	 * MCS node is properly initialized before updating the tail.
	 */
	return (u32)xchg_relaxed(&lock->tail,
				 tail >> _Q_TAIL_OFFSET) << _Q_TAIL_OFFSET;
}

#else /* _Q_PENDING_BITS == 8 */

/**
 * clear_pending - clear the pending bit.
 * @lock: Pointer to queued spinlock structure
 *
 * *,1,* -> *,0,*
 */
static __always_inline void clear_pending(struct orig_qspinlock *lock)
{
	atomic_andnot(_Q_PENDING_VAL, &lock->val);
}

/**
 * clear_pending_set_locked - take ownership and clear the pending bit.
 * @lock: Pointer to queued spinlock structure
 *
 * *,1,0 -> *,0,1
 */
static __always_inline void clear_pending_set_locked(struct orig_qspinlock *lock)
{
	atomic_add(-_Q_PENDING_VAL + _Q_LOCKED_VAL, &lock->val);
}

/**
 * xchg_tail - Put in the new queue tail code word & retrieve previous one
 * @lock : Pointer to queued spinlock structure
 * @tail : The new queue tail code word
 * Return: The previous queue tail code word
 *
 * xchg(lock, tail)
 *
 * p,*,* -> n,*,* ; prev = xchg(lock, node)
 */
static __always_inline u32 xchg_tail(struct orig_qspinlock *lock, u32 tail)
{
	u32 old, new, val = atomic_read(&lock->val);

	for (;;) {
		new = (val & _Q_LOCKED_PENDING_MASK) | tail;
		/*
		 * We can use relaxed semantics since the caller ensures that
		 * the MCS node is properly initialized before updating the
		 * tail.
		 */
		old = atomic_cmpxchg_relaxed(&lock->val, val, new);
		if (old == val)
			break;

		val = old;
	}
	return old;
}
#endif /* _Q_PENDING_BITS == 8 */

/**
 * set_locked - Set the lock bit and own the lock
 * @lock: Pointer to queued spinlock structure
 *
 * *,*,0 -> *,0,1
 */
static __always_inline void set_locked(struct orig_qspinlock *lock)
{
	WRITE_ONCE(lock->locked, _Q_LOCKED_VAL);
}

/*
 * Generate the native code for queued_spin_unlock_slowpath(); provide NOPs for
 * all the PV callbacks.
 */

static __always_inline void __pv_init_node(struct mcs_spinlock *node) { }
static __always_inline void __pv_wait_node(struct mcs_spinlock *node,
					   struct mcs_spinlock *prev) { }
static __always_inline void __pv_kick_node(struct orig_qspinlock *lock,
					   struct mcs_spinlock *node) { }
static __always_inline u32  __pv_wait_head_or_lock(struct orig_qspinlock *lock,
						   struct mcs_spinlock *node)
						   { return 0; }

#define pv_enabled()		false

#define pv_init_node		__pv_init_node
#define pv_wait_node		__pv_wait_node
#define pv_kick_node		__pv_kick_node
#define pv_wait_head_or_lock	__pv_wait_head_or_lock

#ifdef CONFIG_PARAVIRT_SPINLOCKS
#define queued_spin_lock_slowpath	native_queued_spin_lock_slowpath
#endif

/**
 * queued_spin_lock_slowpath - acquire the queued spinlock
 * @lock: Pointer to queued spinlock structure
 * @val: Current value of the queued spinlock 32-bit word
 *
 * (queue tail, pending bit, lock value)
 *
 *              fast     :    slow                                  :    unlock
 *                       :                                          :
 * uncontended  (0,0,0) -:--> (0,0,1) ------------------------------:--> (*,*,0)
 *                       :       | ^--------.------.             /  :
 *                       :       v           \      \            |  :
 * pending               :    (0,1,1) +--> (0,1,0)   \           |  :
 *                       :       | ^--'              |           |  :
 *                       :       v                   |           |  :
 * uncontended           :    (n,x,y) +--> (n,0,0) --'           |  :
 *   queue               :       | ^--'                          |  :
 *                       :       v                               |  :
 * contended             :    (*,x,y) +--> (*,0,0) ---> (*,0,1) -'  :
 *   queue               :         ^--'                             :
 */
void cna_spin_lock_slowpath(struct orig_qspinlock *lock, u32 val)
{
	struct mcs_spinlock *prev, *next, *node, *succ;
	u32 old, tail, new;
	int idx, cid;

	BUILD_BUG_ON(CONFIG_NR_CPUS >= (1U << _Q_TAIL_CPU_BITS));

	/* if (pv_enabled()) */
		/* goto pv_queue; */

	/* if (virt_spin_lock(lock)) */
	/* 	return; */

	/*
	 * Wait for in-progress pending->locked hand-overs with a bounded
	 * number of spins so that we guarantee forward progress.
	 *
	 * 0,1,0 -> 0,0,1
	 */
	if (val == _Q_PENDING_VAL) {
		int cnt = _Q_PENDING_LOOPS;
		val = atomic_cond_read_relaxed(&lock->val,
					       (VAL != _Q_PENDING_VAL) || !cnt--);
	}

	/*
	 * If we observe any contention; queue.
	 */
	if (val & ~_Q_LOCKED_MASK)
		goto queue;

	/*
	 * trylock || pending
	 *
	 * 0,0,0 -> 0,0,1 ; trylock
	 * 0,0,1 -> 0,1,1 ; pending
	 */
	val = atomic_fetch_or_acquire(_Q_PENDING_VAL, &lock->val);
	if (!(val & ~_Q_LOCKED_MASK)) {
		/*
		 * We're pending, wait for the owner to go away.
		 *
		 * *,1,1 -> *,1,0
		 *
		 * this wait loop must be a load-acquire such that we match the
		 * store-release that clears the locked bit and create lock
		 * sequentiality; this is because not all
		 * clear_pending_set_locked() implementations imply full
		 * barriers.
		 */
		if (val & _Q_LOCKED_MASK) {
			atomic_cond_read_acquire(&lock->val,
						 !(VAL & _Q_LOCKED_MASK));
		}

		/*
		 * take ownership and clear the pending bit.
		 *
		 * *,1,0 -> *,0,1
		 */
		clear_pending_set_locked(lock);
		qstat_inc(qstat_lock_pending, true);
		return;
	}

	/*
	 * If pending was clear but there are waiters in the queue, then
	 * we need to undo our setting of pending before we queue ourselves.
	 */
	if (!(val & _Q_PENDING_MASK))
		clear_pending(lock);

	/*
	 * End of pending bit optimistic spinning and beginning of MCS
	 * queuing.
	 */
queue:
	qstat_inc(qstat_lock_slowpath, true);
pv_queue:
	node = this_cpu_ptr(&mcs_nodes[0]);
	/* idx = node->count++; */
	/* tail = encode_tail(smp_processor_id(), idx); */
        idx = decode_count(node->node_and_count);
        cid = smp_processor_id();
        tail = encode_tail(cid, idx);


	node += idx;

	/*
	 * Ensure that we increment the head node->count before initialising
	 * the actual node. If the compiler is kind enough to reorder these
	 * stores, then an IRQ could overwrite our assignments.
	 */
	barrier();

	node->locked = 0;
	node->next = NULL;
        set_node(node, -1);
        node->encoded_tail = tail;
	pv_init_node(node);

	/*
	 * We touched a (possibly) cold cacheline in the per-cpu queue node;
	 * attempt the trylock once more in the hope someone let go while we
	 * weren't watching.
	 */
	if (cna_spin_trylock(lock))
		goto release;

	/*
	 * Ensure that the initialisation of @node is complete before we
	 * publish the updated tail via xchg_tail() and potentially link
	 * @node into the waitqueue via WRITE_ONCE(prev->next, node) below.
	 */
	smp_wmb();

	/*
	 * Publish the updated tail.
	 * We have already touched the queueing cacheline; don't bother with
	 * pending stuff.
	 *
	 * p,*,* -> n,*,*
	 */
	old = xchg_tail(lock, tail);
	next = NULL;

	/*
	 * if there was a previous node; link it and wait until reaching the
	 * head of the waitqueue.
	 */
	if (old & _Q_TAIL_MASK) {
		prev = decode_tail(old);

                /*
                 * An explicit barrier after the store to @socket
                 * is not required as making the socket value visible is
                 * required only for performance, not correctness, and
                 * we rather avoid the cost of the barrier.
                 */
                set_node(node, numa_node_id());

		/* Link @node into the waitqueue. */
		WRITE_ONCE(prev->next, node);

		pv_wait_node(node, prev);
		arch_mcs_spin_lock_contended(&node->locked);

		/*
		 * While waiting for the MCS lock, the next pointer may have
		 * been set by another lock waiter. We optimistically load
		 * the next pointer & prefetch the cacheline for writing
		 * to reduce latency in the upcoming MCS unlock operation.
		 */
		next = READ_ONCE(node->next);
		if (next)
			prefetchw(next);
	} else {
                 /* Must pass a non-zero value to successor when we unlock. */
                node->locked = 1;
        }

	/*
	 * we're at the head of the waitqueue, wait for the owner & pending to
	 * go away.
	 *
	 * *,x,y -> *,0,0
	 *
	 * this wait loop must use a load-acquire such that we match the
	 * store-release that clears the locked bit and create lock
	 * sequentiality; this is because the set_locked() function below
	 * does not imply a full barrier.
	 *
	 * The PV pv_wait_head_or_lock function, if active, will acquire
	 * the lock and return a non-zero value. So we have to skip the
	 * atomic_cond_read_acquire() call. As the next PV queue head hasn't
	 * been designated yet, there is no way for the locked value to become
	 * _Q_SLOW_VAL. So both the set_locked() and the
	 * atomic_cmpxchg_relaxed() calls will be safe.
	 *
	 * If PV isn't active, 0 will be returned instead.
	 *
	 */
	if ((val = pv_wait_head_or_lock(lock, node)))
		goto locked;

	val = atomic_cond_read_acquire(&lock->val, !(VAL & _Q_LOCKED_PENDING_MASK));

locked:
	/*
	 * claim the lock:
	 *
	 * n,0,0 -> 0,0,1 : lock, uncontended
	 * *,*,0 -> *,*,1 : lock, contended
	 *
	 * If the queue head is the only one in the queue (lock value == tail)
	 * and nobody is pending, clear the tail code and grab the lock.
	 * Otherwise, we only need to grab the lock.
	 */

	/*
	 * In the PV case we might already have _Q_LOCKED_VAL set.
	 *
	 * The atomic_cond_read_acquire() call above has provided the
	 * necessary acquire semantics required for locking.
	 */
	/* if (((val & _Q_TAIL_MASK) == tail) && */
	/*     atomic_try_cmpxchg_relaxed(&lock->val, &val, _Q_LOCKED_VAL)) */
	/* 	goto release; /1* No contention *1/ */

        if ((val & _Q_TAIL_MASK) == tail) {
                /* Check whether the secondary queue is empty. */
                if (node->locked == 1) {
                        if (atomic_try_cmpxchg_relaxed(&lock->val, &val,
                                                       _Q_LOCKED_VAL))
                                goto release; /* No contention */
                } else {
                        /*
                         * Pass the lock to the first thread in the secondary
                         * queue, but first try to update the queue's tail to
                         * point to the last node in the secondary queue.
                         */
                        succ = MCS_NODE(node->locked);
                        new = succ->tail->encoded_tail + _Q_LOCKED_VAL;
                        if (atomic_try_cmpxchg_relaxed(&lock->val, &val, new)) {
                                arch_mcs_spin_unlock_contended(&succ->locked, 1);
                                goto release;
                        }
                }
        }
	/* Either somebody is queued behind us or _Q_PENDING_VAL is set */
	set_locked(lock);

	/*
	 * contended path; wait for next if not observed yet, release.
	 */
	if (!next)
		next = smp_cond_load_relaxed(&node->next, (VAL));

        /* /1* Try to pass the lock to a thread running on the same socket. *1/ */
        /* succ = find_successor(node, cid); */
        /*
         *  Try to pass the lock to a thread running on the same socket.
         *  For long-term fairness, search for such a thread with high
         *  probability rather than always.
         */
        succ = NULL;
        if (probably(INTRA_SOCKET_HANDOFF_PROB_ARG))
                succ = find_successor(node, cid);

        if (succ) {
                arch_mcs_spin_unlock_contended(&succ->locked, node->locked);
        } else if (node->locked > 1) {
                /*
                 * If the secondary queue is not empty, pass the lock
                 * to the first node in that queue.
                 */
                succ = MCS_NODE(node->locked);
                succ->tail->next = next;
                arch_mcs_spin_unlock_contended(&succ->locked, 1);
        } else {
                /*
                 * Otherwise, pass the lock to the immediate successor
                 * in the main queue
                 */
                arch_mcs_spin_unlock_contended(&next->locked, 1);
        }
	pv_kick_node(lock, next);

release:
	/*
	 * release the node
	 */
	__this_cpu_dec(mcs_nodes[0].node_and_count);
}
