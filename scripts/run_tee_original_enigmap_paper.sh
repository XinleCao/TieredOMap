#!/usr/bin/env bash
# Paper-facing TEE/EnigMap runner.
#
# Default paper parameters:
#   hot-set size n = 1024
#   Zipf skewness s = 1.0
#   query count Q = 200
#   database sizes N = 2^16, 2^20, 2^24
#
# It runs both value scopes used in the paper:
#   1. 256B map-side values, no separate data ORAM
#   2. 4KB full-query values, with OMAP values as 8-byte data references

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="${BUILD_DIR:-$PROJECT_DIR/build}"

OUTDIR="${1:-$PROJECT_DIR/results/tee_original_enigmap_n1024_s10_$(date +%Y%m%d_%H%M%S)}"
LOGNS="${LOGNS:-16 20 24}"
HOT_N="${HOT_N:-1024}"
ZIPF_S="${ZIPF_S:-1.0}"
Q="${Q:-200}"
ENV_MODE="${ENV_MODE:-large}"
TRUSTED_KB="${TRUSTED_KB:-8192}"
PAGE_US="${PAGE_US:-8}"
BUILD="${BUILD:-1}"

if command -v nproc >/dev/null 2>&1; then
  JOBS_DEFAULT="$(nproc)"
else
  JOBS_DEFAULT="$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"
fi
JOBS="${JOBS:-$JOBS_DEFAULT}"

mkdir -p "$OUTDIR"

if [ "$BUILD" = "1" ]; then
  cmake -S "$PROJECT_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
  cmake --build "$BUILD_DIR" --target bench_tee_original_enigmap -j"$JOBS"
fi

BIN="$BUILD_DIR/bench_tee_original_enigmap"
if [ ! -x "$BIN" ]; then
  echo "missing benchmark binary: $BIN" >&2
  exit 1
fi

echo "============================================"
echo " TEE Original EnigMap Paper Run"
echo " outdir=$OUTDIR"
echo " logNs=$LOGNS"
echo " n=$HOT_N  s=$ZIPF_S  Q=$Q"
echo " env=$ENV_MODE trusted_kb=$TRUSTED_KB page_us=$PAGE_US"
echo "============================================"

run_one() {
  local scope="$1"
  local logN="$2"
  local val="$3"
  local data_val="$4"
  local dir="$OUTDIR/$scope/log$logN"
  mkdir -p "$dir"

  echo ""
  echo "[$scope] logN=$logN"

  local args=(
    --min_logN "$logN"
    --max_logN "$logN"
    --n "$HOT_N"
    --Q "$Q"
    --s "$ZIPF_S"
    --val "$val"
    --data_val "$data_val"
    --env "$ENV_MODE"
    --trusted_kb "$TRUSTED_KB"
    --page_us "$PAGE_US"
    --outdir "$dir"
  )

  if [ "$data_val" != "0" ]; then
    mkdir -p "$OUTDIR/data_oram_cache" "$OUTDIR/data_oram_runtime"
    args+=(
      --data_cache "$OUTDIR/data_oram_cache"
      --data_runtime "$OUTDIR/data_oram_runtime/$scope-log$logN"
    )
  fi

  "$BIN" "${args[@]}" 2>&1 | tee "$dir/run.log"
}

combine_csv() {
  local output="$1"
  shift
  local first=1
  : > "$output"
  for csv in "$@"; do
    if [ ! -f "$csv" ]; then
      continue
    fi
    if [ "$first" = "1" ]; then
      cat "$csv" >> "$output"
      first=0
    else
      tail -n +2 "$csv" >> "$output"
    fi
  done
}

csv_256=()
csv_4kb=()
for logN in $LOGNS; do
  run_one "256B_map" "$logN" 256 0
  csv_256+=("$OUTDIR/256B_map/log$logN/tee_original_enigmap.csv")

  run_one "4KB_full_query" "$logN" 8 4096
  csv_4kb+=("$OUTDIR/4KB_full_query/log$logN/tee_original_enigmap.csv")
done

combine_csv "$OUTDIR/tee_original_enigmap_256B_map.csv" "${csv_256[@]}"
combine_csv "$OUTDIR/tee_original_enigmap_4KB_full_query.csv" "${csv_4kb[@]}"
combine_csv "$OUTDIR/tee_original_enigmap_combined.csv" \
  "$OUTDIR/tee_original_enigmap_256B_map.csv" \
  "$OUTDIR/tee_original_enigmap_4KB_full_query.csv"

echo ""
echo "Done. Combined CSV files:"
echo "  $OUTDIR/tee_original_enigmap_256B_map.csv"
echo "  $OUTDIR/tee_original_enigmap_4KB_full_query.csv"
echo "  $OUTDIR/tee_original_enigmap_combined.csv"
