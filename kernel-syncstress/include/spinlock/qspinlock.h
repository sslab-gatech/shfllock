#ifndef __QUEUED_SPINLOCK_H__
#define __QUEUED_SPINLOCK_H__

#include "qspinlock_i.h"

#define DEFINE_QSPINLOCK(x)    \
    struct orig_qspinlock (x) = (struct orig_qspinlock)__ORIG_QSPIN_LOCK_UNLOCKED

/**
 * queued_spin_is_locked - is the spinlock locked?
 * @lock: Pointer to queued spinlock structure
 * Return: 1 if it is locked, 0 otherwise
 */
static __always_inline
int orig_queued_spin_is_locked(struct orig_qspinlock *lock)
{
	/*
	 * Any !0 state indicates it is locked, even if _Q_LOCKED_VAL
	 * isn't immediately observable.
	 */
	return atomic_read(&lock->val);
}

extern
void orig_queued_spin_lock_slowpath(struct orig_qspinlock *lock, u32 val);
/**
 * queued_spin_lock - acquire a queued spinlock
 * @lock: Pointer to queued spinlock structure
 */
static __always_inline
void orig_queued_spin_lock(struct orig_qspinlock *lock)
{
	u32 val;

	val = atomic_cmpxchg_acquire(&lock->val, 0, _Q_LOCKED_VAL);
	if (likely(val == 0))
		return;
	orig_queued_spin_lock_slowpath(lock, val);
}

static __always_inline
void orig_queued_spin_unlock(struct orig_qspinlock *lock)
{
	/*
	 * smp_mb__before_atomic() in order to guarantee release semantics
	 */
        smp_store_release(&lock->locked, 0);
}

/**
 * queued_spin_trylock - try to acquire the queued spinlock
 * @lock : Pointer to queued spinlock structure
 * Return: 1 if lock acquired, 0 if failed
 */
static __always_inline
int orig_queued_spin_trylock(struct orig_qspinlock *lock)
{
	if (!atomic_read(&lock->val) &&
	   (atomic_cmpxchg_acquire(&lock->val, 0, _Q_LOCKED_VAL) == 0))
		return 1;
	return 0;
}

#endif /* __QUEUED_SPINLOCK_H__ */
