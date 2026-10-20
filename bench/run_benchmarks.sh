#!/usr/bin/env bash
# Reproduces the numbers in bench/RESULTS.md.
#   make bench
# Needs wrk:  macOS `brew install wrk`, Ubuntu `sudo apt install wrk`,
#             or build it: git clone https://github.com/wg/wrk && cd wrk && make
set -eu

WRK="${WRK:-wrk}"
PORT="${PORT:-18090}"
DURATION="${DURATION:-15s}"
BASE="http://127.0.0.1:${PORT}"

command -v "$WRK" >/dev/null || { echo "wrk not found (set WRK=/path/to/wrk)"; exit 1; }

mkdir -p logs
# Rate limiting is switched off here: with it on we would be measuring 429
# rejections, not the real request path.
PORT="$PORT" RATE_LIMIT=off ./server > logs/bench_server.log 2>&1 &
PID=$!
trap 'kill $PID 2>/dev/null; wait $PID 2>/dev/null' EXIT
sleep 1

echo "CPU cores: $(getconf _NPROCESSORS_ONLN)   server threads: 8   duration: $DURATION"

run() {   # run <label> <wrk args...>
    echo
    echo "=== $1"
    shift
    "$WRK" -t2 -d"$DURATION" --latency "$@" | grep -E "Requests/sec|Latency  |  50%|  99%|Non-2xx|Socket errors"
}

run "JSON endpoint, 100 connections, Keep-Alive"        -c100  "$BASE/health"
run "JSON endpoint, 100 connections, NO Keep-Alive"     -c100  -H "Connection: close" "$BASE/health"
run "Static file (cached), 100 connections"             -c100  "$BASE/static/index.html"
run "JSON endpoint, 1000 connections"                   -c1000 "$BASE/health"
