# Experiment Scripts

Use these scripts from the repository root.

## Current Mainline

- `run_revised_experiments.sh`: current paper-facing runner.
  It runs FO-only client/server experiments and TEE FO/BatchTM experiments.
- `run_server.sh`: starts the remote ORAM storage server for client/server runs.

## Historical Broad Runner

- `run_paper_experiments.sh`: older all-in-one runner for backend, bandwidth,
  skewness, hotsize, mode, write, dynamic, workload, and drift experiments.
- `run_experiments.sh`: legacy local runner using `bench_tiered_omap`.

## Simulators and Postprocessing

- `sim_dynamic.py`: dynamic hot-set maintenance simulator.
- `sim_stash_ablation.py`: ordinary ORAM stash ablation simulator.
- `compute_skewness_hotsize.py`: derives skewness/hot-size figure coordinates
  from profile CSVs.
- `plot_figures.py`: CSV-to-plot helper for older figure generation.
