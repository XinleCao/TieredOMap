# Experiment Scripts

Use these scripts from the repository root.

## Current Mainline

- `run_table2_client_fo_remote.sh`: current client/server FO table runner.
  Start `oram_server` separately, then set `HOST`, `PORT`, `LOGNS`, and `Q` on
  the client side.
- `run_tee_original_enigmap_paper.sh`: current TEE/EnigMap paper-table runner.
  It requires the optional EnigMap source and emits combined CSVs.
- `run_revised_experiments.sh`: current paper-facing runner.
  It runs FO-only client/server experiments and TEE FO/BatchTM experiments
  under large and constrained trusted-memory settings.
- `run_stash_ablation_experiments.sh`: paper-facing runner for the stash-only
  client-state appendix ablation.
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

## Result Hygiene

- Put ad hoc reruns under `results_local/`.
- Use `results/paper_tables/` for compact submitted-table CSV snapshots.
- Do not commit raw remote logs, public IP addresses, SSH key paths, usernames,
  or machine-local absolute paths.
