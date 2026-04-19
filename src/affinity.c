#define _GNU_SOURCE
#include "affinity.h"

#include <errno.h>
#include <sched.h>
#include <unistd.h>

int affinity_set_self(int cpu) {
    if (cpu < 0) return 0;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    return pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
}

int affinity_set_thread(pthread_t th, int cpu) {
    if (cpu < 0) return 0;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    return pthread_setaffinity_np(th, sizeof(set), &set);
}

int affinity_online_cpus(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n <= 0) return 1;
    return (int)n;
}
