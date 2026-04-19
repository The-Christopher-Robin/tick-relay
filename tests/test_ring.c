#define _GNU_SOURCE
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "feed.h"
#include "ring.h"

#define T_ASSERT(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "%s:%d FAIL: %s\n", __FILE__, __LINE__, #cond); \
        exit(1); \
    } \
} while (0)

static void make_msg(feed_msg_t *m, uint64_t seq) {
    memset(m, 0, sizeof(*m));
    m->magic    = FEED_MAGIC;
    m->msg_type = FEED_MSG_TRADE;
    m->seq      = seq;
    m->symbol_id = (uint32_t)(seq * 31);
    m->price_cents = seq * 100;
    m->qty      = (uint32_t)seq;
}

static void test_push_pop_basic(void) {
    spsc_ring_t r;
    T_ASSERT(ring_init(&r, 8) == 0);
    T_ASSERT(ring_size(&r) == 0);

    feed_msg_t a;
    T_ASSERT(ring_try_pop(&r, &a) == -1);

    feed_msg_t m;
    for (uint64_t i = 0; i < 8; ++i) {
        make_msg(&m, i);
        T_ASSERT(ring_try_push(&r, &m) == 0);
    }
    T_ASSERT(ring_size(&r) == 8);

    /* full */
    make_msg(&m, 999);
    T_ASSERT(ring_try_push(&r, &m) == -1);

    for (uint64_t i = 0; i < 8; ++i) {
        T_ASSERT(ring_try_pop(&r, &a) == 0);
        T_ASSERT(a.seq == i);
        T_ASSERT(a.symbol_id == (uint32_t)(i * 31));
    }
    T_ASSERT(ring_size(&r) == 0);
    T_ASSERT(ring_try_pop(&r, &a) == -1);

    ring_destroy(&r);
}

static void test_wraparound(void) {
    spsc_ring_t r;
    T_ASSERT(ring_init(&r, 4) == 0);

    feed_msg_t m, out;
    for (uint64_t round = 0; round < 100; ++round) {
        make_msg(&m, round);
        T_ASSERT(ring_try_push(&r, &m) == 0);
        T_ASSERT(ring_try_pop(&r, &out) == 0);
        T_ASSERT(out.seq == round);
    }
    ring_destroy(&r);
}

static void test_bad_capacity(void) {
    spsc_ring_t r;
    T_ASSERT(ring_init(&r, 0) == -1);
    T_ASSERT(ring_init(&r, 3) == -1);
    T_ASSERT(ring_init(&r, 6) == -1);
}

/* SPSC stress: one thread pushes 1..N, one thread pops and checks order. */
typedef struct {
    spsc_ring_t *ring;
    uint64_t     count;
} stress_ctx_t;

static void *producer_main(void *arg) {
    stress_ctx_t *ctx = arg;
    feed_msg_t m;
    uint64_t i = 0;
    while (i < ctx->count) {
        make_msg(&m, i);
        if (ring_try_push(ctx->ring, &m) == 0) {
            i++;
        }
    }
    return NULL;
}

static void *consumer_main(void *arg) {
    stress_ctx_t *ctx = arg;
    feed_msg_t out;
    uint64_t expected = 0;
    while (expected < ctx->count) {
        if (ring_try_pop(ctx->ring, &out) == 0) {
            if (out.seq != expected) {
                fprintf(stderr, "consumer: expected %llu got %llu\n",
                        (unsigned long long)expected,
                        (unsigned long long)out.seq);
                exit(1);
            }
            expected++;
        }
    }
    return NULL;
}

static void test_spsc_stress(void) {
    spsc_ring_t r;
    T_ASSERT(ring_init(&r, 64) == 0);
    stress_ctx_t ctx = { .ring = &r, .count = 200000 };

    pthread_t p, c;
    T_ASSERT(pthread_create(&c, NULL, consumer_main, &ctx) == 0);
    T_ASSERT(pthread_create(&p, NULL, producer_main, &ctx) == 0);

    pthread_join(p, NULL);
    pthread_join(c, NULL);

    ring_destroy(&r);
}

int main(void) {
    test_push_pop_basic();
    test_wraparound();
    test_bad_capacity();
    test_spsc_stress();
    printf("test_ring: OK\n");
    return 0;
}
