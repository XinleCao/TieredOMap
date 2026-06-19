# TieredOMap Artifact Guide

This document explains how the public artifact is organized and how it matches
the ICDE 2027 submission.

## Scope

The artifact contains:

- the TieredOMap implementation;
- local and client/server benchmark entry points;
- TEE-oriented code paths and Gramine manifest templates;
- paper-facing CSV files copied into `artifact-data/`;
- scripts for dynamic-maintenance simulation and figure-data derivation.

The artifact does not include private machine-specific run scripts. In
particular, scripts that contain a fixed cloud IP address, SSH username, or SSH
key path are intentionally replaced by a generic template.

## Main Code Paths

| Paper component | Implementation files |
| --- | --- |
| TieredOMap FO/TM framework | `include/tiered_omap/tiered_omap.h`, `src/tiered_omap.cpp` |
| AVL OMAP backend | `include/tiered_omap/omap/avl_omap.h`, `src/omap/avl_omap.cpp` |
| B+ OMAP backend | `include/tiered_omap/omap/bplus_omap.h`, `src/omap/bplus_omap.cpp` |
| DAORAM backends | `include/tiered_omap/oram/da_oram.h`, `src/oram/da_oram.cpp`, `src/omap/da_ost_omap.cpp` |
| TCP client/server storage | `include/tiered_omap/network/`, `src/network/`, `tools/oram_server.cpp` |
| Dynamic maintenance | `include/tiered_omap/maintenance.h`, `src/tiered_omap.cpp`, `scripts/sim_dynamic.py` |
| TEE mode | `include/tiered_omap/tee/`, `src/tee/`, `benchmark/bench_tee*.cpp` |

## Paper-Facing Data

| Paper evidence | Artifact data |
| --- | --- |
| Main WAN client/server table | `artifact-data/client_server/paper_wan.csv` |
| Per-backend client/server measurements | `artifact-data/client_server/per_backend.csv` |
| Scalability and skewness figures | `artifact-data/client_server/bandwidth_vs_N.csv`, `artifact-data/profile/profile.csv` |
| Dynamic maintenance, static workload | `artifact-data/dynamic/dynamic.csv` |
| Dynamic maintenance, drift recovery | `artifact-data/dynamic/drift.csv` |
| TEE/SGX table | `artifact-data/tee/tee_corrected.csv`, `artifact-data/tee/tee_full.csv` |
| Storage overhead | `artifact-data/storage/storage_cost.csv` |

The paper rounds and formats several values for tables and plots. Some figures
are derived from profile rows and Zipf hit-rate calculations rather than copied
directly from a single CSV column. The derivation script is:

```bash
python scripts/compute_skewness_hotsize.py artifact-data/profile/profile.csv
```

The dynamic-maintenance figure data can be regenerated with:

```bash
python scripts/sim_dynamic.py regenerated-dynamic
```

This produces `dynamic.csv`, `drift.csv`, `dynamic_N20.csv`, and `drift_N20.csv`
under the requested output directory.

## Quick Reproduction

Build and run unit tests:

```bash
mkdir -p build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . -j
ctest --output-on-failure
```

Run a local smoke benchmark:

```bash
./bench_paper --exp=bandwidth --Q=50 --max_logN=16 --outdir=../smoke-results
```

Run the dynamic simulation:

```bash
cd ..
python scripts/sim_dynamic.py smoke-dynamic --smoke
```

## Full Experiments

The full client/server experiments require two machines. Start the storage
server on the server machine:

```bash
bash scripts/run_server.sh 12345
```

Run the paper benchmark from the client:

```bash
bash scripts/run_paper_experiments.sh 24 500 SERVER_IP 12345
```

The full TEE experiments require SGX and Gramine. See
`scripts/run_tee_e2e_template.sh` for a generic template.

## Known Reproduction Notes

- Large `N=2^24` experiments require substantial memory and time.
- SGX measurements are hardware-dependent and may not match the paper exactly
  on a different cloud instance.
- The provided CSV files are included to let reviewers inspect the submitted
  numbers without rerunning all large experiments.
