#!/usr/bin/env bash
set -eo pipefail

# Generic TEE E2E runner. Configure these variables for your own SGX machine.
# This file intentionally contains no private host, username, or SSH key path.

TEE_HOST="${TEE_HOST:-}"
TEE_USER="${TEE_USER:-}"
TEE_KEY="${TEE_KEY:-}"
TEE_REPO="${TEE_REPO:-~/TieredOMap}"
PORT="${PORT:-12345}"
OUTDIR="${OUTDIR:-tee_e2e_results}"

MIN_LOGN="${MIN_LOGN:-14}"
MAX_LOGN="${MAX_LOGN:-22}"
N_HOT="${N_HOT:-1024}"
Q="${Q:-200}"
S="${S:-1.0}"

if [[ -z "$TEE_HOST" || -z "$TEE_USER" || -z "$TEE_KEY" ]]; then
  echo "Set TEE_HOST, TEE_USER, and TEE_KEY before running this script." >&2
  exit 2
fi

mkdir -p "$OUTDIR"
echo "logN,N,n,avg_hot_early_ms,avg_cold_total_ms,avg_all_ms,hit_pct" \
  > "$OUTDIR/tee_e2e.csv"

SSH_TEE=(ssh -i "$TEE_KEY" -o StrictHostKeyChecking=no -o ConnectTimeout=10
  "$TEE_USER@$TEE_HOST")

for logN in $(seq "$MIN_LOGN" 2 "$MAX_LOGN"); do
  N=$((1 << logN))
  n="$N_HOT"
  if [[ "$n" -gt $((N / 2)) ]]; then
    n=$((N / 2))
  fi

  echo "=== logN=$logN N=$N n=$n ==="

  "${SSH_TEE[@]}" "cd $TEE_REPO && \
    gramine-manifest -Darch_libdir=/lib/x86_64-linux-gnu \
      tee_server.manifest.template tee_server.manifest && \
    gramine-sgx-sign --manifest tee_server.manifest \
      --output tee_server.manifest.sgx && \
    tmux new-session -d -s sgx 'gramine-sgx tee_server'"

  sleep 10

  ./build/bench_tee_e2e \
    --host "$TEE_HOST" --port "$PORT" \
    --min_logN "$logN" --max_logN "$logN" \
    --n "$N_HOT" --Q "$Q" --s "$S" \
    --out "$OUTDIR/tee_e2e.csv"

  "${SSH_TEE[@]}" "tmux kill-session -t sgx 2>/dev/null || true"
done
