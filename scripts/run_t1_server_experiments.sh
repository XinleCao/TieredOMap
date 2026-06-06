#!/usr/bin/env bash
# T1/TEE server-side experiment runner.
#
# Intended for the SGX/vSGX server.  It measures only in-process
# server-side computation time; no client/network round trip is included.

set -euo pipefail

PROJECT_DIR=${PROJECT_DIR:-"$PWD"}
BUILD_DIR=${BUILD_DIR:-"$PROJECT_DIR/build"}
OUTROOT=${OUTROOT:-"$PROJECT_DIR/results_server"}
STAMP=${STAMP:-$(date +%Y%m%d_%H%M%S)}

MIN_LOGN=${MIN_LOGN:-14}
MAX_LOGN=${MAX_LOGN:-22}
Q=${Q:-1000}
N_HOT=${N_HOT:-1024}
VALUE_SIZE=${VALUE_SIZE:-32}
BETAS=${BETAS:-10,32,100,300,1000}
TRUSTED_KB=${TRUSTED_KB:-8192}
PAGE_US=${PAGE_US:-8}

mkdir -p "$BUILD_DIR" "$OUTROOT"

cd "$PROJECT_DIR"
cmake -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD_DIR" --target bench_tee_batch bench_paper -j"$(nproc)"

run_one() {
    local name="$1"
    shift
    local outdir="$OUTROOT/$STAMP/$name"
    mkdir -p "$outdir"
    echo "=== $name ==="
    echo "outdir=$outdir"
    /usr/bin/time -v "$BUILD_DIR/bench_tee_batch" "$@" \
        --outdir "$outdir" 2>&1 | tee "$outdir/run.log"
}

run_one "t1_main_val${VALUE_SIZE}" \
    --min_logN "$MIN_LOGN" --max_logN "$MAX_LOGN" \
    --n "$N_HOT" --Q "$Q" --val "$VALUE_SIZE" \
    --betas "$BETAS" \
    --env both --trusted_kb "$TRUSTED_KB" --page_us "$PAGE_US" \
    --include_flat 1 --reuse_init 1

echo "Done: $OUTROOT/$STAMP"
