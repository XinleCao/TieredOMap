# TieredOMap Experiment Guide

This guide maps the ICDE 2027 submission to the public artifact. The current
paper-facing experiments are intentionally narrower than some historical code
paths in the repository. In particular, client/server experiments use FO only,
while tier-membership leakage is evaluated only in the batched TEE setting.

## 1. Build

Install a C++17 compiler, CMake, OpenSSL, and Google Test. Then build the main
targets:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target bench_paper bench_tee_batch oram_server
```

For the optional original-EnigMap comparison, place EnigMap under
`external/EnigMap` and follow `docs/tee_original_enigmap_benchmark.md`.

## 2. Current Paper-Facing Entry Points

### Client/Server FO

Start the storage server:

```bash
bash scripts/run_server.sh 12345
```

Run the client/server FO experiment from the client machine:

```bash
HOST=<SERVER_IP> PORT=12345 Q=1024 LOGNS="16 20 24" \
  bash scripts/run_table2_client_fo_remote.sh
```

This script calls:

```bash
./build/bench_paper --exp=client_fo
```

The generated CSV contains the full FO bandwidth and rounds, plus the time at
which the answer becomes available. When `RTT_MS` is set, the response columns
are modeled from answer rounds. Otherwise they are measured from wall-clock TCP
execution.

### Dynamic FO Bandwidth

The dynamic maintenance experiment keeps FO security and measures the bandwidth
cost of fixed-rate maintenance:

```bash
./build/bench_paper --exp=client_dynamic_bw --Q=500 --max_logN=20 \
  --dyn_obs=256 --dyn_swap=32 --dyn_cache=8 --outdir=results_local/dynamic_bw
```

The dynamic hit-rate curves used by the paper are simulator outputs:

```bash
python scripts/sim_dynamic.py results_local/dynamic_sim
```

### TEE FO and BatchTM

The main TEE runner in the paper compares original EnigMap, TieredOMap-FO, and
TieredOMap-BatchTM:

```bash
bash scripts/run_tee_original_enigmap_paper.sh \
  results_local/tee_original_enigmap_n1024_s10
```

It uses `n=1024`, Zipf `s=1.0`, and `Q=200`. The 256 B map-side setting stores
values directly in the map. The 4 KB full-query setting stores 8-byte references
in the map and retrieves the payload through a disk-backed data ORAM.

## 3. Submitted Result Files

The paper-reported tables are kept in a compact, reviewer-facing form under
`results/paper_tables/`.

| Paper item | File |
| --- | --- |
| Client/server FO table | `results/paper_tables/client_server_fo_table.csv` |
| TEE EnigMap table | `results/paper_tables/tee_enigmap_table.csv` |
| Dynamic convergence | `results_paper/dynamic.csv` |
| Workload drift | `results_paper/drift.csv` |
| Skewness/hot-size profile | `results_paper/profile.csv` |

The repository also contains historical raw and diagnostic result snapshots.
They are useful for development checks, but the files above are the authoritative
mapping for the current submitted paper.

`results/paper_tables/client_server_fo_table.csv` should be read as the
submitted table snapshot. Its bandwidth and round counts match the tracked FO
run, while the response-time columns are the submitted WAN measurements. Reruns
on another network may change response time without changing the protocol
rounds. `results/paper_tables/tee_enigmap_table.csv` is derived from the
tracked combined EnigMap comparison CSV and rounded to the paper table
precision.

## 4. Historical Diagnostics

`bench_paper` still exposes older experiment names such as `bandwidth`,
`latency`, and `modes`. These paths are retained because they helped compare
intermediate designs. They should not be used as the main evidence for the
current client/server claim, since some of them evaluate per-query
tier-membership variants that are no longer the submitted client/server mode.

Useful diagnostic commands:

```bash
./build/bench_paper --exp=backend_cmp --Q=500 --max_logN=20
./build/bench_paper --exp=profile --Q=200 --max_logN=24 --outdir=results_local/profile
python scripts/compute_skewness_hotsize.py results_paper/profile.csv
```

## 5. Reproducibility Notes

- Client/server response time depends on the network environment. The paper
  table records the submitted WAN measurements; reruns on another network may
  reproduce the same rounds and bandwidth while producing different wall-clock
  response times.
- TEE table values report server-side time and exclude network RTT.
- Initialization time is not included in per-query latency. TEE runners report
  initialization separately when relevant.
- `results_local/` is intended for scratch outputs and is ignored by Git except
  for historical tracked files already present in the repository.
- Raw remote logs and machine-specific captures were removed from the public
  artifact; keep new rerun logs under `results_local/`.
