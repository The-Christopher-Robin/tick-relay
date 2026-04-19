#ifndef TICKRELAY_WORKER_H
#define TICKRELAY_WORKER_H

#include "ring.h"
#include "histogram.h"
#include "timing.h"

typedef struct {
    int                   worker_id;
    int                   cpu;          /* -1 = no pinning */
    spsc_ring_t          *ring;
    histogram_t          *hist;
    const timing_calib_t *calib;
    volatile int         *stop_flag;

    _Atomic uint64_t     *msgs_processed;
} worker_config_t;

void *worker_thread_main(void *arg);

#endif /* TICKRELAY_WORKER_H */
