# TieredOMap ICDE 2027 Artifact

This file is the shortest reviewer path through the public artifact. The
repository branch for review should be `codex/icde-public-artifact`.

## 1. Build

Install a C++17 compiler, CMake, OpenSSL, and Google Test. Then run:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target bench_paper bench_tee_batch oram_server
```

If Google Test is available, run:

```bash
ctest --test-dir build --output-on-failure
```

The optional original-EnigMap comparison target needs EnigMap under
`external/EnigMap`; the core artifact builds without it.

## 2. Smoke Runs

Local FO smoke run:

```bash
bash scripts/run_revised_experiments.sh 14 50 "" 12345 results_local/smoke
```

Client/server FO smoke run:

```bash
# Storage server
bash scripts/run_server.sh 12345

# Client
HOST=<SERVER_IP> PORT=12345 Q=50 LOGNS=14 OUTDIR=results_local/client_fo_smoke \
  bash scripts/run_table2_client_fo_remote.sh
```

TEE BatchTM smoke run:

```bash
./build/bench_tee_batch --max_logN 14 --Q 50 --outdir results_local/tee_batch_smoke
```

## 3. Paper Table Mapping

The compact submitted-table snapshots are:

| Paper item | Artifact file | Regeneration path |
| --- | --- | --- |
| Client/server FO table | `results/paper_tables/client_server_fo_table.csv` | `scripts/run_table2_client_fo_remote.sh` |
| TEE EnigMap table | `results/paper_tables/tee_enigmap_table.csv` | `scripts/run_tee_original_enigmap_paper.sh` |
| Dynamic convergence and drift | `results_paper/dynamic.csv`, `results_paper/drift.csv` | `scripts/sim_dynamic.py` |
| Skewness and hot-size profile | `results_paper/profile.csv` | `scripts/compute_skewness_hotsize.py` |

`client_server_fo_table.csv` is a submitted table snapshot. Its bandwidth and
round counts match the tracked FO run in
`results/table2_client_fo_20260606_212627/client_fo.csv`; its response-time
columns are the submitted WAN measurements from the paper. Reruns on a
different network should reproduce the same protocol rounds and bandwidth, but
wall-clock response times can change with RTT, congestion, and host load.

`tee_enigmap_table.csv` is derived from
`results/tee_original_enigmap_n1024_s10_20260608_231901/tee_original_enigmap_combined.csv`
and rounded to the submitted table precision.

## 4. Environment Limits

- Client/server response time is network-dependent. The submitted paper used a
  WAN setting; local or cloud reruns can have different wall-clock latencies.
- TEE results report server-side time and exclude network RTT.
- The EnigMap baseline is optional because it requires third-party source code
  under `external/EnigMap`.
- SGX/Gramine templates are provided as reviewer-local templates. Generate them
  with a local repository path rather than the original authors' server paths.
- `results_local/` is for scratch reruns. The committed paper snapshots live in
  `results/paper_tables/` and `results_paper/`.
