#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/percpu.h>
#include <linux/export.h>
#include <linux/atomic.h>
#include <linux/slab.h>

#include "rwlock/cmcsmcsrw.h"
#include "mutex_cst.h"

static DEFINE_PER_CPU_ALIGNED(struct cst_qnode, percpu_qnode);

static inline void acquire_global(struct cstmcsrw_lock *lock,
				  struct cmcsrw_snode *snode)
{
	struct cmcsrw_snode *old_snode;

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

static int __cstmcs_acquire_local_lock(struct cstmcsrw_lock *lock,
				       struct cmcsrw_snode *snode,
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


static void __always_inline cstmcs_acquire(struct cstmcsrw_lock *lock)
{
	struct cmcsrw_snode *snode;
	struct cst_qnode *qnode;

	snode = &lock->nodes[numa_node_id()];
	qnode = this_cpu_ptr(&percpu_qnode);

	__cstmcs_acquire_local_lock(lock, snode, qnode);
}

static inline void __cstmcsrw_write_global_unlock(struct cstmcsrw_lock *lock,
					       struct cmcsrw_snode *snode)
{
	struct cmcsrw_snode *next_snode = READ_ONCE(snode->gnext);

	if (likely(!next_snode)) {
		if (likely(cmpxchg_release(&lock->gtail, snode, NULL) ==
			   snode))
			return;

		while (!(next_snode = READ_ONCE(snode->gnext)))
			cpu_relax();
	}
	smp_store_release(&next_snode->status, STATE_LOCKED);
}

static inline void __cstmcsrw_write_local_unlock(struct cmcsrw_snode *snode,
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

static void __always_inline cstmcs_release(struct cstmcsrw_lock *lock)
{
	struct cst_qnode *next_qnode, *me;
	struct cmcsrw_snode *snode;
	uint64_t cur_count;

	snode = lock->serving_socket;
	me = this_cpu_ptr(&percpu_qnode);

	cur_count = me->status;
	if(cur_count == NUMA_BATCH_SIZE) {
		__cstmcsrw_write_global_unlock(lock, snode);
		__cstmcsrw_write_local_unlock(snode, me);
		return;
	}

	next_qnode = READ_ONCE(me->next);
	if (next_qnode) {
		smp_store_release(&next_qnode->status, cur_count + 1);
		return;
	}

	__cstmcsrw_write_global_unlock(lock, snode);
	__cstmcsrw_write_local_unlock(snode, me);
}

static inline bool try_acquire_global_lock(struct cstmcsrw_lock *lock,
					   struct cmcsrw_snode *snode)
{
	snode->gnext = NULL;
	snode->status = STATE_PARKED;

	if (cmpxchg(&lock->gtail, NULL, (void *)snode) == NULL) {
		snode->status = STATE_LOCKED;
		return true;
	}
	return false;
}

static inline bool try_acquire_local_lock(struct cmcsrw_snode *snode,
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

static int __always_inline cstmcs_trylock(struct cstmcsrw_lock *lock)
{
	struct cst_qnode *qnode;
	struct cmcsrw_snode *snode;
	int ret;

	snode = &lock->nodes[numa_node_id()];
	qnode = this_cpu_ptr(&percpu_qnode);

	ret = try_acquire_local_lock(snode, qnode);
	if (!ret)
		goto out;

	ret = try_acquire_global_lock(lock, snode);
	if (ret)
		goto out;

	__cstmcsrw_write_local_unlock(snode, qnode);

     out:
	return ret;
}

static inline void wait_for_active_readers(struct cstmcsrw_lock *lock)
{
        struct cmcsrw_snode *snode;
        int nid;

        for_each_online_node(nid) {
                snode = &lock->nodes[nid];

                while (atomic_read(&snode->active_readers))
                        cpu_relax();
        }
}

void inline cstmcsrw_write_lock(struct cstmcsrw_lock *lock)
{
	preempt_disable();
	cstmcs_acquire(lock);

        wait_for_active_readers(lock);
}

void inline cstmcsrw_write_unlock(struct cstmcsrw_lock *lock)
{
	cstmcs_release(lock);
	preempt_enable();
}

int inline cstmcsrw_write_trylock(struct cstmcsrw_lock *lock)
{
	return cstmcs_trylock(lock);
}

int inline cstmcsrw_write_is_locked(struct cstmcsrw_lock *lock)
{
	return !!READ_ONCE(lock->gtail);
}


void inline cstmcsrw_read_lock(struct cstmcsrw_lock *lock)
{
        struct cmcsrw_snode *snode;
        preempt_disable();

        snode = &lock->nodes[numa_node_id()];

        for (;;) {
                while (READ_ONCE(lock->gtail))
                        cpu_relax();

                atomic_inc(&snode->active_readers);
                if (!READ_ONCE(lock->gtail))
                        break;

                atomic_dec(&snode->active_readers);
        }
}

void inline cstmcsrw_read_unlock(struct cstmcsrw_lock *lock)
{
        struct cmcsrw_snode *snode = &lock->nodes[numa_node_id()];
        atomic_dec(&snode->active_readers);
        preempt_enable();
}

void cstmcsrw_write_init(struct cstmcsrw_lock *lock)
{
	lock->serving_socket = NULL;
	lock->gtail = NULL;
	memset(lock->nodes, 0, sizeof(lock->nodes));
}
