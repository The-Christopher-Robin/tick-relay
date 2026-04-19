#include "histogram.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const size_t HIST_SLOTS = (size_t)HIST_MAX_NS + 1;

int histogram_init(histogram_t *h) {
    if (!h) return -1;
    h->counts = (uint64_t *)calloc(HIST_SLOTS, sizeof(uint64_t));
    if (!h->counts) return -1;
    h->overflow_count = 0;
    h->total_count    = 0;
    h->sum_ns         = 0;
    h->min_ns         = UINT64_MAX;
    h->max_ns         = 0;
    return 0;
}

void histogram_destroy(histogram_t *h) {
    if (!h) return;
    free(h->counts);
    h->counts = NULL;
}

void histogram_record(histogram_t *h, uint64_t value_ns) {
    if (!h || !h->counts) return;
    if (value_ns > (uint64_t)HIST_MAX_NS) {
        h->overflow_count++;
    } else {
        h->counts[value_ns]++;
    }
    h->total_count++;
    h->sum_ns += value_ns;
    if (value_ns < h->min_ns) h->min_ns = value_ns;
    if (value_ns > h->max_ns) h->max_ns = value_ns;
}

uint64_t histogram_percentile(const histogram_t *h, double pct) {
    if (!h || h->total_count == 0) return 0;
    if (pct < 0.0)   pct = 0.0;
    if (pct > 100.0) pct = 100.0;

    /* Round up so p99 of a small sample behaves like "at least 99%". */
    uint64_t target = (uint64_t)((pct / 100.0) * (double)h->total_count + 0.5);
    if (target == 0) target = 1;
    if (target > h->total_count) target = h->total_count;

    uint64_t cum = 0;
    for (size_t i = 0; i < HIST_SLOTS; ++i) {
        cum += h->counts[i];
        if (cum >= target) {
            return (uint64_t)i;
        }
    }
    /* Fell through - all in overflow. Best we can do is report max. */
    return h->max_ns;
}

void histogram_merge(histogram_t *dst, const histogram_t *src) {
    if (!dst || !src || !dst->counts || !src->counts) return;
    for (size_t i = 0; i < HIST_SLOTS; ++i) {
        dst->counts[i] += src->counts[i];
    }
    dst->overflow_count += src->overflow_count;
    dst->total_count    += src->total_count;
    dst->sum_ns         += src->sum_ns;
    if (src->min_ns < dst->min_ns) dst->min_ns = src->min_ns;
    if (src->max_ns > dst->max_ns) dst->max_ns = src->max_ns;
}

void histogram_print_summary(const histogram_t *h, const char *label) {
    if (!h) return;
    if (h->total_count == 0) {
        printf("[%s] no samples\n", label ? label : "hist");
        return;
    }
    double mean_ns = (double)h->sum_ns / (double)h->total_count;
    uint64_t p50  = histogram_percentile(h, 50.0);
    uint64_t p90  = histogram_percentile(h, 90.0);
    uint64_t p99  = histogram_percentile(h, 99.0);
    uint64_t p999 = histogram_percentile(h, 99.9);

    printf("[%s] n=%llu  min=%llu ns  mean=%.1f ns  "
           "p50=%llu  p90=%llu  p99=%llu  p99.9=%llu  max=%llu  overflow=%llu\n",
           label ? label : "hist",
           (unsigned long long)h->total_count,
           (unsigned long long)h->min_ns,
           mean_ns,
           (unsigned long long)p50,
           (unsigned long long)p90,
           (unsigned long long)p99,
           (unsigned long long)p999,
           (unsigned long long)h->max_ns,
           (unsigned long long)h->overflow_count);
}
