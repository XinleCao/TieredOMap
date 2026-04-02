# TieredOMap

Exploiting access skewness for efficient oblivious key-value stores.

## Overview

TieredOMap partitions keys into independent **hot** and **cold** oblivious maps (OMAPs) and searches both in parallel on every query. It offers two operating modes:

- **Full Obliviousness** — identical security to a standard OMAP (zero additional leakage), yet the client receives hot-key answers after only O(log n) rounds instead of O(log N).
- **Tier-Membership Privacy** — reveals one bit (hot or cold) per query, reducing hot-key bandwidth to O(log²n + log N) via Split-ORAM.

The system is agnostic to the underlying OMAP construction: any OMAP (AVL-based, B⁺-tree-based, etc.) can be used as a drop-in component.

## Prerequisites

- C++17 compiler (GCC 9+ or Clang 10+)
- CMake >= 3.16
- OpenSSL (libssl-dev / openssl@3)
- [Google Test](https://github.com/google/googletest) (optional, for unit tests)

## Building

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

## Running Tests

```bash
cd build
ctest --output-on-failure
```

## Running Benchmarks

### Local mode

All ORAM storage resides in the same process (no network):

```bash
cd build

# Bandwidth and latency experiments
./bench_paper --exp=bandwidth --Q=500 --max_logN=20

# Backend comparison (standalone OMAPs)
./bench_paper --exp=backend_cmp --Q=500 --max_logN=20

# Skewness sensitivity
./bench_paper --exp=skewness --Q=500

# Dynamic hot-set maintenance convergence
./bench_paper --exp=dynamic

# Workload drift recovery
./bench_paper --exp=drift
```

### Client-server mode (TCP)

For measuring real network latency, run the storage server on a separate machine:

```bash
# On the server machine
bash scripts/run_server.sh [port]

# On the client machine
bash scripts/run_paper_experiments.sh [max_logN] [Q] [server_host] [port]

# Example: run all experiments against a remote server
bash scripts/run_paper_experiments.sh 20 500 10.0.0.1 12345
```

### TEE mode (Intel SGX via Gramine)

Requires an SGX-capable machine with [Gramine](https://gramine.readthedocs.io/) installed.
See the `*.manifest.template` files for Gramine configuration examples.

```bash
# Build the TEE server and benchmark
cd build && make tee_server bench_tee bench_tee_e2e

# Generate and sign the Gramine manifest
gramine-manifest -Darch_libdir=/lib/x86_64-linux-gnu \
    tee_server.manifest.template tee_server.manifest
gramine-sgx-sign --manifest tee_server.manifest \
    --output tee_server.manifest.sgx

# Run inside the SGX enclave
gramine-sgx tee_server
```

## Experiments

The unified benchmark binary `bench_paper` supports the following experiments, each producing a CSV file:

| Experiment | Flag | Description |
|---|---|---|
| `backend_cmp` | `--exp=backend_cmp` | Standalone OMAP backend comparison (AVL, B⁺, DA+AVL, DA+B⁺) |
| `bandwidth` | `--exp=bandwidth` | Bandwidth & rounds vs database size N |
| `skewness` | `--exp=skewness` | Effect of Zipf skewness parameter s |
| `hotsize` | `--exp=hotsize` | Effect of hot-set size n |
| `latency` | `--exp=latency` | End-to-end latency under different RTTs |
| `modes` | `--exp=modes` | Security mode comparison (FO vs TM vs TM+Split) |
| `write` | `--exp=write` | Read vs write overhead |
| `dynamic` | `--exp=dynamic` | Dynamic hot-set maintenance convergence |
| `workload` | `--exp=workload` | YCSB workload distributions (Zipfian / Uniform / Latest) |
| `drift` | `--exp=drift` | Workload drift adaptation |

Common options: `--Q=<queries>`, `--max_logN=<log₂N>`, `--host=<ip>`, `--port=<port>`, `--outdir=<dir>`.

## Project Structure

```
include/tiered_omap/
  common.h                  # Types, bandwidth stats, EpochMeta, utilities
  maintenance.h             # Dynamic hot-set maintenance (epoch-based)
  workload.h                # ZipfSampler, UniformSampler, LatestSampler
  tiered_omap.h             # TieredOMap: parallel traversal + two security modes
  oram/
    path_oram.h             # Path ORAM with AES-128-CTR encryption
    da_oram.h               # De-amortized ORAM (PRF-based position map)
    binary_tree_storage.h   # Array-backed binary tree storage
  omap/
    omap_interface.h        # OMAP interface (incl. partial_dummy_access, piggyback)
    avl_omap.h              # AVL-tree OMAP with Split-ORAM support
    bplus_omap.h            # B⁺-tree OMAP
    da_ost_omap.h           # DA-OST OMAP (DAORAM + oblivious data structure)
  network/
    tcp_channel.h           # TCP communication channel
    network_storage.h       # Remote storage proxy (client side)
    storage_server.h        # Storage server (server side)
    storage_interface.h     # Abstract storage interface
  tee/
    oblivious.h             # Oblivious primitives (cmov, o_swap, o_compact)
    enclave_oram.h          # Doubly-oblivious ORAM for enclave execution
    packed_directory.h      # Oblivious packed hot-key directory
    tee_avl_omap.h          # Doubly-oblivious AVL OMAP for TEE
    tee_omap.h              # TEE-mode TieredOMap
    tee_server.h            # TEE server and client

src/                        # Implementations (.cpp)
tests/                      # Google Test suites
benchmark/                  # Benchmark programs
  bench_paper.cpp           # Unified experiment entry point (10 experiments)
  bench_dynamic.cpp         # Dynamic maintenance experiments
  bench_latency.cpp         # Latency experiments
  bench_rounds.cpp          # Round reduction experiments
  bench_large.cpp           # Large-scale single-point measurements
  bench_comptime.cpp        # Pure computation time measurement
  bench_tee.cpp             # TEE in-enclave benchmark
  bench_tee_e2e.cpp         # TEE end-to-end (client-server) benchmark
  bench_tiered_omap.cpp     # Basic bandwidth experiments
scripts/
  run_server.sh             # Start the ORAM storage server
  run_paper_experiments.sh  # Run all paper experiments (local or TCP)
  run_experiments.sh        # Run basic experiments (local)
  plot_figures.py           # Convert CSV results to pgfplots coordinates
tools/
  oram_server.cpp           # Standalone ORAM storage server binary
  tee_server_main.cpp       # Standalone TEE server binary
```

## License

This project is licensed under the MIT License. See [LICENSE](LICENSE) for details.
