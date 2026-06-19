# Artifact Data Index

This directory contains the paper-facing data files for the TieredOMap ICDE
2027 submission.

## Directory Map

```text
client_server/   WAN and client/server benchmark data
dynamic/         Dynamic hot-set maintenance simulation data
profile/         Profile rows used to derive skewness and hot-set-size plots
tee/             SGX/TEE benchmark data
storage/         Storage-overhead calculation
```

## Client/Server Data

- `client_server/paper_wan.csv` is the main WAN table source.
- `client_server/per_backend.csv` records baseline, FO, and TM metrics for
  multiple backends and database sizes.
- `client_server/bandwidth_vs_N.csv`, `skewness.csv`, `hotsize.csv`,
  `latency.csv`, `throughput.csv`, and `valuesize.csv` are supporting
  experiment outputs.

## Dynamic Data

- `dynamic/dynamic.csv` and `dynamic/drift.csv` correspond to `N=2^24`.
- `dynamic/dynamic_N20.csv` and `dynamic/drift_N20.csv` are the matching
  `N=2^20` runs.

Regenerate these files with:

```bash
python scripts/sim_dynamic.py artifact-data/dynamic-regenerated
```

## Profile Data

`profile/profile.csv` is used by:

```bash
python scripts/compute_skewness_hotsize.py artifact-data/profile/profile.csv
```

The script prints pgfplots-ready coordinates for the skewness and hot-set-size
figures.

## TEE Data

- `tee/tee_corrected.csv` is the primary SGX table data.
- `tee/tee_full.csv` contains the full-query 4 KB value setting.
- `tee/tee_hot_only.csv` and `tee/tee_scalability.csv` contain supporting TEE
  measurements.
- `tee/tee_e2e.csv` contains end-to-end TEE client/server measurements.

TEE results depend on SGX hardware and Gramine configuration, so exact reruns
may vary across machines.
