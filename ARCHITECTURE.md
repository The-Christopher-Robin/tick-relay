# Architecture

```
 producer (replay.py)
        |
        |  TCP, 64-byte frames, TCP_NODELAY
        v
 +---------------------+
 |   server thread     |   epoll_wait, non-blocking recv
 |  (optionally        |   drain frames, stamp ingress_tsc
 |   pinned to CPU S)  |   dispatch by symbol_id % N
 +----------+----------+
            |
            |  per-worker SPSC lock-free rings
            v
 +----------+-----+   +----------------+          +----------------+
 | worker 0 (CPU 0)|  | worker 1 (CPU 1)|  ...   | worker N-1      |
 |  - pop frame    |  |  - pop frame    |         |                 |
 |  - touch bytes  |  |  - touch bytes  |         |                 |
 |  - stamp egress |  |  - stamp egress |         |                 |
 |  - record hist  |  |  - record hist  |         |                 |
 +-----------------+  +-----------------+         +-----------------+
            \                |                            /
             \_______________|___________________________/
                             |
                             v
              on shutdown: merge histograms and print
```

## Threads

- One **server thread** owns the listening socket, all accepted client
  sockets, and the `epoll` instance. It is the sole producer on every
  worker ring.
- **N worker threads**, each pinned to a single logical CPU. Each
  worker has exactly one ring and one histogram, so it never contends
  with anything else.

This is SPSC by construction. No mutex is taken on the hot path.

## Ring buffer

Fixed-capacity power-of-two array with two atomic `uint64_t` indices:

- `head` is written only by the server, read by both.
- `tail` is written only by the worker, read by both.

Both indices sit on their own cache line (padded by `RING_CACHELINE`
bytes) so the producer writing `head` does not invalidate the line
the consumer is holding for `tail`. Slot writes use release ordering
on `head`; slot reads use acquire on `head` to pair with it.

Slot storage is `posix_memalign`-ed to a cache line, and each slot
itself is 64 bytes (one `feed_msg_t`, one cache line), so neighbouring
slots never share a line.

## Memory ordering

```
producer:                               consumer:
    copy msg into slot[h & mask]            tail_local = load tail (relaxed)
    store head <- h+1  (release)            head_local = load head (acquire)
                                            if tail_local == head_local: empty
                                            read slot[tail_local & mask]
                                            store tail <- tail_local+1 (release)
```

The release/acquire on `head` synchronises the slot write with the
slot read. `tail` uses release/acquire in the same way so the producer
sees drained slots as free.

## Timing path

- Server thread takes `rdtsc()` right after `recv` hands bytes over
  and stores it in the frame's `ingress_tsc`.
- Worker takes `rdtsc()` right before it records the sample and
  stores it in `egress_tsc`.
- `egress_tsc - ingress_tsc` is a TSC delta. Calibration against
  `CLOCK_MONOTONIC` over a 50 ms window at startup gives a Q32
  fixed-point `ticks / ns` factor used to convert to nanoseconds.

This assumes TSC_INVARIANT, which is the norm on modern x86-64
Linux boxes. On other ISAs we fall back to the platform cycle
counter (aarch64 `cntvct_el0`) or `CLOCK_MONOTONIC`.

## Histogram

HdrHistogram-inspired but deliberately simpler. We keep an exact
1-ns bucket array for values in `[0, 100 us]` plus a single overflow
counter. This gives exact percentiles in the range we actually care
about (sub-microsecond feed handlers) with no per-record math beyond
a bounds check and an increment. Merging is a plain sum of arrays.

At summary time we walk the array once to compute p50, p90, p99,
p99.9 and print min, mean, and max. Each worker owns its own
histogram so recording is single-writer and lock-free. The main
thread merges all worker histograms after join.

## What is intentionally not here

- No kernel-bypass (DPDK, io_uring zero-copy). Sticking to portable
  `epoll` so anyone can run this on a plain Linux box.
- No TLS, no replay from pcap. Would be the next step for a real
  deployment.
- No multi-producer ring. A real feed handler could grow to MPSC
  if multiple gateways needed to push into one worker, but SPSC
  per worker is the right starting shape and is also the shape
  most low-latency stacks actually use.
