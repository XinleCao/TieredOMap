# TieredOMap

TieredOMap is a skewness-aware framework for oblivious key-value stores.
It separates records into a small hot tier and a large cold tier, while still
using oblivious map (OMAP) components as the underlying storage primitive.

The repository contains the implementation and artifact data for the ICDE 2027
submission.

## Overview

TieredOMap supports two modes.

- Full-oblivious mode (FO): every query accesses both the hot and cold tiers.
  The server observes a fixed protocol shape, while the client can obtain hot
  answers earlier because the hot tier is much smaller.
- Tier-membership mode (TM): the protocol intentionally reveals whether a query
  is hot or cold in exchange for lower total work on hot queries.

The implementation is backend-agnostic. It includes AVL OMAP, B+ OMAP,
DAORAM+AVL, and DAORAM+B+ backends, plus TCP client/server and TEE-oriented
components.

## Repository Layout

```text
include/tiered_omap/       Public headers
src/                       Implementations
benchmark/                 Benchmark binaries
tests/                     GoogleTest unit tests
tools/                     Server binaries
scripts/                   Experiment and post-processing scripts
artifact-data/             Paper-facing CSV data and derived experiment data
ARTIFACT.md                Reproduction guide
EXPERIMENT_GUIDE.md        Detailed benchmark guide
```

## Prerequisites

- C++17 compiler, such as GCC 9+ or Clang 10+
- CMake 3.16+
- OpenSSL development package
- GoogleTest, for unit tests
- Python 3 with NumPy, for the dynamic-maintenance simulation scripts

TEE experiments additionally require an SGX-capable machine with Gramine.

## Build

```bash
mkdir -p build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . -j
```

## Run Tests

```bash
cd build
ctest --output-on-failure
```

## Quick Artifact Smoke Test

The following commands run a small local benchmark and regenerate the dynamic
simulation data on a normal development machine.

```bash
cd build
./bench_paper --exp=bandwidth --Q=50 --max_logN=16 --outdir=../smoke-results

cd ..
python scripts/sim_dynamic.py smoke-dynamic --smoke
python scripts/compute_skewness_hotsize.py artifact-data/profile/profile.csv
```

The full WAN and SGX experiments used in the paper require a two-machine
client/server setup and an SGX server. The corresponding paper-facing data are
included under `artifact-data/`.

## Paper Data

The most important data files are:

- `artifact-data/client_server/paper_wan.csv`: WAN client/server measurements.
- `artifact-data/client_server/per_backend.csv`: per-backend client/server
  measurements.
- `artifact-data/profile/profile.csv`: profile data used to derive skewness and
  hot-set-size plots.
- `artifact-data/dynamic/dynamic.csv` and `artifact-data/dynamic/drift.csv`:
  dynamic hot-set maintenance simulations for `N=2^24`.
- `artifact-data/tee/tee_corrected.csv` and `artifact-data/tee/tee_full.csv`:
  SGX/TEE measurements.
- `artifact-data/storage/storage_cost.csv`: storage-overhead calculation.

See `artifact-data/README.md` for the mapping from paper claims to files.

## Client/Server Mode

Run the storage server on the server machine:

```bash
bash scripts/run_server.sh 12345
```

Run the benchmark on the client machine:

```bash
bash scripts/run_paper_experiments.sh 20 500 SERVER_IP 12345
```

For a local-only benchmark, omit the host argument and use a small `max_logN`.

## TEE Mode

The SGX/Gramine manifest templates are included for reproducibility, but they
usually need local path adjustment before use. A generic E2E template is
provided in `scripts/run_tee_e2e_template.sh`.

## License

This project is licensed under the MIT License. See `LICENSE` for details.
