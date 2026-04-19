#define _POSIX_C_SOURCE 200809L
#include "timing.h"

#include <errno.h>
#include <time.h>

uint64_t timing_now_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

int timing_calibrate(timing_calib_t *c, uint32_t sample_ms) {
    if (!c || sample_ms == 0) {
        errno = EINVAL;
        return -1;
    }

    uint64_t ns0 = timing_now_ns();
    uint64_t t0  = rdtsc_now();

    struct timespec req;
    req.tv_sec  = sample_ms / 1000u;
    req.tv_nsec = (long)(sample_ms % 1000u) * 1000000L;
    while (nanosleep(&req, &req) == -1 && errno == EINTR) {
        /* finish the sleep if we were interrupted */
    }

    uint64_t ns1 = timing_now_ns();
    uint64_t t1  = rdtsc_now();

    uint64_t dns = ns1 - ns0;
    uint64_t dt  = t1 - t0;
    if (dns == 0 || dt == 0) {
        return -1;
    }

    /* ticks per ns in Q32 fixed-point. (dt << 32) / dns. */
    __uint128_t num = ((__uint128_t)dt) << 32;
    c->tsc_per_ns_q32 = (uint64_t)(num / dns);
    c->base_tsc = t1;
    c->base_ns  = ns1;
    return 0;
}

uint64_t timing_tsc_delta_to_ns(const timing_calib_t *c, uint64_t tsc_delta) {
    if (!c || c->tsc_per_ns_q32 == 0) return 0;
    __uint128_t num = ((__uint128_t)tsc_delta) << 32;
    return (uint64_t)(num / c->tsc_per_ns_q32);
}

uint64_t timing_tsc_to_ns(const timing_calib_t *c, uint64_t tsc) {
    if (!c) return 0;
    if (tsc >= c->base_tsc) {
        uint64_t d = tsc - c->base_tsc;
        return c->base_ns + timing_tsc_delta_to_ns(c, d);
    }
    /* tsc is before calibration - rare, happens for ingress timestamps
     * taken during startup. Clamp rather than return garbage. */
    uint64_t d  = c->base_tsc - tsc;
    uint64_t dn = timing_tsc_delta_to_ns(c, d);
    return (c->base_ns > dn) ? (c->base_ns - dn) : 0;
}
