#define _GNU_SOURCE
#include <errno.h>
#include <getopt.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "affinity.h"
#include "histogram.h"
#include "ring.h"
#include "server.h"
#include "timing.h"
#include "worker.h"

#define DEFAULT_PORT         9001
#define DEFAULT_WORKERS      2
#define DEFAULT_RING_CAPACITY 65536  /* 64k slots -> 4 MiB per ring */
#define MAX_WORKERS          16

static volatile int g_stop = 0;

static void handle_signal(int sig) {
    (void)sig;
    g_stop = 1;
}

static void install_signals(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* A replay driver that dies mid-run closes the socket, which on
     * some kernels raises SIGPIPE on the next write. We do not write
     * back to clients but guard anyway. */
    signal(SIGPIPE, SIG_IGN);
}

static void usage(const char *prog) {
    fprintf(stderr,
        "usage: %s [options]\n"
        "  -p, --port N           TCP port to listen on (default %d)\n"
        "  -w, --workers N        number of worker threads (default %d, max %d)\n"
        "  -c, --ring-capacity N  ring capacity per worker, power of 2 (default %d)\n"
        "  -s, --server-cpu N     pin server thread to CPU N (default auto)\n"
        "  -W, --worker-cpu-base N pin workers to CPUs N, N+1, ... (default auto)\n"
        "  -d, --duration-ms N    run for N ms then print and exit (default: run until signal)\n"
        "  -h, --help             show this help\n",
        prog, DEFAULT_PORT, DEFAULT_WORKERS, MAX_WORKERS, DEFAULT_RING_CAPACITY);
}

static int parse_positive(const char *s, long *out) {
    char *end = NULL;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno != 0 || !end || *end != '\0' || v <= 0) return -1;
    *out = v;
    return 0;
}

