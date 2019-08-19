#include <linux/module.h>
#include <linux/smp.h>
#include <linux/bug.h>
#include <linux/percpu.h>
#include <linux/hardirq.h>
#include <linux/prefetch.h>
#include <linux/atomic.h>
#include <asm/byteorder.h>
#include <linux/random.h>

#ifdef KERNEL_SYNCSTRESS
#include "spinlock/aqs.h"
#else
#include <linux/aqs.h>
#endif

#define ENABLE_SHUFFLERS                        1
#define ALLOW_PREDECESSORS_AS_SHUFFLERS         1
#define SELECT_SLEADER                          1

static DEFINE_PER_CPU_ALIGNED(struct aqs_node, aqs_nodes[4]);

#define _AQS_NOSTEAL_VAL        (1U << (_AQS_LOCKED_OFFSET + _AQS_LOCKED_BITS))
#define AQS_MAX_PATIENCE_COUNT  2
#define MAX_CONT_SHFLD_COUNT    2

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

static inline void enable_stealing(struct aqs_lock *lock)
{
        atomic_andnot(_AQS_NOSTEAL_VAL, &lock->val);
}

static inline void disable_stealing(struct aqs_lock *lock)
{
        atomic_fetch_or_acquire(_AQS_NOSTEAL_VAL, &lock->val);
}

static inline u8 is_stealing_disabled(struct aqs_lock *lock)
{
        return smp_load_acquire(&lock->no_stealing);
}

static inline void set_sleader(struct aqs_node *node, struct aqs_node *qend)
{
        smp_store_release(&node->sleader, 1);
        if (qend != node)
                smp_store_release(&node->last_visited, qend);
}

static inline void clear_sleader(struct aqs_node *node)
{
        node->sleader = 0;
}

static inline void set_waitcount(struct aqs_node *node, int count)
{
        smp_store_release(&node->wcount, count);
}

static inline __pure u16 encode_tail(int cpu, int idx)
{
        u32 tail;

        tail = (cpu + 1) << _AQS_TAIL_CPU_OFFSET;
        tail |= idx << _AQS_TAIL_IDX_OFFSET;

        return tail;
}

static __always_inline u16 xchg_tail(struct aqs_lock *lock, u32 tail)
{
        return (u32)xchg_relaxed(&lock->tail, tail);
}

static inline __pure struct aqs_node *decode_tail(u16 tail)
{
        int cpu = (tail >> _AQS_TAIL_CPU_OFFSET) - 1;
        int idx = (tail & _AQS_TAIL_IDX_MASK) >> _AQS_TAIL_IDX_OFFSET;

        return per_cpu_ptr(&aqs_nodes[idx], cpu);
}

/*
 * This function is responsible for aggregating waiters in a
 * particular socket in one place up to a certain batch count.
 * The invariant is that the shuffle leaders always start from
 * the very next waiter and they are selected ahead in the queue,
 * if required. Moreover, none of the waiters will be behind the
 * shuffle leader, they are always ahead in the queue.
 * Currently, only one shuffle leader is chosen.
 * TODO: Another aggressive approach could be to use HOH locking
 * for n shuffle leaders, in which n corresponds to the number
 * of sockets.
 */
static void shuffle_waiters(struct aqs_lock *lock, struct aqs_node *node,
                            int is_next_waiter)
{
        struct aqs_node *curr, *prev, *next, *last, *sleader, *qend;
        int nid;
        int curr_locked_count;
        int one_shuffle = false;
        int shuffled_count = 0;

#ifdef ENABLE_SHUFFLERS
        return;
#endif

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
        if (!probably(INTRA_SOCKET_HANDOFF_PROB_ARG)) {
                sleader = READ_ONCE(node->next);
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

             recheck_curr_tail:

                /*
                 * If we are the last one in the tail, then
                 * we cannot do anything, we should return back
                 * while selecting the next sleader as the last one
                 */
                if (curr->cid == READ_ONCE(lock->tail) >> _AQS_TAIL_CPU_OFFSET) {
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
                                smp_wmb();

                                last = curr;
                                curr = next;
                                one_shuffle = true;
                                shuffled_count++;

                                if (!is_next_waiter)
                                        goto recheck_curr_tail;
                        }
                } else
                        prev = curr;

                if (is_next_waiter) {

                        if ((one_shuffle && !READ_ONCE(lock->locked)) ||
                            shuffled_count >= MAX_CONT_SHFLD_COUNT) {
                                sleader = last;
                                qend = prev;
                                break;
                        }

                } else if (smp_load_acquire(&node->lstatus)) {

                        sleader = last;
                        qend = prev;
                        break;
                }
        }

     out:
