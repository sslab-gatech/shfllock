#ifndef __LINUX_CST_COMMON_H
#define __LINUX_CST_COMMON_H

/**
 * linux-like circular list manipulation
 */
struct qnode {
	struct qnode *next;
	uint32_t status;

	uint32_t parked ____cacheline_aligned;
} ____cacheline_aligned;

/*
 * Used by the spinlock
 */
#define FIRST_ELEM 	1
#define CST_WAIT 		(1 << 30)
#define ACQUIRE_PARENT 	((CST_WAIT) - 1)

#define CST_MAX_NODES 4
#define get_numa_id() 	((numa_node_id()) + 1)
#define snode_id(_n) 	(((_n) - 1) * (CST_MAX_NODES) + idx)
#define get_idx_acq() 	(qnode->count++)
#define get_idx_rel() 	((qnode->count) - 1)

struct __cstmcs_lock {
	union {
		atomic_t val;
		u8 gtail[4];
	};
};

struct cst_qnode {
	struct cst_qnode *next;
	int status;
	int count;
} ____cacheline_aligned;

#endif /* __LINUX_CST_COMMON_H */
