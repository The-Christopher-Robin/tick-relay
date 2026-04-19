#ifndef TICKRELAY_SERVER_H
#define TICKRELAY_SERVER_H

#include <stddef.h>
#include "ring.h"
#include "timing.h"

typedef struct {
    int                   port;
    int                   cpu;             /* -1 = no pinning */
    size_t                num_workers;
    spsc_ring_t          *worker_rings;    /* length num_workers */
    const timing_calib_t *calib;
    volatile int         *stop_flag;

    /* Observability counters updated by the server thread. */
    _Atomic uint64_t     *msgs_received;
    _Atomic uint64_t     *msgs_dropped;
    _Atomic uint64_t     *bad_frames;
} server_config_t;

void *server_thread_main(void *arg);

#endif /* TICKRELAY_SERVER_H */
