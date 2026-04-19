#define _GNU_SOURCE
#include "worker.h"

#include <stdatomic.h>
#include <stdio.h>
#include <time.h>

#include "affinity.h"
#include "feed.h"
#include "timing.h"

/* Short backoff when the ring is empty. Spinning hot would steal cycles
 * from the server thread. */
static void worker_backoff(int *iters) {
    if (*iters < 16) {
        for (int i = 0; i < 64; ++i) {
#if defined(__x86_64__) || defined(__i386__)
            __asm__ __volatile__("pause" ::: "memory");
#else
            __asm__ __volatile__("" ::: "memory");
#endif
        }
        (*iters)++;
        return;
    }
    struct timespec req = { .tv_sec = 0, .tv_nsec = 20000 }; /* 20 us */
    nanosleep(&req, NULL);
}

static inline void process_msg(worker_config_t *cfg, feed_msg_t *m) {
    /* "Processing" in a real pipeline is where order-book updates,
     * normalization, or a strategy would run. Here we do a cheap,
     * data-dependent transform so the compiler cannot elide it and so
     * the measurement reflects real work-on-cache-line behaviour. */
    uint64_t mix = m->seq ^ m->price_cents ^ ((uint64_t)m->qty << 32);
    m->flags ^= (uint32_t)(mix * 0x9E3779B97F4A7C15ull >> 32);

    m->egress_tsc = rdtsc_now();

    uint64_t latency_ns = 0;
    if (m->egress_tsc > m->ingress_tsc) {
        uint64_t dt = m->egress_tsc - m->ingress_tsc;
        latency_ns = timing_tsc_delta_to_ns(cfg->calib, dt);
    }
    histogram_record(cfg->hist, latency_ns);
    atomic_fetch_add_explicit(cfg->msgs_processed, 1, memory_order_relaxed);
}

void *worker_thread_main(void *arg) {
    worker_config_t *cfg = (worker_config_t *)arg;

    if (cfg->cpu >= 0) {
        int rc = affinity_set_self(cfg->cpu);
        if (rc != 0) {
            fprintf(stderr, "worker %d: affinity set to cpu %d failed (rc=%d)\n",
                    cfg->worker_id, cfg->cpu, rc);
        }
    }

    int idle_iters = 0;
    while (!*cfg->stop_flag) {
        feed_msg_t msg;
        if (ring_try_pop(cfg->ring, &msg) == 0) {
            process_msg(cfg, &msg);
            idle_iters = 0;
        } else {
            worker_backoff(&idle_iters);
        }
    }

    /* Drain anything left in the ring so we do not lose final samples. */
    feed_msg_t msg;
    while (ring_try_pop(cfg->ring, &msg) == 0) {
        process_msg(cfg, &msg);
    }
    return NULL;
}
