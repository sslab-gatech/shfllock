#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/percpu.h>
#include <linux/export.h>
#include <linux/atomic.h>
#include <linux/slab.h>

#include "spinlock/ctasmcs.h"
#include "mutex_cst.h"

static DEFINE_PER_CPU_ALIGNED(struct cst_qnode, percpu_qnode);

static inline void acquire_global(struct ctasmcs_lock *lock,
				  struct cmcsnode *snode)
{
        for (;;) {
                if (atomic_cmpxchg_acquire(&lock->glock, 0, 1) == 0)
                        break;

                cpu_relax();
        }
}

static int __ctasmcsacquire_local_lock(struct ctasmcs_lock *lock,
				       struct cmcsnode *snode,
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


static void __always_inline ctasmcsacquire(struct ctasmcs_lock *lock)
{
	struct cmcsnode *snode;
	struct cst_qnode *qnode;

	snode = &lock->nodes[numa_node_id()];
	qnode = this_cpu_ptr(&percpu_qnode);

	__ctasmcsacquire_local_lock(lock, snode, qnode);
}

static inline void __ctasmcsspin_global_unlock(struct ctasmcs_lock *lock,
					       struct cmcsnode *snode)
{
	smp_store_release(&lock->glock.counter, 0);
}

static inline void __ctasmcsspin_local_unlock(struct cmcsnode *snode,
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

void __always_inline ctasmcsrelease(struct ctasmcs_lock *lock)
{
	struct cst_qnode *next_qnode, *me;
	struct cmcsnode *snode;
	uint64_t cur_count;

	snode = lock->serving_socket;
	me = this_cpu_ptr(&percpu_qnode);

	cur_count = me->status;
	if(cur_count == NUMA_BATCH_SIZE) {
		__ctasmcsspin_global_unlock(lock, snode);
		__ctasmcsspin_local_unlock(snode, me);
		return;
	}

	next_qnode = READ_ONCE(me->next);
	if (next_qnode) {
		smp_store_release(&next_qnode->status, cur_count + 1);
		return;
	}

	__ctasmcsspin_global_unlock(lock, snode);
	__ctasmcsspin_local_unlock(snode, me);
}

static inline bool try_acquire_global_lock(struct ctasmcs_lock *lock,
					   struct cmcsnode *snode)
{
	if (atomic_cmpxchg(&lock->glock, 0, 1) == 0) {
		snode->status = STATE_LOCKED;
		return true;
	}
	return false;
}

static inline bool try_acquire_local_lock(struct cmcsnode *snode,
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

static int __always_inline ctasmcstrylock(struct ctasmcs_lock *lock)
{
	struct cst_qnode *qnode;
	struct cmcsnode *snode;
	int ret;

	snode = &lock->nodes[numa_node_id()];
	qnode = this_cpu_ptr(&percpu_qnode);

	ret = try_acquire_local_lock(snode, qnode);
	if (!ret)
		goto out;

	ret = try_acquire_global_lock(lock, snode);
	if (ret)
		goto out;

	__ctasmcsspin_local_unlock(snode, qnode);

     out:
	return ret;
}

void inline ctasmcsspin_lock(struct ctasmcs_lock *lock)
{
	preempt_disable();
	ctasmcsacquire(lock);
}

void inline ctasmcsspin_unlock(struct ctasmcs_lock *lock)
{
	ctasmcsrelease(lock);
	preempt_enable();
}

void inline ctasmcsspin_lock_irq(struct ctasmcs_lock *lock)
{
	local_irq_disable();
	ctasmcsspin_lock(lock);
}

void inline ctasmcsspin_unlock_irq(struct ctasmcs_lock *lock)
{
	ctasmcsrelease(lock);
	local_irq_enable();
	preempt_enable();
}

void inline ctasmcsspin_unlock_irqrestore(struct ctasmcs_lock *lock,
					  unsigned long flags)
{
	ctasmcsrelease(lock);
	local_irq_restore(flags);
	preempt_enable();
}

int inline ctasmcsspin_trylock(struct ctasmcs_lock *lock)
{
	return ctasmcstrylock(lock);
}

int inline ctasmcsspin_is_locked(struct ctasmcs_lock *lock)
{
        return atomic_read(&lock->glock);
}

void ctasmcsspin_init(struct ctasmcs_lock *lock)
{
	lock->serving_socket = NULL;
        atomic_set(&lock->glock, 0);
	memset(lock->nodes, 0, sizeof(lock->nodes));
}
