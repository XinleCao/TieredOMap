#!/usr/bin/env bash
# Template for a direct TCP TEE end-to-end run.
#
# Required:
#   TEE_HOST=<host or IP of the TEE server>
#
# Optional:
#   TEE_USER=<ssh user>              default: current local user
#   TEE_KEY=<ssh private key path>   default: use ssh agent/default keys
#   REMOTE_REPO=<remote repo path>   default: $HOME/TieredOMap on the server
#   LOCAL_REPO=<local repo path>     default: current working directory
#   OUTDIR=<output directory>        default: results_local/tee_e2e_results

set -euo pipefail

: "${TEE_HOST:?set TEE_HOST to the TEE server host or IP}"

TEE_USER="${TEE_USER:-$USER}"
TEE_KEY="${TEE_KEY:-}"
REMOTE_REPO="${REMOTE_REPO:-\$HOME/TieredOMap}"
LOCAL_REPO="${LOCAL_REPO:-$PWD}"
PORT="${PORT:-12345}"
OUTDIR="${OUTDIR:-results_local/tee_e2e_results}"
LOGFILE="$OUTDIR/run.log"

MIN_LOGN="${MIN_LOGN:-14}"
MAX_LOGN="${MAX_LOGN:-22}"
N_HOT="${N_HOT:-1024}"
Q="${Q:-200}"
S="${S:-1.0}"
MAX_WAIT="${MAX_WAIT:-14400}"

mkdir -p "$OUTDIR"

log() {
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] $*" | tee -a "$LOGFILE"
}

ssh_key_args=()
if [[ -n "$TEE_KEY" ]]; then
    ssh_key_args=(-i "$TEE_KEY")
fi

ssh_tee=(ssh "${ssh_key_args[@]}" -o StrictHostKeyChecking=accept-new -o ConnectTimeout=10 "$TEE_USER@$TEE_HOST")
scp_tee=(scp "${ssh_key_args[@]}" -o StrictHostKeyChecking=accept-new)

log "=== TEE E2E Benchmark (direct TCP, port $PORT) ==="
log "TEE server: $TEE_HOST  logN range: $MIN_LOGN..$MAX_LOGN"

echo "logN,N,n,avg_hot_early_ms,avg_cold_total_ms,avg_all_ms,hit_pct" > "$OUTDIR/tee_e2e.csv"

for logN in $(seq "$MIN_LOGN" 2 "$MAX_LOGN"); do
    N=$((1 << logN))
    n=$N_HOT
    if [ "$n" -gt $((N / 2)) ]; then n=$((N / 2)); fi

    log ""
    log ">>> logN=$logN  N=$N  n=$n"

    "${ssh_tee[@]}" "kill \$(pgrep -f 'gramine-sgx') 2>/dev/null; \
              kill \$(pgrep -f tee_server) 2>/dev/null; \
              tmux kill-session -t sgx 2>/dev/null; \
              sleep 2; echo 'cleaned'" 2>&1 | tee -a "$LOGFILE" || true

    log "Preparing SGX manifest for logN=$logN..."
    "${ssh_tee[@]}" "cd $REMOTE_REPO && \
        sed -i 's/--min_logN\", \"[0-9]*/--min_logN\", \"$logN/' tee_server.manifest.template && \
        sed -i 's/--max_logN\", \"[0-9]*/--max_logN\", \"$logN/' tee_server.manifest.template && \
        gramine-manifest -Darch_libdir=/lib/x86_64-linux-gnu -Dtieredomap_root=$REMOTE_REPO tee_server.manifest.template tee_server.manifest 2>&1 | tail -1 && \
        gramine-sgx-sign --manifest tee_server.manifest --output tee_server.manifest.sgx 2>&1 | tail -3 && \
        rm -f /tmp/tee_server_e2e_${logN}.log && \
        echo 'Manifest signed'" 2>&1 | tee -a "$LOGFILE"

    log "Launching SGX enclave..."
    "${ssh_tee[@]}" "cd $REMOTE_REPO && \
        tmux new-session -d -s sgx 'gramine-sgx tee_server 2>&1 | tee /tmp/tee_server_e2e_${logN}.log'" 2>&1 | tee -a "$LOGFILE"

    log "Waiting for server to finish OMAP init..."
    WAITED=0
    while true; do
        READY=$("${ssh_tee[@]}" "grep -c 'Waiting for client' /tmp/tee_server_e2e_${logN}.log 2>/dev/null || echo 0" 2>/dev/null || echo 0)
        if [ "$READY" -ge 1 ] 2>/dev/null; then
            log "  Server ready after ${WAITED}s!"
            break
        fi
        sleep 10
        WAITED=$((WAITED + 10))
        if [ $((WAITED % 120)) -eq 0 ]; then
            log "  still waiting... ${WAITED}s elapsed"
            "${ssh_tee[@]}" "tail -3 /tmp/tee_server_e2e_${logN}.log 2>/dev/null" 2>/dev/null | tee -a "$LOGFILE" || true
        fi
        if [ "$WAITED" -ge "$MAX_WAIT" ]; then
            log "  TIMEOUT after ${MAX_WAIT}s, skipping logN=$logN"
            break
        fi
    done

    if [ "$WAITED" -ge "$MAX_WAIT" ]; then
        "${ssh_tee[@]}" "tmux kill-session -t sgx 2>/dev/null" 2>/dev/null || true
        continue
    fi

    log "Running benchmark Q=$Q queries (direct TCP to $TEE_HOST:$PORT)..."
    cd "$LOCAL_REPO"
    set +e
    ./build/bench_tee_e2e \
        --host "$TEE_HOST" --port "$PORT" \
        --min_logN "$logN" --max_logN "$logN" \
        --n "$N_HOT" --Q "$Q" --s "$S" \
        --out "$OUTDIR/tee_e2e.csv" 2>&1 | tee -a "$LOGFILE"
    BENCH_RC=${PIPESTATUS[0]}
    set -euo pipefail

    if [ "$BENCH_RC" -ne 0 ]; then
        log "WARNING: Benchmark exited with code $BENCH_RC for logN=$logN"
    else
        log "Benchmark succeeded for logN=$logN"
    fi

    "${scp_tee[@]}" "$TEE_USER@$TEE_HOST:/tmp/tee_server_e2e_${logN}.log" \
        "$OUTDIR/server_${logN}.log" 2>/dev/null || true

    "${ssh_tee[@]}" "tmux kill-session -t sgx 2>/dev/null; \
              kill \$(pgrep -f 'gramine-sgx') 2>/dev/null; \
              kill \$(pgrep -f tee_server) 2>/dev/null; echo 'killed'" 2>/dev/null | tee -a "$LOGFILE" || true

    log "Sleeping 5s before next logN..."
    sleep 5
done

log ""
log "=== ALL DONE ==="
log "Results:"
cat "$OUTDIR/tee_e2e.csv" 2>/dev/null | tee -a "$LOGFILE"
