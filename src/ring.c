#define _POSIX_C_SOURCE 200112L
#include "ring.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

static int is_power_of_two(size_t x) {
    return x != 0 && (x & (x - 1)) == 0;
}

int ring_init(spsc_ring_t *r, size_t capacity) {
    if (!r || !is_power_of_two(capacity)) {
        errno = EINVAL;
        return -1;
    }
    memset(r, 0, sizeof(*r));
    r->meta.capacity = capacity;
    r->meta.mask     = capacity - 1;

    /* Align the slot array to a cache line so the first slot does
     * not straddle lines with whatever came before it in memory. */
    void *mem = NULL;
    int rc = posix_memalign(&mem, RING_CACHELINE,
                            capacity * sizeof(feed_msg_t));
    if (rc != 0) {
        errno = rc;
        return -1;
    }
    r->meta.slots = (feed_msg_t *)mem;
    memset(r->meta.slots, 0, capacity * sizeof(feed_msg_t));

    atomic_store_explicit(&r->head, 0, memory_order_relaxed);
    atomic_store_explicit(&r->tail, 0, memory_order_relaxed);
    return 0;
}

void ring_destroy(spsc_ring_t *r) {
    if (!r) return;
    free(r->meta.slots);
    r->meta.slots    = NULL;
    r->meta.capacity = 0;
    r->meta.mask     = 0;
}

int ring_try_push(spsc_ring_t *r, const feed_msg_t *msg) {
    /* Producer-only field, so a relaxed load of head is fine. */
    uint64_t h = atomic_load_explicit(&r->head, memory_order_relaxed);
    /* Acquire on tail so we see the consumer's advance. */
    uint64_t t = atomic_load_explicit(&r->tail, memory_order_acquire);

    if ((h - t) >= r->meta.capacity) {
        return -1;
    }

    size_t idx = (size_t)(h & r->meta.mask);
    __builtin_prefetch(&r->meta.slots[(h + 1) & r->meta.mask], 1, 0);
    r->meta.slots[idx] = *msg;

    /* Release so the consumer's acquire sees the slot contents
     * before it sees the new head. */
    atomic_store_explicit(&r->head, h + 1, memory_order_release);
    return 0;
}

int ring_try_pop(spsc_ring_t *r, feed_msg_t *out) {
    uint64_t t = atomic_load_explicit(&r->tail, memory_order_relaxed);
    uint64_t h = atomic_load_explicit(&r->head, memory_order_acquire);

    if (t >= h) {
        return -1;
    }

    size_t idx = (size_t)(t & r->meta.mask);
    __builtin_prefetch(&r->meta.slots[(t + 1) & r->meta.mask], 0, 0);
    *out = r->meta.slots[idx];

    atomic_store_explicit(&r->tail, t + 1, memory_order_release);
    return 0;
}
