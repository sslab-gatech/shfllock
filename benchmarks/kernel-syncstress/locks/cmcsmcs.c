#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/percpu.h>
#include <linux/export.h>
#include <linux/atomic.h>
#include <linux/slab.h>

#include "spinlock/cmcsmcs.h"
#include "mutex_cst.h"

static DEFINE_PER_CPU_ALIGNED(struct cst_qnode, percpu_qnode);

static inline void acquire_global(struct cstmcs_lock *lock,
				  struct cst_snode *snode)
{
	struct cst_snode *old_snode;

	snode->gnext = NULL;
	snode->status = STATE_PARKED;

	old_snode = xchg(&lock->gtail, snode);
	if (likely(old_snode == NULL)) {
		snode->status = STATE_LOCKED;
		return;
	}

	WRITE_ONCE(old_snode->gnext, snode);

	while(smp_load_acquire(&snode->status) == STATE_PARKED) {
		cpu_relax();
	}
}

static int __cstmcs_acquire_local_lock(struct cstmcs_lock *lock,
				       struct cst_snode *snode,
				       struct cst_qnode *qnode)
{
	struct cst_qnode *old_qnode;

	qnode->next = NULL;
	qnode->status = CST_WAIT;

	old_qnode = xchg(&snode->qtail, qnode);
	if (old_qnode) {
		uint64_t cur_status;

		WRITE_ONCE(old_qnode->next, qnode);

		while((cur_status = smp_load_acquire(&qnode->status)) == CST_WAIT)
			cpu_relax();

		if (cur_status < ACQUIRE_PARENT)
			goto out;
	}

	qnode->status = FIRST_ELEM;
	acquire_global(lock, snode);
	WRITE_ONCE(lock->serving_socket, snode);

     out:
	return 0;
}


static void __always_inline cstmcs_acquire(struct cstmcs_lock *lock)
{
	struct cst_snode *snode;
	struct cst_qnode *qnode;

	snode = &lock->nodes[numa_node_id()];
	qnode = this_cpu_ptr(&percpu_qnode);

	__cstmcs_acquire_local_lock(lock, snode, qnode);
}

static inline void __cstmcs_spin_global_unlock(struct cstmcs_lock *lock,
					       struct cst_snode *snode)
{
	struct cst_snode *next_snode = READ_ONCE(snode->gnext);

	if (likely(!next_snode)) {
		if (likely(cmpxchg_release(&lock->gtail, snode, NULL) ==
			   snode))
			return;

		while (!(next_snode = READ_ONCE(snode->gnext)))
			cpu_relax();
	}
	smp_store_release(&next_snode->status, STATE_LOCKED);
}

static inline void __cstmcs_spin_local_unlock(struct cst_snode *snode,
					      struct cst_qnode *qnode)
{
	struct cst_qnode *next_qnode = READ_ONCE(qnode->next);

	if (likely(!next_qnode)) {
		if (likely(cmpxchg_release(&snode->qtail, qnode, NULL) ==
			   qnode))
			return;

		while(!(next_qnode = READ_ONCE(qnode->next)))
			cpu_relax();
	}

	smp_store_release(&next_qnode->status, ACQUIRE_PARENT);
}

static void __always_inline cstmcs_release(struct cstmcs_lock *lock)
{
	struct cst_qnode *next_qnode, *me;
	struct cst_snode *snode;
	uint64_t cur_count;

	snode = lock->serving_socket;
	me = this_cpu_ptr(&percpu_qnode);

	cur_count = me->status;
	if(cur_count == NUMA_BATCH_SIZE) {
		__cstmcs_spin_global_unlock(lock, snode);
		__cstmcs_spin_local_unlock(snode, me);
		return;
	}

	next_qnode = READ_ONCE(me->next);
	if (next_qnode) {
		smp_store_release(&next_qnode->status, cur_count + 1);
		return;
	}

	__cstmcs_spin_global_unlock(lock, snode);
	__cstmcs_spin_local_unlock(snode, me);
}

static inline bool try_acquire_global_lock(struct cstmcs_lock *lock,
					   struct cst_snode *snode)
{
	snode->gnext = NULL;
	snode->status = STATE_PARKED;

	if (cmpxchg(&lock->gtail, NULL, (void *)snode) == NULL) {
		snode->status = STATE_LOCKED;
		return true;
	}
	return false;
}

static inline bool try_acquire_local_lock(struct cst_snode *snode,
					  struct cst_qnode *qnode)
{
	qnode->next = NULL;
	qnode->status = STATE_PARKED;

	if (cmpxchg(&snode->qtail, NULL, (void *)qnode) == NULL) {
		snode->status = FIRST_ELEM;
		return true;
	}
	return false;
}

static int __always_inline cstmcs_trylock(struct cstmcs_lock *lock)
{
	struct cst_qnode *qnode;
	struct cst_snode *snode;
	int ret;

	snode = &lock->nodes[numa_node_id()];
	qnode = this_cpu_ptr(&percpu_qnode);

	ret = try_acquire_local_lock(snode, qnode);
	if (!ret)
		goto out;

	ret = try_acquire_global_lock(lock, snode);
	if (ret)
		goto out;

	__cstmcs_spin_local_unlock(snode, qnode);

     out:
	return ret;
}

void inline cstmcs_spin_lock(struct cstmcs_lock *lock)
{
	preempt_disable();
	cstmcs_acquire(lock);
}

void inline cstmcs_spin_unlock(struct cstmcs_lock *lock)
{
	cstmcs_release(lock);
	preempt_enable();
}

void inline cstmcs_spin_lock_irq(struct cstmcs_lock *lock)
{
	local_irq_disable();
	cstmcs_spin_lock(lock);
}

void inline cstmcs_spin_unlock_irq(struct cstmcs_lock *lock)
{
	cstmcs_release(lock);
	local_irq_enable();
	preempt_enable();
}

void inline cstmcs_spin_unlock_irqrestore(struct cstmcs_lock *lock,
					  unsigned long flags)
{
	cstmcs_release(lock);
	local_irq_restore(flags);
	preempt_enable();
}

int inline cstmcs_spin_trylock(struct cstmcs_lock *lock)
{
	return cstmcs_trylock(lock);
}

int inline cstmcs_spin_is_locked(struct cstmcs_lock *lock)
{
	return !!READ_ONCE(lock->gtail);
}

void cstmcs_spin_init(struct cstmcs_lock *lock)
{
	lock->serving_socket = NULL;
	lock->gtail = NULL;
	memset(lock->nodes, 0, sizeof(lock->nodes));
}
