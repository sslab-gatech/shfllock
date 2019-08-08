#ifndef _CSTMCSLOCK_H_
#define _CSTMCSLOCK_H_

#include <linux/linkage.h>
#include <linux/numa.h>
#include <asm/processor.h>
#include "cohort_common.h"

struct cst_snode {
	/*
	 * ONE CACHELINE
	 * ticket info
	 */
	struct cst_qnode *qtail;

	/*
	 * ANOTHER CACHELINE
	 * tail management
	 */
	/* MCS tail to know who is the next waiter */
	struct cst_snode *gnext ____cacheline_aligned;
	uint64_t status;

} ____cacheline_aligned;

struct cstmcs_lock {
	/* tail for the MCS style */
	struct cst_snode *gtail;
	/* snode which holds the global lock */
	struct cst_snode *serving_socket;
	struct cst_snode nodes[MAX_NUMNODES];
};

#define __CSTMCS_LOCK_INITIALIZER(lockname) \
	{ .serving_socket = NULL \
	, .nodes = {{0}} \
	, .gtail = NULL \
	}

#define cstmcs_spin_lock_irqsave(_l, _f) 		\
	do { 						\
		local_irq_save(_f);			\
		cstmcs_spin_lock_irq(_l); 		\
	} while (0)

#define cstmcs_spin_trylock_irqsave(_l, _f) 		\
	({ 						\
		local_irq_save(_f);			\
		cstmcs_spin_trylock(_l)?1:		\
		({local_irq_restore(flags); 0;});	\
	})

#define DEFINE_CMCSMCS(x) \
        struct cstmcs_lock x = __CSTMCS_LOCK_INITIALIZER(x);

void inline cstmcs_spin_lock(struct cstmcs_lock *lock);
void inline cstmcs_spin_unlock(struct cstmcs_lock *lock);

void inline cstmcs_spin_lock_irq(struct cstmcs_lock *lock);
void inline cstmcs_spin_unlock_irq(struct cstmcs_lock *lock);

void inline cstmcs_spin_unlock_irqrestore(struct cstmcs_lock *lock,
					  unsigned long flags);

int inline cstmcs_spin_trylock(struct cstmcs_lock *lock);

int inline cstmcs_spin_is_locked(struct cstmcs_lock *lock);

void cstmcs_spin_init(struct cstmcs_lock *lock);

#endif
