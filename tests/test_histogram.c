#include <stdio.h>
#include <stdlib.h>

#include "histogram.h"

#define T_ASSERT(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "%s:%d FAIL: %s\n", __FILE__, __LINE__, #cond); \
        exit(1); \
    } \
} while (0)

static void test_empty(void) {
    histogram_t h;
    T_ASSERT(histogram_init(&h) == 0);
    T_ASSERT(h.total_count == 0);
    T_ASSERT(histogram_percentile(&h, 50.0) == 0);
    histogram_destroy(&h);
}

static void test_simple_percentile(void) {
    histogram_t h;
    T_ASSERT(histogram_init(&h) == 0);

    /* 100 samples uniformly spread from 1 to 100 ns. */
    for (uint64_t v = 1; v <= 100; ++v) {
        histogram_record(&h, v);
    }
    T_ASSERT(h.total_count == 100);
    T_ASSERT(h.min_ns == 1);
    T_ASSERT(h.max_ns == 100);

    uint64_t p50 = histogram_percentile(&h, 50.0);
    uint64_t p99 = histogram_percentile(&h, 99.0);

    /* With 100 samples, p50 should land around 50, p99 around 99. */
    T_ASSERT(p50 >= 49 && p50 <= 51);
    T_ASSERT(p99 >= 98 && p99 <= 100);
    histogram_destroy(&h);
}

static void test_overflow(void) {
    histogram_t h;
    T_ASSERT(histogram_init(&h) == 0);
    histogram_record(&h, 500);
    histogram_record(&h, (uint64_t)HIST_MAX_NS + 1000);
    T_ASSERT(h.total_count == 2);
    T_ASSERT(h.overflow_count == 1);
    histogram_destroy(&h);
}

static void test_merge(void) {
    histogram_t a, b;
    T_ASSERT(histogram_init(&a) == 0);
    T_ASSERT(histogram_init(&b) == 0);

    for (uint64_t v = 1; v <= 50; ++v)   histogram_record(&a, v);
    for (uint64_t v = 51; v <= 100; ++v) histogram_record(&b, v);

    histogram_merge(&a, &b);
    T_ASSERT(a.total_count == 100);
    T_ASSERT(a.min_ns == 1);
    T_ASSERT(a.max_ns == 100);

    uint64_t p50 = histogram_percentile(&a, 50.0);
    T_ASSERT(p50 >= 49 && p50 <= 51);

    histogram_destroy(&a);
    histogram_destroy(&b);
}

int main(void) {
    test_empty();
    test_simple_percentile();
    test_overflow();
    test_merge();
    printf("test_histogram: OK\n");
    return 0;
}