#ifndef SELECT_SLEADER
        if (sleader) {
                set_sleader(sleader, qend);
        }
#endif
}

void aqs_spin_lock_slowpath(struct aqs_lock *lock)
{
        struct aqs_node *node, *prev, *next;
        u16 old, tail;
        int idx, cid;
        int patience_count = AQS_MAX_PATIENCE_COUNT;

        cid = smp_processor_id();

        node = this_cpu_ptr(&aqs_nodes[0]);
        idx = node->count++;
        tail = encode_tail(cid, idx);

        node += idx;

        barrier();

        node->next = NULL;
        node->last_visited = NULL;
        node->locked = AQS_STATUS_WAIT;
        node->nid = numa_node_id();
        node->cid = cid + 1;

        /*
         * Ensure that the initialisation of @node is complete before we
         * publish the updated tail via xchg_tail() and potentially link
         * @node into the waitqueue via WRITE_ONCE(prev->next, node) below.
         */
        smp_wmb();

        /*
         * Publish the updated tail.
         */
        old = xchg_tail(lock, tail);
        next = NULL;

        /*
         * if there was a previous node; link it and wait until reaching the
         * head of the waitqueue.
         */
        if (old) {
                prev = decode_tail(old);

                WRITE_ONCE(prev->next, node);

                /*
                 * Now, we are waiting for the lock holder to
                 * allow us to become the very next lock waiter.
                 * In the meantime, we also check whether the node
                 * is the shuffle leader, if that's the case,
                 * then it goes on shuffling waiters in its socket
                 */
                for (;;) {
                        if (READ_ONCE(node->lstatus) == AQS_STATUS_LOCKED)
                                break;

#ifdef ALLOW_PREDECESSORS_AS_SHUFFLERS
                        if (READ_ONCE(node->sleader))
                                shuffle_waiters(lock, node, false);
#endif
                }
                cpu_relax();
        }

        /*
         * we are now the very next waiters, all we have to do is
         * to wait for the @lock->locked to become 0, i.e. unlocked.
         * In the meantime, we will try to be shuffle leader if possible
         * and at least find someone in my socket.
         */
        for (;;) {
                int wcount;

                if (!READ_ONCE(lock->locked))
                        break;
         /*
                 * There are two ways to become a shuffle leader:
                 * 1) my @node->wcount is 0
                 * 2) someone or myself (earlier) appointed me
                 * (@node->sleader = 1)
                 */

                wcount = node->wcount;
                if ((!wcount ||
                /* if (do_shuffling(node) && (!wcount || */
                    (wcount && node->sleader)))
                        shuffle_waiters(lock, node, true);

                cpu_relax();
        }

        /*
         * The biggest catch with our algorithm is that it allows
         * stealing in the fast path.
         * Thus, even if the @lock->locked was 0 above, it doesn't
         * mean that we have the lock. So, we acquire the lock
         * in two ways:
         * 1) Either someone disabled the lock stealing before us
         * that allows us to directly set the lock->locked value 1
         * 2) Or, I will explicitly try to do a cmpxchg operation
         * on the @lock->locked variable. If I am unsuccessful for
         * @impatient_cap times, then I explicitly lock stealing,
         * this is to ensure starvation freedom, and will wait
         * for the lock->locked status to change to 0.
         */
        for (;;) {
                /*
                 * If someone already disabled stealing,
                 * change locked and proceed forward
                 */
                if (patience_count == 0 && !is_stealing_disabled(lock))
                        disable_stealing(lock);

                if (cmpxchg(&lock->locked, 0, 1) == 0)
                        break;

                while (smp_load_acquire(&lock->locked))
                        cpu_relax();

                patience_count--;

                cpu_relax();
        }

        next = READ_ONCE(node->next);
        if (!next) {
                if (cmpxchg(&lock->tail, tail, 0) == tail) {
                        enable_stealing(lock);
                        goto release;
                }

                for (;;) {
                        next = READ_ONCE(node->next);
                        if (next)
                                break;
                        cpu_relax();
                }
        }
        /*
         * Notify whoever the next guy to become the very
         * next lock waiter and be ready to get into the CS
         */
        WRITE_ONCE(next->lstatus, AQS_STATUS_LOCKED);

        if (next->nid != node->nid)
                enable_stealing(lock);

     release:
        __this_cpu_dec(aqs_nodes[0].count);
}
EXPORT_SYMBOL(aqs_spin_lock_slowpath);
