#ifndef TICKRELAY_TIMING_H
#define TICKRELAY_TIMING_H

#include <stdint.h>

/* Wall-clock ns from CLOCK_MONOTONIC. Used for calibration and logging. */
uint64_t timing_now_ns(void);

/* rdtsc / equivalent cycle counter. Assumes TSC_INVARIANT on x86-64,
 * which is the norm on any Linux machine we actually care about. */
static inline uint64_t rdtsc_now(void) {
#if defined(__x86_64__) || defined(__i386__)
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | (uint64_t)lo;
#elif defined(__aarch64__)
    uint64_t v;
    __asm__ __volatile__("mrs %0, cntvct_el0" : "=r"(v));
    return v;
#else
    return timing_now_ns();
#endif
}

typedef struct {
    uint64_t tsc_per_ns_q32;  /* ticks-per-ns in Q32 fixed-point */
    uint64_t base_tsc;
    uint64_t base_ns;
} timing_calib_t;

/* Samples TSC against CLOCK_MONOTONIC for roughly sample_ms milliseconds.
 * Returns 0 on success, -1 on error. 20-100 ms is a sensible sample window. */
int timing_calibrate(timing_calib_t *c, uint32_t sample_ms);

/* Converts a TSC reading into ns using the calibration. Signed delta
 * relative to base_tsc so values before calibration still make sense. */
uint64_t timing_tsc_to_ns(const timing_calib_t *c, uint64_t tsc);

/* Converts a TSC *delta* (a duration, not a point-in-time) to ns. */
uint64_t timing_tsc_delta_to_ns(const timing_calib_t *c, uint64_t tsc_delta);

#endif /* TICKRELAY_TIMING_H */
