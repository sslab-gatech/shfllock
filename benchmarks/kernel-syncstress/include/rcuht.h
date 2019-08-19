#ifndef __RCU_HT_H_
#define __RCU_HT_H_

#define DECLARE_TABLE_LOCK(l, linit,                            \
                            wl, wul, rl, rul)                   \
                                                                \
static __cacheline_aligned_in_smp   linit(l);                   \
                                                                \
static void l ## _write_lock_buckets(struct rcuhashbash_bucket *b1,     \
                                     struct rcuhashbash_bucket *b2)     \
{                                                               \
        preempt_disable();                                      \
        wl(&l);                                                 \
}                                                               \
                                                                \
static void l ## _write_unlock_buckets(struct rcuhashbash_bucket *b1,   \
                                       struct rcuhashbash_bucket *b2)   \
{                                                               \
        wul(&l);                                                \
        preempt_enable();                                       \
}                                                               \
                                                                \
static void l ## _read_lock_bucket(struct rcuhashbash_bucket *bucket)   \
{                                                               \
        preempt_disable();                                      \
        rl(&l);                                                 \
}                                                               \
                                                                \
static void l ## _read_unlock_bucket(struct rcuhashbash_bucket *bucket) \
{                                                               \
        rul(&l);                                                \
        preempt_enable();                                       \
}                                                               \



#define DECLARE_TABLE_LOCK_W_NODE(l, linit,                     \
                            wl, wul, rl, rul, _t_)              \
                                                                \
static __cacheline_aligned_in_smp   linit(l);                   \
                                                                \
static void l ## _write_lock_buckets(struct rcuhashbash_bucket *b1,     \
                                     struct rcuhashbash_bucket *b2)     \
{                                                               \
        struct aqm_node node ____cacheline_aligned;             \
        wl(&l, &node, _t_);                                     \
}                                                               \
                                                                \
static void l ## _write_unlock_buckets(struct rcuhashbash_bucket *b1,   \
                                       struct rcuhashbash_bucket *b2)   \
{                                                               \
        wul(&l);                                                \
}                                                               \
                                                                \
static void l ## _read_lock_bucket(struct rcuhashbash_bucket *bucket)   \
{                                                               \
        struct aqm_node node ____cacheline_aligned;             \
        rl(&l, &node, _t_);                                     \
}                                                               \
                                                                \
static void l ## _read_unlock_bucket(struct rcuhashbash_bucket *bucket) \
{                                                               \
        rul(&l);                                                \
}                                                               \

#endif /* __RCU_HT_H_ */
