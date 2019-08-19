#ifndef _CSTMCSRWLOCK_H_
#define _CSTMCSRWLOCK_H_

#include <linux/linkage.h>
#include <linux/numa.h>
#include <asm/processor.h>
#include "../spinlock/cohort_common.h"

struct cmcsrw_snode {
	struct cst_qnode *qtail ____cacheline_aligned;

	/*
	 * ANOTHER CACHELINE
	 * tail management
	 */
	/* MCS tail to know who is the next waiter */
	struct cmcsrw_snode *gnext;

	uint64_t status ____cacheline_aligned;

        atomic_t active_readers;
} ____cacheline_aligned;

struct cstmcsrw_lock {
	/* tail for the MCS style */
	struct cmcsrw_snode *gtail;
	/* snode which holds the global lock */
	struct cmcsrw_snode *serving_socket;
	struct cmcsrw_snode nodes[MAX_NUMNODES];
};

#define __CSTMCSRW_LOCK_INITIALIZER(lockname) \
	{ .serving_socket = NULL \
	, .nodes = {{0}} \
	, .gtail = NULL \
	}

#define DEFINE_CMCSMCSRW(x) \
        struct cstmcsrw_lock x = __CSTMCSRW_LOCK_INITIALIZER(x);

void inline cstmcsrw_write_lock(struct cstmcsrw_lock *lock);
void inline cstmcsrw_write_unlock(struct cstmcsrw_lock *lock);

void inline cstmcsrw_read_lock(struct cstmcsrw_lock *lock);
void inline cstmcsrw_read_unlock(struct cstmcsrw_lock *lock);

int inline cstmcsrw_write_trylock(struct cstmcsrw_lock *lock);

int inline cstmcsrw_write_is_locked(struct cstmcsrw_lock *lock);

void cstmcsrw_write_init(struct cstmcsrw_lock *lock);

#endif
