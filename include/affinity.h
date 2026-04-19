#ifndef TICKRELAY_AFFINITY_H
#define TICKRELAY_AFFINITY_H

#include <pthread.h>

/* Pin the calling thread (or a specific thread) to a single logical CPU.
 * Returns 0 on success, an errno-like value on failure, as pthread does. */
int affinity_set_self(int cpu);
int affinity_set_thread(pthread_t th, int cpu);

/* Best-effort: returns the number of online CPUs, or 1 on error. */
int affinity_online_cpus(void);

#endif /* TICKRELAY_AFFINITY_H */
