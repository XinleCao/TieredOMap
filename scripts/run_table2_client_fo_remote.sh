#!/usr/bin/env bash
# Run Table 2 client/server FO rows one configuration at a time.
# Intended for the client host: start oram_server separately on the storage
# server, then run this client over TCP. By default response_ms is measured
# wall-clock time. Set RTT_MS explicitly only for a modeled round-count run.

set -euo pipefail

Q=${Q:-1024}
WARMUP=${WARMUP:-0}
VALUE_SIZE=${VALUE_SIZE:-4096}
RTT_MS=${RTT_MS:-}
RECV_TIMEOUT=${RECV_TIMEOUT:-7200}
HOST=${HOST:-127.0.0.1}
PORT=${PORT:-12345}
BACKENDS=${BACKENDS:-"AVL BPlus DaBplus"}
LOGNS=${LOGNS:-"16 20 24"}
OUTDIR=${OUTDIR:-"results/table2_client_fo_$(date +%Y%m%d_%H%M%S)"}

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$PROJECT_DIR/build"

mkdir -p "$OUTDIR"

echo "Table 2 client/server FO run"
if [[ -n "$RTT_MS" ]]; then
    echo "  Q=$Q warmup=$WARMUP value_size=$VALUE_SIZE response=modeled rtt_ms=$RTT_MS"
else
    echo "  Q=$Q warmup=$WARMUP value_size=$VALUE_SIZE response=measured_wall_clock"
fi
echo "  recv_timeout=$RECV_TIMEOUT"
echo "  host=$HOST port=$PORT"
echo "  backends=$BACKENDS"
echo "  logNs=$LOGNS"
echo "  outdir=$OUTDIR"

for backend in $BACKENDS; do
    for logN in $LOGNS; do
        echo ""
        echo "==== $(date '+%F %T') backend=$backend logN=$logN ===="
        if command -v free >/dev/null 2>&1; then free -h; fi
        df -h / || true
        cmd=("$BUILD_DIR/bench_paper" \
            --exp=client_fo \
            --Q="$Q" \
            --warmup="$WARMUP" \
            --logNs="$logN" \
            --backend="$backend" \
            --value_size="$VALUE_SIZE" \
            --client_fo_standalone=0 \
            --recv_timeout="$RECV_TIMEOUT" \
            --outdir="$OUTDIR" \
            --host="$HOST" \
            --port="$PORT")
        if [[ -n "$RTT_MS" ]]; then
            cmd+=(--rtt_ms="$RTT_MS")
        fi
        "${cmd[@]}"
        echo "==== $(date '+%F %T') done backend=$backend logN=$logN ===="
    done
done

echo ""
echo "Done. CSV:"
find "$OUTDIR" -maxdepth 1 -name '*.csv' -print
