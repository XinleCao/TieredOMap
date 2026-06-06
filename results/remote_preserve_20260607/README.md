# Remote Preserve 2026-06-07

Pulled from `root@8.141.99.97:/root/tieredomap_runs` before pausing the server.

Completed probe CSVs:

- `tee_original_enigmap_large_smoke`
- `tee_original_enigmap_large_calib14`
- `tee_original_enigmap_bulk_probe_12`
- `tee_original_enigmap_bulk_probe_14`

Zero-byte CSVs are failed or interrupted probes:

- `tee_original_enigmap_large_20260607_log16`
- `tee_original_enigmap_large_probe_15`
- `tee_original_enigmap_large_probe_16`
- `tee_original_enigmap_bulk_probe16`
- `tee_original_enigmap_bulk_backend_probe16`
- `tee_original_enigmap_bulk_probe_15`

Current engineering state:

- Original EnigMap artifact bulk initialization is incomplete.
- `bench_tee_original_enigmap` now repairs setup by bulk-loading a balanced OBST
  into a valid Path-ORAM state while keeping queries on EnigMap `OBST::Get()`.
- The artifact's default EnigMap memory backend is only `1 << 28` bytes; the
  benchmark now resizes the backend per `logN`.
- On a Linux server, rebuild `external/EnigMap/build_noboost_linux/ods/libcommon.a`
  and copy it to `external/EnigMap/build_noboost/ods/libcommon.a` before
  rebuilding `bench_tee_original_enigmap`.
