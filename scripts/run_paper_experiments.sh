#!/bin/bash
# run_paper_experiments.sh — Run all paper experiments.
#
# Local mode (storage in same process):
#   bash scripts/run_paper_experiments.sh [max_logN] [Q]
#
# TCP mode (client/server, real network latency):
#   1. On server machine:  bash scripts/run_server.sh [port]
#   2. On client machine:  bash scripts/run_paper_experiments.sh [max_logN] [Q] [host] [port]
#
# Examples:
#   bash scripts/run_paper_experiments.sh                       # local, logN≤20, Q=200
#   bash scripts/run_paper_experiments.sh 22 500                # local, larger scale
#   bash scripts/run_paper_experiments.sh 20 200 10.0.0.1       # TCP, default port 12345
#   bash scripts/run_paper_experiments.sh 20 200 10.0.0.1 9999  # TCP, custom port

set -e

MAX_LOGN=${1:-20}
Q=${2:-200}
HOST=${3:-""}
PORT=${4:-12345}
OUTDIR="results_$(date +%Y%m%d_%H%M%S)"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$PROJECT_DIR/build"

# Build
echo ""
echo "[0/9] Building (Release)..."
mkdir -p "$BUILD_DIR" && cd "$BUILD_DIR"
cmake .. -DCMAKE_BUILD_TYPE=Release > /dev/null 2>&1
make -j$(nproc 2>/dev/null || sysctl -n hw.ncpu) bench_paper > /dev/null 2>&1
cd "$PROJECT_DIR"
echo "  Build OK."

BENCH="$BUILD_DIR/bench_paper"

NET_ARGS=""
MODE_LABEL="Local"
if [ -n "$HOST" ]; then
    NET_ARGS="--host=$HOST --port=$PORT"
    MODE_LABEL="TCP ($HOST:$PORT)"
fi

echo "============================================"
echo " TieredOMap Paper Experiments"
echo " Mode: $MODE_LABEL"
echo " max_logN=$MAX_LOGN  Q=$Q"
echo " Output: $OUTDIR/"
echo "============================================"

echo ""
echo "[1/9] Exp: Backend comparison (standalone OMAP)..."
$BENCH --exp=backend_cmp --Q=$Q --max_logN=$MAX_LOGN --outdir=$OUTDIR $NET_ARGS

echo ""
echo "[2/9] Exp: Bandwidth & rounds vs N..."
$BENCH --exp=bandwidth --Q=$Q --max_logN=$MAX_LOGN --outdir=$OUTDIR $NET_ARGS

echo ""
echo "[3/9] Exp: Skewness effect..."
$BENCH --exp=skewness --Q=$Q --outdir=$OUTDIR $NET_ARGS

echo ""
echo "[4/9] Exp: Hot-set size effect..."
$BENCH --exp=hotsize --Q=$Q --outdir=$OUTDIR $NET_ARGS

echo ""
echo "[5/9] Exp: Latency (analytical rounds × RTT)..."
$BENCH --exp=latency --Q=$Q --outdir=$OUTDIR $NET_ARGS

echo ""
echo "[6/9] Exp: Security mode comparison (FO vs TM vs TM+Split)..."
$BENCH --exp=modes --Q=$Q --max_logN=$MAX_LOGN --outdir=$OUTDIR $NET_ARGS

echo ""
echo "[7/9] Exp: Write overhead (search vs update vs insert)..."
$BENCH --exp=write --Q=$Q --outdir=$OUTDIR $NET_ARGS

echo ""
echo "[8/9] Exp: Dynamic hot-set maintenance..."
$BENCH --exp=dynamic --outdir=$OUTDIR $NET_ARGS

echo ""
echo "[9/9] Exp: Workload drift..."
$BENCH --exp=drift --outdir=$OUTDIR $NET_ARGS

echo ""
echo "============================================"
echo " ALL DONE"
echo " CSV files in: $OUTDIR/"
echo "============================================"
echo ""
echo "Generated CSV files:"
ls -lh $OUTDIR/*.csv
