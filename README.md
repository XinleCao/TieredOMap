# TieredOMap

TieredOMap is a skewness-aware oblivious map prototype for the ICDE 2027
submission. The system separates a small hot tier from the remaining cold tier
so that hot keys can be answered earlier while the observable access template
remains oblivious.

The submitted artifact focuses on the current paper design:

- In the client/server setting, the paper-facing experiments use Full
  Obliviousness (FO). Every query executes the complete hot-side and cold-side
  template, so the server does not learn the tier of an individual query.
- In the TEE setting, the artifact evaluates FO and batched Tier Membership
  (BatchTM). BatchTM reveals only the aggregate hot/cold composition of a
  batch, not which individual request is hot.
- Older per-query TM experiments are kept only as diagnostic code and are not
  the main evidence for the current submission.

## Repository Layout

```text
include/tiered_omap/        Public C++ headers
src/                        Core implementation
benchmark/                  Paper and diagnostic benchmark binaries
tools/                      Storage-server utilities
scripts/                    Reproduction scripts and simulators
docs/                       Experiment notes and setup details
results/paper_tables/       CSVs matching the submitted paper tables
results_paper/              Dynamic-maintenance simulator outputs
```

Important entry points:

- `ARTIFACT.md` gives the shortest reviewer path for build, smoke runs, paper
  table mapping, and environment limits.
- `scripts/run_revised_experiments.sh` builds and runs the current
  client/server FO runner and the TEE FO/BatchTM runner.
- `scripts/run_table2_client_fo_remote.sh` regenerates the client/server FO
  rows one backend and one size at a time over TCP.
- `scripts/run_tee_original_enigmap_paper.sh` regenerates the TEE comparison
  against the original EnigMap implementation.
- `scripts/sim_dynamic.py` regenerates the dynamic hot-set maintenance curves.
- `scripts/compute_skewness_hotsize.py` derives the skewness and hot-set-size
  figure coordinates from `results_paper/profile.csv`.

## Build

The core artifact requires C++17, CMake, OpenSSL, and Google Test.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target bench_paper bench_tee_batch oram_server
ctest --test-dir build --output-on-failure
```

The optional EnigMap comparison also needs the EnigMap source under
`external/EnigMap`. See `docs/tee_original_enigmap_benchmark.md` for that
setup.

## Quick Smoke Run

For a local smoke run without a remote storage server:

```bash
bash scripts/run_revised_experiments.sh 14 50 "" 12345 results_local/smoke
```

For a client/server run, start the storage server first:

```bash
# Server machine
bash scripts/run_server.sh 12345

# Client machine
bash scripts/run_revised_experiments.sh 20 500 <SERVER_IP> 12345 results_local/server_run
```

## Paper Result Mapping

The submitted paper tables are stored in `results/paper_tables/`.

| Paper item | Artifact file | Main regeneration path |
| --- | --- | --- |
| Client/server FO table | `results/paper_tables/client_server_fo_table.csv` | `scripts/run_table2_client_fo_remote.sh` |
| TEE EnigMap table | `results/paper_tables/tee_enigmap_table.csv` | `scripts/run_tee_original_enigmap_paper.sh` |
| Dynamic convergence and drift | `results_paper/dynamic.csv`, `results_paper/drift.csv` | `scripts/sim_dynamic.py` |
| Skewness and hot-size figures | `results_paper/profile.csv` | `scripts/compute_skewness_hotsize.py` |

`client_server_fo_table.csv` is a submitted table snapshot. Its bandwidth and
round counts match `results/table2_client_fo_20260606_212627/client_fo.csv`;
its response-time columns are the submitted WAN measurements and can vary on
rerun. Raw remote logs and machine-specific run captures are intentionally not
part of the public artifact.

## Security Modes

FO mode performs both tier accesses for every query. It preserves the same
observable template as a standard OMAP access while allowing the client to
return a hot value after the hot-tier traversal finishes.

BatchTM mode is used only for batched TEE experiments in the current paper. It
processes the batch through the hot directory first, releases the hot results as
a batch, and continues only cold misses through the cold OMAP. The leakage is
the batch-level hot/cold count.

## License

Academic research artifact for the TieredOMap ICDE 2027 submission.
