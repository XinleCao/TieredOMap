#!/usr/bin/env bash
# Run Table 2 client/server FO rows one configuration at a time.
# Intended for the storage server host: start oram_server separately, then run
# this client through TCP localhost while modeling the measured WAN RTT.

set -euo pipefail

Q=${Q:-1024}
WARMUP=${WARMUP:-0}
VALUE_SIZE=${VALUE_SIZE:-4096}
RTT_MS=${RTT_MS:-28.87}
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
echo "  Q=$Q warmup=$WARMUP value_size=$VALUE_SIZE rtt_ms=$RTT_MS"
echo "  recv_timeout=$RECV_TIMEOUT"
echo "  host=$HOST port=$PORT"
echo "  backends=$BACKENDS"
echo "  logNs=$LOGNS"
echo "  outdir=$OUTDIR"

for backend in $BACKENDS; do
    for logN in $LOGNS; do
        echo ""
        echo "==== $(date '+%F %T') backend=$backend logN=$logN ===="
        free -h || true
        df -h / || true
        "$BUILD_DIR/bench_paper" \
            --exp=client_fo \
            --Q="$Q" \
            --warmup="$WARMUP" \
            --logNs="$logN" \
            --backend="$backend" \
            --value_size="$VALUE_SIZE" \
            --client_fo_standalone=0 \
            --rtt_ms="$RTT_MS" \
            --recv_timeout="$RECV_TIMEOUT" \
            --outdir="$OUTDIR" \
            --host="$HOST" \
            --port="$PORT"
        echo "==== $(date '+%F %T') done backend=$backend logN=$logN ===="
    done
done

echo ""
echo "Done. CSV:"
find "$OUTDIR" -maxdepth 1 -name '*.csv' -print