int main(int argc, char **argv) {
    int  port = DEFAULT_PORT;
    int  num_workers = DEFAULT_WORKERS;
    size_t ring_capacity = DEFAULT_RING_CAPACITY;
    int  server_cpu = -1;
    int  worker_cpu_base = -1;
    long duration_ms = 0;

    static struct option opts[] = {
        {"port",             required_argument, 0, 'p'},
        {"workers",          required_argument, 0, 'w'},
        {"ring-capacity",    required_argument, 0, 'c'},
        {"server-cpu",       required_argument, 0, 's'},
        {"worker-cpu-base",  required_argument, 0, 'W'},
        {"duration-ms",      required_argument, 0, 'd'},
        {"help",             no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    long v;
    while ((opt = getopt_long(argc, argv, "p:w:c:s:W:d:h", opts, NULL)) != -1) {
        switch (opt) {
            case 'p':
                if (parse_positive(optarg, &v) != 0 || v > 65535) {
                    fprintf(stderr, "bad port: %s\n", optarg); return 2;
                }
                port = (int)v;
                break;
            case 'w':
                if (parse_positive(optarg, &v) != 0 || v > MAX_WORKERS) {
                    fprintf(stderr, "bad workers: %s\n", optarg); return 2;
                }
                num_workers = (int)v;
                break;
            case 'c':
                if (parse_positive(optarg, &v) != 0) {
                    fprintf(stderr, "bad ring-capacity: %s\n", optarg); return 2;
                }
                ring_capacity = (size_t)v;
                break;
            case 's':
                if (parse_positive(optarg, &v) != 0) {
                    fprintf(stderr, "bad server-cpu: %s\n", optarg); return 2;
                }
                server_cpu = (int)v - 1;  /* 1-based for humans */
                if (server_cpu < 0) server_cpu = 0;
                break;
            case 'W':
                if (parse_positive(optarg, &v) != 0) {
                    fprintf(stderr, "bad worker-cpu-base: %s\n", optarg); return 2;
                }
                worker_cpu_base = (int)v - 1;
                if (worker_cpu_base < 0) worker_cpu_base = 0;
                break;
            case 'd':
                if (parse_positive(optarg, &v) != 0) {
                    fprintf(stderr, "bad duration-ms: %s\n", optarg); return 2;
                }
                duration_ms = v;
                break;
            case 'h':
            default:
                usage(argv[0]);
                return (opt == 'h') ? 0 : 2;
        }
    }

    /* Validate ring capacity is a power of 2. */
    if (ring_capacity == 0 || (ring_capacity & (ring_capacity - 1)) != 0) {
        fprintf(stderr, "ring-capacity must be a power of two (got %zu)\n",
                ring_capacity);
        return 2;
    }

    /* Auto-pin: pick the last CPU for the server, the first few for workers,
     * unless the user asked for something specific. */
    int online = affinity_online_cpus();
    if (server_cpu < 0 && online > 1) {
        server_cpu = online - 1;
    }
    if (worker_cpu_base < 0 && online > 1) {
        worker_cpu_base = 0;
    }

    install_signals();

    timing_calib_t calib;
    if (timing_calibrate(&calib, 50) != 0) {
        fprintf(stderr, "timing calibration failed\n");
        return 1;
    }
    printf("calib: ticks/ns q32 = %llu (%.3f GHz)\n",
           (unsigned long long)calib.tsc_per_ns_q32,
           (double)calib.tsc_per_ns_q32 / (double)(1ull << 32));

    spsc_ring_t   rings[MAX_WORKERS];
    histogram_t   hists[MAX_WORKERS];
    worker_config_t wcfgs[MAX_WORKERS];
    pthread_t       wtids[MAX_WORKERS];

    _Atomic uint64_t msgs_received  = 0;
    _Atomic uint64_t msgs_dropped   = 0;
    _Atomic uint64_t bad_frames     = 0;
    _Atomic uint64_t msgs_processed[MAX_WORKERS];

    int initialised = 0;
    for (int i = 0; i < num_workers; ++i) {
        if (ring_init(&rings[i], ring_capacity) != 0) {
            fprintf(stderr, "ring_init failed: %s\n", strerror(errno));
            goto cleanup;
        }
        if (histogram_init(&hists[i]) != 0) {
            fprintf(stderr, "histogram_init failed\n");
            ring_destroy(&rings[i]);
            goto cleanup;
        }
        atomic_init(&msgs_processed[i], 0);
        initialised = i + 1;
    }

    for (int i = 0; i < num_workers; ++i) {
        wcfgs[i].worker_id      = i;
        wcfgs[i].cpu            = (worker_cpu_base >= 0)
                                  ? (worker_cpu_base + i) % online
                                  : -1;
        wcfgs[i].ring           = &rings[i];
        wcfgs[i].hist           = &hists[i];
        wcfgs[i].calib          = &calib;
        wcfgs[i].stop_flag      = &g_stop;
        wcfgs[i].msgs_processed = &msgs_processed[i];

        int rc = pthread_create(&wtids[i], NULL, worker_thread_main, &wcfgs[i]);
        if (rc != 0) {
            fprintf(stderr, "worker %d: pthread_create failed: %s\n",
                    i, strerror(rc));
            g_stop = 1;
            for (int j = 0; j < i; ++j) pthread_join(wtids[j], NULL);
            goto cleanup;
        }
    }

    server_config_t scfg;
    scfg.port          = port;
    scfg.cpu           = server_cpu;
    scfg.num_workers   = (size_t)num_workers;
    scfg.worker_rings  = rings;
    scfg.calib         = &calib;
    scfg.stop_flag     = &g_stop;
    scfg.msgs_received = &msgs_received;
    scfg.msgs_dropped  = &msgs_dropped;
    scfg.bad_frames    = &bad_frames;

    pthread_t stid;
    int rc = pthread_create(&stid, NULL, server_thread_main, &scfg);
    if (rc != 0) {
        fprintf(stderr, "server: pthread_create failed: %s\n", strerror(rc));
        g_stop = 1;
        for (int i = 0; i < num_workers; ++i) pthread_join(wtids[i], NULL);
        goto cleanup;
    }

    if (duration_ms > 0) {
        uint64_t start = timing_now_ns();
        uint64_t budget_ns = (uint64_t)duration_ms * 1000000ull;
        while (!g_stop && (timing_now_ns() - start) < budget_ns) {
            struct timespec ts = { .tv_sec = 0, .tv_nsec = 50 * 1000000L };
            nanosleep(&ts, NULL);
        }
        g_stop = 1;
    } else {
        while (!g_stop) {
            struct timespec ts = { .tv_sec = 0, .tv_nsec = 200 * 1000000L };
            nanosleep(&ts, NULL);
        }
    }

    pthread_join(stid, NULL);
    for (int i = 0; i < num_workers; ++i) pthread_join(wtids[i], NULL);

    histogram_t combined;
    if (histogram_init(&combined) == 0) {
        for (int i = 0; i < num_workers; ++i) {
            histogram_merge(&combined, &hists[i]);
        }
        puts("");
        histogram_print_summary(&combined, "tick-to-response");
        for (int i = 0; i < num_workers; ++i) {
            char label[32];
            snprintf(label, sizeof(label), "worker[%d]", i);
            histogram_print_summary(&hists[i], label);
        }
        histogram_destroy(&combined);
    }

    uint64_t rx  = atomic_load(&msgs_received);
    uint64_t drp = atomic_load(&msgs_dropped);
    uint64_t bad = atomic_load(&bad_frames);
    uint64_t tot_proc = 0;
    for (int i = 0; i < num_workers; ++i) tot_proc += atomic_load(&msgs_processed[i]);
    printf("counters: received=%llu processed=%llu dropped=%llu bad=%llu\n",
           (unsigned long long)rx, (unsigned long long)tot_proc,
           (unsigned long long)drp, (unsigned long long)bad);

cleanup:
    for (int i = 0; i < initialised; ++i) {
        histogram_destroy(&hists[i]);
        ring_destroy(&rings[i]);
    }
    return 0;
}
