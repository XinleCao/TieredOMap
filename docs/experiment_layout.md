# Experiment File Layout

This repository keeps experiment code, runner scripts, and result artifacts in
separate places.  Use this file as the first stop before running or updating
experiments.

## Source Code

- `benchmark/`: C++ benchmark binaries.
- `tools/`: standalone server binaries, including `oram_server`.
- `scripts/`: shell and Python experiment runners.
- `docs/`: experiment plans, related-work notes, and benchmark notes.

Important current entry points:

- `scripts/run_revised_experiments.sh`: current mainline runner.
  It runs the FO-only client/server experiment and the TEE FO/BatchTM
  experiment under large and constrained trusted-memory settings.
- `scripts/run_tee_original_enigmap_paper.sh`: current paper-facing TEE runner
  against the original EnigMap code.  It uses `n=1024`, Zipf `s=1.0`, and runs
  both the 256B map-side and 4KB full-query settings.
- `scripts/run_stash_ablation_experiments.sh`: paper-facing runner for the
  stash-only client-state appendix ablation.
- `scripts/run_paper_experiments.sh`: older broad paper runner that still covers
  backend, bandwidth, skewness, hotsize, mode, dynamic, workload, and drift
  experiments.
- `scripts/run_server.sh`: starts the remote ORAM storage server.
- `scripts/sim_dynamic.py`: dynamic hot-set maintenance simulator.
- `scripts/sim_stash_ablation.py`: ordinary ORAM stash ablation simulator.

## Result Directories

- `results/`: legacy tracked local CSVs from earlier paper experiments.
- `results_final/`: tracked CSVs used as final-style local experiment snapshots.
- `server_results/`: tracked server-side experiment snapshots and recovered runs.
- `tee_results/`, `tee_results_v2/`, `tee_results_sgx/`: tracked TEE experiment
  snapshots from different rounds.
- `test_results/`, `test_tcp_results/`: tracked smoke-test and TCP-test results.
- `results_local/`: local scratch outputs.  This directory is ignored by Git
  except for the few historical tracked files already present.

Use `results_local/<topic>/...` for ad hoc runs.  For example, stash ablation
scratch output belongs under:

```text
results_local/client_state/stash_ablation/
```

When a result becomes paper-facing, move or copy only the selected CSV into a
tracked result directory with a clear name.  Do not commit large raw run
directories unless they are needed for reproducibility.

For the paper-facing stash-only ablation, use:

```bash
OUTDIR=results/client_state_stash_ablation_<date> \
  bash scripts/run_stash_ablation_experiments.sh
```

## External Baselines

- `external/`: optional third-party baseline code such as EnigMap.

This directory is ignored by Git.  Keep external code there so CMake can find
optional local baselines without mixing third-party source into this repository.

## Server Workflow

On both machines:

```bash
mkdir -p build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target bench_paper bench_tee_batch oram_server
```

On the storage server:

```bash
bash scripts/run_server.sh 12345
```

On the client:

```bash
bash scripts/run_revised_experiments.sh 20 500 <SERVER_IP> 12345 results_local/server_run
```

For client-state experiments, follow:

```text
docs/static_client_state_experiment_plan.md
```
