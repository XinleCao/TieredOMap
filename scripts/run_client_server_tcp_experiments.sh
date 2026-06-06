#!/bin/bash
# Client/server TCP runner for the revised static experiments.
#
# This script starts a local ORAM storage server, runs the benchmark through
# the TCP path, and stops the server after each experiment.

set -euo pipefail

MAX_LOGN=${MAX_LOGN:-${1:-20}}
Q=${Q:-${2:-200}}
PORT=${PORT:-${3:-12410}}
OUTDIR=${OUTDIR:-${4:-"results_server/client_server_tcp_$(date +%Y%m%d_%H%M%S)"}}
WARMUP=${WARMUP:-50}
DYN_OBS=${DYN_OBS:-256}
DYN_SWAP=${DYN_SWAP:-32}
DYN_CACHE=${DYN_CACHE:-8}
TIMEOUT_SEC=${TIMEOUT_SEC:-7200}

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$PROJECT_DIR/build"

mkdir -p "$BUILD_DIR" "$OUTDIR"

cd "$BUILD_DIR"
cmake .. -DCMAKE_BUILD_TYPE=Release > /dev/null
JOBS=$(nproc 2>/dev/null || sysctl -n hw.ncpu)
cmake --build . --target bench_paper oram_server -j"$JOBS"
cd "$PROJECT_DIR"

SERVER_PID=""

stop_server() {
    if [ -n "${SERVER_PID:-}" ] && kill -0 "$SERVER_PID" 2>/dev/null; then
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
    SERVER_PID=""
}

wait_for_server() {
    for _ in $(seq 1 50); do
        if (echo > "/dev/tcp/127.0.0.1/$PORT") >/dev/null 2>&1; then
            return 0
        fi
        sleep 0.1
    done
    return 1
}

start_server() {
    local log_file="$1"
    stop_server
    "$BUILD_DIR/oram_server" --port="$PORT" > "$log_file" 2>&1 &
    SERVER_PID=$!
    if ! wait_for_server; then
        echo "Failed to start ORAM server on port $PORT" >&2
        cat "$log_file" >&2 || true
        exit 1
    fi
}

run_bench() {
    local exp="$1"
    local subdir="$2"
    shift 2

    mkdir -p "$OUTDIR/$subdir"
    start_server "$OUTDIR/${subdir}_server.log"

    local cmd=("$BUILD_DIR/bench_paper" "--exp=$exp" "--Q=$Q"
        "--warmup=$WARMUP" "--max_logN=$MAX_LOGN"
        "--outdir=$OUTDIR/$subdir" "--host=127.0.0.1" "--port=$PORT" "$@")

    local time_cmd=(/usr/bin/time -p)
    if /usr/bin/time -v true >/dev/null 2>&1; then
        time_cmd=(/usr/bin/time -v)
    fi

    if command -v timeout >/dev/null 2>&1; then
        "${time_cmd[@]}" timeout "$TIMEOUT_SEC" "${cmd[@]}"
    else
        "${time_cmd[@]}" "${cmd[@]}"
    fi

    stop_server
}

trap stop_server EXIT

echo "============================================"
echo " Client/server TCP experiments"
echo " max_logN=$MAX_LOGN Q=$Q warmup=$WARMUP port=$PORT"
echo " output=$OUTDIR"
echo "============================================"

run_bench client_fo client_fo_tcp
run_bench client_dynamic_bw client_dynamic_tcp \
    "--dyn_obs=$DYN_OBS" "--dyn_swap=$DYN_SWAP" "--dyn_cache=$DYN_CACHE"

echo ""
echo "Done. CSV files:"
find "$OUTDIR" -maxdepth 3 -name '*.csv' -print
