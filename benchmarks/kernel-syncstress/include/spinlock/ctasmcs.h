#ifndef _CHRT_TAS_MCS_H_
#define _CHRT_TAS_MCS_H_

#include <linux/linkage.h>
#include <linux/numa.h>
#include <asm/processor.h>
#include "cohort_common.h"

struct cmcsnode {
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
	struct cmcsnode *gnext ____cacheline_aligned;
	uint64_t status;

} ____cacheline_aligned;

struct ctasmcs_lock {
	/* tail for the MCS style */
        atomic_t glock;
	/* snode which holds the global lock */
	struct cmcsnode *serving_socket;
	struct cmcsnode nodes[MAX_NUMNODES];
};

#define __CTASMCS_LOCK_INITIALIZER(lockname) \
	{ .serving_socket = NULL \
	, .nodes = {{0}} \
	, .glock = ATOMIC_INIT(0) \
	}

#define DEFINE_CTASMCS(x) \
        struct ctasmcs_lock x = __CTASMCS_LOCK_INITIALIZER(x);

#define ctasmcsspin_lock_irqsave(_l, _f) 		\
	do { 						\
		local_irq_save(_f);			\
		ctasmcsspin_lock_irq(_l); 		\
	} while (0)

#define ctasmcsspin_trylock_irqsave(_l, _f) 		\
	({ 						\
		local_irq_save(_f);			\
		ctasmcsspin_trylock(_l)?1:		\
		({local_irq_restore(flags); 0;});	\
	})

void inline ctasmcsspin_lock(struct ctasmcs_lock *lock);
void inline ctasmcsspin_unlock(struct ctasmcs_lock *lock);

void inline ctasmcsspin_lock_irq(struct ctasmcs_lock *lock);
void inline ctasmcsspin_unlock_irq(struct ctasmcs_lock *lock);

void inline ctasmcsspin_unlock_irqrestore(struct ctasmcs_lock *lock,
					  unsigned long flags);

int inline ctasmcsspin_trylock(struct ctasmcs_lock *lock);

int inline ctasmcsspin_is_locked(struct ctasmcs_lock *lock);

void ctasmcsspin_init(struct ctasmcs_lock *lock);

#endif
