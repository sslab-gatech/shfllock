#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#define THRESHOLD 0x10000
#ifndef UNLOCK_COUNT_THRESHOLD
#define UNLOCK_COUNT_THRESHOLD 64
#endif

static inline uint32_t xor_random() {
    static __thread uint32_t rv = 0;

    if (rv == 0)
        rv = 20 + 1;

    uint32_t v = rv;
    v ^= v << 6;
    v ^= (uint32_t)(v) >> 21;
    v ^= v << 7;
    rv = v;

    return v & (UNLOCK_COUNT_THRESHOLD - 1);
    /* return v; //& (UNLOCK_COUNT_THRESHOLD - 1); */
}

static int keep_lock_local(void)
{
    return xor_random() & (THRESHOLD - 1);
}

int main()
{
    int count = 0;
    int total_count = 1000000000;
    int prev = 0;
    int min = THRESHOLD;
    int max = 0;
    for (int i = 0; i < total_count; ++i) {
        if (!keep_lock_local()) {
            /* printf("%d  ", count - prev); */
            if (count - prev > max)
                max = count - prev;
            if (count - prev < min)
                min = count - prev;
            prev = count;
        } else
            count++;
    }
    printf("%d %d\n", min, max);
    return 0;
}
