# Benchmark Results

Measured with [wrk](https://github.com/wg/wrk) using `bench/run_benchmarks.sh` (`make bench`).

## Setup

| | |
|---|---|
| Machine | 2-core Linux cloud VM (wrk and the server share the same 2 cores) |
| Build | `make release` (`g++ -O2`, C++17) |
| Server | 8 worker threads, access logging **on**, rate limiting **off** |
| Load | `wrk -t2 -d15s --latency`, 15 seconds per test |

Rate limiting is turned off (`RATE_LIMIT=off`) for benchmarking. With it on, wrk
gets `429 Too Many Requests` almost immediately, so we would be measuring
rejections instead of the real request path.

## Results

| Test | Requests/sec | Median latency | p99 latency | Errors |
|---|---:|---:|---:|---:|
| `GET /health` (JSON), 100 connections, Keep-Alive | **64,175** | 0.23 ms | 150 ms | 0 |
| `GET /health` (JSON), 100 connections, **no** Keep-Alive | 33,726 | 1.60 ms | 4.6 ms | 0 |
| `GET /static/index.html` (cached), 100 connections | **60,475** | 0.25 ms | 158 ms | 0 |
| `GET /health` (JSON), **1,000** connections | **59,390** | 0.22 ms | 1.69 s | 0 |

## What the numbers show

**Keep-Alive roughly doubles throughput (1.9x).** Without it, every request
pays for a new TCP handshake (connect, accept, close) and a new task in the
thread pool. Reusing one connection for up to 100 requests removes that cost.

**The cache keeps static files almost as fast as an in-memory JSON route.**
After the first read, files are served from memory. A cached entry is
invalidated when the file's last-modified time changes, so edits show up
without a restart.

**High p99 latency is the cost of thread-per-connection.** A worker thread
stays with its connection until the client closes it, sends 100 requests, or
is idle for 5 seconds. With 8 threads, only 8 connections are served at any
moment and the rest wait in the queue. Median latency stays tiny, but the
slowest 1% of requests wait their turn: about 150 ms at 100 connections and
1.7 s at 1,000.

This is the classic [C10K problem](https://en.wikipedia.org/wiki/C10k_problem).
The fix is an event-driven design (`epoll` on Linux, `kqueue` on macOS), where
one thread watches thousands of sockets and only does work for the ones that
are ready. That is the next item on the roadmap.

## Run it yourself

```bash
# macOS: brew install wrk    Ubuntu: sudo apt install wrk
make bench
```

Numbers depend on the CPU, so a laptop with more cores will usually be faster.
Load generator and server on the same machine compete for CPU, so these are
conservative.
