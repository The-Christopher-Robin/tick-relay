#ifndef TICKRELAY_HISTOGRAM_H
#define TICKRELAY_HISTOGRAM_H

#include <stdint.h>
#include <stddef.h>

/*
 * HdrHistogram-inspired fixed-resolution latency histogram.
 *
 * We care about latencies in the 0..100 microsecond range, so an exact
 * 1-ns bucket array is cheap (100k entries * 8 bytes = 800 kB) and gives
 * us exact percentiles without the sub-bucket math a real HdrHistogram
 * would need. Values above HIST_MAX_NS fall into a single overflow
 * counter and are surfaced in the summary.
 */
#define HIST_MAX_NS 100000  /* 100 us */

typedef struct {
    uint64_t *counts;         /* length HIST_MAX_NS + 1 */
    uint64_t  overflow_count;
    uint64_t  total_count;
    uint64_t  sum_ns;
    uint64_t  min_ns;
    uint64_t  max_ns;
} histogram_t;

int  histogram_init(histogram_t *h);
void histogram_destroy(histogram_t *h);

void histogram_record(histogram_t *h, uint64_t value_ns);

/* pct is in [0, 100]. Returns the ns value at that percentile. */
uint64_t histogram_percentile(const histogram_t *h, double pct);

/* Adds src into dst. Both histograms must be initialised. */
void histogram_merge(histogram_t *dst, const histogram_t *src);

/* Writes a short human-readable summary to stdout. */
void histogram_print_summary(const histogram_t *h, const char *label);

#endif /* TICKRELAY_HISTOGRAM_H */
