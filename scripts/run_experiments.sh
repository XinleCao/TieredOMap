#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$PROJECT_DIR/build"
RESULTS_DIR="$PROJECT_DIR/results"
BENCH="$BUILD_DIR/bench_tiered_omap"

mkdir -p "$RESULTS_DIR"

# Build if needed
if [ ! -f "$BENCH" ]; then
    echo "Building..."
    mkdir -p "$BUILD_DIR"
    cd "$BUILD_DIR"
    cmake .. -DCMAKE_BUILD_TYPE=Release
    make bench_tiered_omap -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)
fi

echo "============================================"
echo " Running all paper experiments"
echo " Output: $RESULTS_DIR/"
echo "============================================"
echo

Q=${Q:-500}

echo "[1/6] Bandwidth vs N..."
"$BENCH" --exp=bandwidth --n=1024 --s=1.0 --Q=$Q --outdir="$RESULTS_DIR"

echo "[2/6] Skewness..."
"$BENCH" --exp=skewness --N=65536 --n=1024 --Q=$Q --outdir="$RESULTS_DIR"

echo "[3/6] Hot-set size..."
"$BENCH" --exp=hotsize --N=65536 --s=1.0 --Q=$Q --outdir="$RESULTS_DIR"

echo "[4/6] Split-ORAM ablation..."
"$BENCH" --exp=ablation --n=1024 --s=1.0 --Q=$Q --outdir="$RESULTS_DIR"

echo "[5/6] Read vs write..."
"$BENCH" --exp=write --N=65536 --n=1024 --s=1.0 --Q=$Q --outdir="$RESULTS_DIR"

echo "[6/6] Mode comparison..."
"$BENCH" --exp=modes --N=65536 --n=1024 --s=1.0 --Q=$Q --outdir="$RESULTS_DIR"

echo
echo "============================================"
echo " All experiments complete."
echo " CSV files in: $RESULTS_DIR/"
echo "============================================"
ls -la "$RESULTS_DIR"/*.csv
