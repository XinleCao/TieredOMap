#!/bin/bash
set -e

WORKDIR="${TIEREDOMAP_ROOT:-$PWD}"
BUILDDIR="${TIEREDOMAP_BUILDDIR:-$WORKDIR/build}"
OUTDIR="${TIEREDOMAP_OUTDIR:-$WORKDIR/tee_results_sgx}"
mkdir -p "$OUTDIR"
rm -f "$OUTDIR"/*.csv

gen_and_run() {
    local enclave_size=$1
    local min_logN=$2
    local max_logN=$3
    local extra_args="$4"

    cat > "$WORKDIR/bench_run.manifest.template" << EOF
[libos]
entrypoint = "/bench_tee"

[loader]
log_level = "error"
argv = ["/bench_tee", "--min_logN", "$min_logN", "--max_logN", "$max_logN", "--n", "1024", "--val", "256", "--Q", "200", "--s", "0.99", "--outdir", "$OUTDIR"$extra_args]

[loader.env]
LD_LIBRARY_PATH = "/lib:/lib/x86_64-linux-gnu:/usr/lib/x86_64-linux-gnu"

[[fs.mounts]]
path = "/lib"
uri = "file:{{ gramine.runtimedir() }}"

[[fs.mounts]]
path = "/lib/x86_64-linux-gnu"
uri = "file:/lib/x86_64-linux-gnu"

[[fs.mounts]]
path = "/usr/lib/x86_64-linux-gnu"
uri = "file:/usr/lib/x86_64-linux-gnu"

[[fs.mounts]]
path = "/bench_tee"
uri = "file:$BUILDDIR/bench_tee"

[[fs.mounts]]
path = "$OUTDIR"
uri = "file:$OUTDIR"
type = "chroot"

[[fs.mounts]]
path = "/etc"
uri = "file:/etc"

[[fs.mounts]]
path = "/tmp"
uri = "file:/tmp"
type = "chroot"

[sgx]
enclave_size = "$enclave_size"
max_threads = 4
debug = false
edmm_enable = false
trusted_files = [
    "file:$BUILDDIR/bench_tee",
    "file:{{ gramine.libos }}",
    "file:{{ gramine.runtimedir() }}/",
    "file:/lib/x86_64-linux-gnu/libcrypto.so.3",
    "file:/lib/x86_64-linux-gnu/libstdc++.so.6",
    "file:/lib/x86_64-linux-gnu/libm.so.6",
    "file:/lib/x86_64-linux-gnu/libgcc_s.so.1",
    "file:/lib/x86_64-linux-gnu/libc.so.6",
    "file:/lib64/ld-linux-x86-64.so.2",
]
allowed_files = [
    "file:$OUTDIR/",
    "file:/tmp/",
]

[sys]
insecure__allow_eventfd = true
enable_sigterm_injection = true
EOF

    cd "$WORKDIR"
    gramine-manifest -Darch_libdir=/lib/x86_64-linux-gnu -Dtieredomap_root="$WORKDIR" bench_run.manifest.template bench_run.manifest
    gramine-sgx-sign --manifest bench_run.manifest --output bench_run.manifest.sgx 2>/dev/null

    echo "[$(date)] Running logN=$min_logN..$max_logN with enclave=$enclave_size ..."
    time gramine-sgx bench_run 2>&1
    echo ""
}

echo "============================================"
echo "Batch 1: logN=14..20, enclave=4G"
echo "============================================"
gen_and_run "4G" 14 20 ""

echo "============================================"
echo "Batch 2: logN=22, enclave=16G"
echo "============================================"
gen_and_run "16G" 22 22 ', "--append"'

echo "============================================"
echo "Batch 3: logN=24, enclave=56G"
echo "============================================"
gen_and_run "56G" 24 24 ', "--append"'

echo ""
echo "=== ALL SGX BATCHES COMPLETE ==="
echo "Results:"
cat "$OUTDIR/tee_scalability.csv"
echo "---"
cat "$OUTDIR/tee_hot_only.csv"
