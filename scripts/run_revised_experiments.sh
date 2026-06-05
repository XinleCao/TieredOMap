#!/bin/bash
# Revised experiment runner:
#   - client/server mainline uses FO only
#   - client/server dynamic extension measures FO bandwidth overhead
#   - TEE mainline compares FO against BatchTM

set -e

MAX_LOGN=${1:-20}
Q=${2:-200}
HOST=${3:-""}
PORT=${4:-12345}
OUTDIR=${5:-"results_revised_$(date +%Y%m%d_%H%M%S)"}

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$PROJECT_DIR/build"

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"
cmake .. -DCMAKE_BUILD_TYPE=Release > /dev/null
make -j$(nproc 2>/dev/null || sysctl -n hw.ncpu) bench_paper bench_tee_batch
cd "$PROJECT_DIR"

NET_ARGS=""
if [ -n "$HOST" ]; then
    NET_ARGS="--host=$HOST --port=$PORT"
fi

mkdir -p "$OUTDIR"

echo "============================================"
echo " Revised TieredOMap Experiments"
echo " max_logN=$MAX_LOGN  Q=$Q"
echo " output=$OUTDIR"
echo "============================================"

echo ""
echo "[1/3] Client/server mainline: FO only"
"$BUILD_DIR/bench_paper" --exp=client_fo --Q=$Q --max_logN=$MAX_LOGN \
    --outdir="$OUTDIR/client_server" $NET_ARGS

echo ""
echo "[2/3] Client/server dynamic: FO bandwidth overhead"
"$BUILD_DIR/bench_paper" --exp=client_dynamic_bw --Q=$Q --max_logN=$MAX_LOGN \
    --dyn_obs=256 --dyn_swap=32 --dyn_cache=8 \
    --outdir="$OUTDIR/client_server_dynamic" $NET_ARGS

echo ""
echo "[3/3] TEE mainline: FO vs BatchTM"
"$BUILD_DIR/bench_tee_batch" --min_logN 14 --max_logN "$MAX_LOGN" \
    --Q "$Q" --betas 10,32,100,300,1000 \
    --outdir "$OUTDIR/tee_batch"

echo ""
echo "Done. CSV files:"
find "$OUTDIR" -name '*.csv' -maxdepth 3 -print
