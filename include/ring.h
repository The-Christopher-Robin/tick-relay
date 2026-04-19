#ifndef TICKRELAY_RING_H
#define TICKRELAY_RING_H

#include <stdatomic.h>
#include <stdint.h>
#include <stddef.h>

#include "feed.h"

/*
 * Single-producer single-consumer lock-free ring. One ring per worker.
 *
 * Layout notes:
 *   - Capacity must be a power of two so we can use head & mask instead
 *     of a divide.
 *   - head and tail each sit on their own cache line to avoid false
 *     sharing between the producer and the consumer.
 *   - slots[] is allocated cache-aligned. Each slot is a full feed_msg_t
 *     (64 bytes, one cache line), so slot writes do not straddle lines.
 */

#define RING_CACHELINE 64

struct spsc_ring_meta {
    size_t      capacity;
    size_t      mask;
    feed_msg_t *slots;
};

typedef struct {
    struct spsc_ring_meta meta;
    char _pad0[RING_CACHELINE];

    _Atomic uint64_t head;   /* only the producer writes */
    char _pad1[RING_CACHELINE - sizeof(_Atomic uint64_t)];

    _Atomic uint64_t tail;   /* only the consumer writes */
    char _pad2[RING_CACHELINE - sizeof(_Atomic uint64_t)];
} spsc_ring_t;

int  ring_init(spsc_ring_t *r, size_t capacity);
void ring_destroy(spsc_ring_t *r);

/* Returns 0 on success, -1 if the ring is full. */
int ring_try_push(spsc_ring_t *r, const feed_msg_t *msg);

/* Returns 0 on success, -1 if the ring is empty. */
int ring_try_pop(spsc_ring_t *r, feed_msg_t *out);

static inline size_t ring_size(const spsc_ring_t *r) {
    uint64_t h = atomic_load_explicit(&r->head, memory_order_acquire);
    uint64_t t = atomic_load_explicit(&r->tail, memory_order_acquire);
    return (size_t)(h - t);
}

#endif /* TICKRELAY_RING_H */
