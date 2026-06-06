#!/usr/bin/env bash
set -euo pipefail

Q="${Q:-1024}"
WARMUP="${WARMUP:-0}"
VALUE_SIZE="${VALUE_SIZE:-4096}"
RTT_MS="${RTT_MS:-28.87}"
SKEW="${SKEW:-1.0}"
HOT_N="${HOT_N:-1024}"
LOGNS="${LOGNS:-16,20,24}"
BASE_CSV="${BASE_CSV:-results/table2_client_fo_20260606_212627/client_fo.csv}"
OUTDIR="${OUTDIR:-results/table2_client_local_index_$(date +%Y%m%d_%H%M%S)}"

cmake --build build --target bench_paper -j "${JOBS:-4}"

./build/bench_paper \
  --exp=client_local_index_fo \
  --Q="${Q}" \
  --warmup="${WARMUP}" \
  --value_size="${VALUE_SIZE}" \
  --rtt_ms="${RTT_MS}" \
  --s="${SKEW}" \
  --n="${HOT_N}" \
  --logNs="${LOGNS}" \
  --base_csv="${BASE_CSV}" \
  --outdir="${OUTDIR}"

echo "Results written to ${OUTDIR}"
