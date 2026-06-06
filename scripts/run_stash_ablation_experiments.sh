#!/usr/bin/env bash
set -euo pipefail

# Paper-facing runner for the stash-only client-state ablation.
# Override variables from the shell, e.g.:
#   LOGNS=16,20,24 Q=200000 OUTDIR=results/client_state_stash_ablation_full \
#     bash scripts/run_stash_ablation_experiments.sh

OUTDIR="${OUTDIR:-results/client_state_stash_ablation_$(date +%Y%m%d_%H%M%S)}"
LOGNS="${LOGNS:-16,20}"
ALPHAS="${ALPHAS:-0.8,1.0,1.2}"
Q="${Q:-100000}"
WARMUP="${WARMUP:-10000}"
HOT_N="${HOT_N:-1024}"
BUCKET_SIZE="${BUCKET_SIZE:-4}"
VALUE_BYTES="${VALUE_BYTES:-4096}"
DATA_PATH_ROUNDS="${DATA_PATH_ROUNDS:-16}"
TAIL_ROUNDS="${TAIL_ROUNDS:-37}"
SEED="${SEED:-42}"
BATCH="${BATCH:-100000}"

mkdir -p "$OUTDIR"

python3 scripts/sim_stash_ablation.py \
  --logNs "$LOGNS" \
  --alphas "$ALPHAS" \
  --Q "$Q" \
  --warmup "$WARMUP" \
  --hot-n "$HOT_N" \
  --bucket-size "$BUCKET_SIZE" \
  --value-bytes "$VALUE_BYTES" \
  --data-path-rounds "$DATA_PATH_ROUNDS" \
  --tail-rounds "$TAIL_ROUNDS" \
  --seed "$SEED" \
  --batch "$BATCH" \
  --out "$OUTDIR/stash_ablation.csv" \
  --summary "$OUTDIR/summary.md"

echo "stash ablation results written to $OUTDIR"
