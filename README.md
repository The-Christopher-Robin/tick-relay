# tick-relay

A low-latency multi-core TCP market-feed processor written in C, built
as a focused playground for the kind of work a feed handler does:
read fixed-size framed messages off a non-blocking socket, fan them out
to CPU-pinned workers through lock-free SPSC ring buffers, and keep the
whole pipeline measurable at nanosecond resolution.

## What it does

- Listens on a TCP port, accepts one or more feed producers.
- Uses `epoll` with non-blocking sockets to drain bytes as they arrive.
- Frames a continuous byte stream into fixed 64-byte feed messages.
- Dispatches each message to a worker ring by `symbol_id % num_workers`,
  so all updates for a given symbol stay on a single worker.
- Workers run pinned to specific CPU cores, touch the message, record
  tick-to-response latency with `rdtsc`, and write it into a per-worker
  HdrHistogram-inspired latency histogram.
- On shutdown (SIGINT, SIGTERM, or `--duration-ms`), the server merges
  worker histograms and prints min, mean, p50, p90, p99, p99.9, and max.

## Build

Linux only (uses `epoll`, `pthread_setaffinity_np`, `clock_gettime`).

```bash
make            # debug-ish build: -O2 + warnings
make release    # -O3 -march=native -flto -DNDEBUG
make test       # unit tests for the ring buffer and histogram
make asan       # AddressSanitizer + UBSan build + tests
```

A compile_commands.json is not generated here; if you want IDE
integration run `bear -- make` or equivalent.

## Run

Start the server:

```bash
./tick-relay --port 9001 --workers 2
```

In another shell, drive it with the Python replay driver:

```bash
python3 tools/replay.py --host 127.0.0.1 --port 9001 --messages 500000
```

Or run the end-to-end benchmark wrapper:

```bash
make release
./scripts/run_bench.sh        # defaults: 500k messages, 2 workers
```

Server options:

| flag                    | meaning                                     |
|-------------------------|---------------------------------------------|
| `-p, --port`            | TCP port (default 9001)                     |
| `-w, --workers`         | worker thread count (default 2, max 16)     |
| `-c, --ring-capacity`   | SPSC ring capacity, power of two (default 65536) |
| `-s, --server-cpu`      | 1-based CPU index to pin the server thread  |
| `-W, --worker-cpu-base` | 1-based base CPU for workers (they pin to base, base+1, ...) |
| `-d, --duration-ms`     | run for N ms then print summary and exit    |

## Architecture

See [ARCHITECTURE.md](ARCHITECTURE.md) for the data-flow picture and the
rationale behind the main design choices (ring layout, memory ordering,
why rdtsc, how the histogram is bucketed).

## Repo layout

```
include/   public headers, one per component
src/       feed framing, ring buffer, server, workers, histogram, timing
tests/     unit tests for the ring buffer and histogram
tools/     Python replay driver (binary-compatible with include/feed.h)
scripts/   bench and sanitizer wrappers
```

## Notes

- Numbers in the summary are wall time from ingress-on-server to
  egress-from-worker, not a network-level tick-to-trade figure.
  Producer-side timestamps are written but not used for the headline
  latency because client and server clocks are not disciplined.
- The histogram uses exact 1-ns buckets up to 100 microseconds and a
  single overflow counter above that. Good enough for the latency
  range we care about, and simple enough to explain.
- Known limits: a single-process demo, no TLS, no auth, no
  replay-from-capture. Easy to add, not the point here.
