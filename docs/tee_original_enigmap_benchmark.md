# Original EnigMap TEE Benchmark

This benchmark evaluates the SOMAP hot tier on top of the original EnigMap
OBST/PathORAM implementation:

- `flat_original_enigmap`: original EnigMap only.
- `packed_FO_original_enigmap`: SOMAP packed hot map plus original EnigMap cold
  map, full-oblivious execution.
- `packed_TM_original_enigmap`: SOMAP packed hot map plus original EnigMap cold
  map, tier-membership execution.

The original EnigMap OBST stores only `uint64_t` values. The benchmark stores a
`uint64_t` reference in EnigMap and keeps the actual byte values in a side array.

## Build

First build the locally vendored EnigMap dependency:

```bash
cmake -S external/EnigMap -B external/EnigMap/build_noboost -DCMAKE_BUILD_TYPE=Release
cmake --build external/EnigMap/build_noboost --target common main
```

Then configure and build the TieredOMap target:

```bash
cmake -S . -B build
cmake --build build --target bench_tee_original_enigmap
```

The target is optional. It is enabled only when
`external/EnigMap/ods/otree/otree.hpp` and
`external/EnigMap/build_noboost/ods/libcommon.a` exist.

## Environments

The benchmark emits measured local timings and, optionally, a constrained trusted
memory model.

`--env large` records the measured result with no synthetic paging penalty. This
models a trusted memory budget large enough for the working set.

`--env constrained` records a synthetic small-trusted-memory result. It keeps the
measured local timing and adds a page penalty when the estimated working set
exceeds `--trusted_kb`.

`--env both` emits both rows from one measurement pass.

`--env hardware` is for real TEE/SGX runs. It labels rows as hardware and does
not add a synthetic penalty, because the measured latency already includes the
hardware behavior.

## Example Commands

Large trusted memory:

```bash
./build/bench_tee_original_enigmap \
  --min_logN 12 --max_logN 18 --n 128 --Q 200 --s 1.2 \
  --env large --outdir results_local/enig_large
```

Constrained trusted memory model:

```bash
./build/bench_tee_original_enigmap \
  --min_logN 12 --max_logN 18 --n 128 --Q 200 --s 1.2 \
  --env constrained --trusted_kb 8192 --page_us 8 \
  --outdir results_local/enig_constrained
```

Real trusted hardware:

```bash
./build/bench_tee_original_enigmap \
  --min_logN 12 --max_logN 18 --n 128 --Q 200 --s 1.2 \
  --env hardware --trusted_kb 8192 \
  --outdir results_sgx/enig_hardware
```

## CSV Columns

The CSV includes:

- `measured_*_us`: directly measured local or hardware latency.
- `modeled_total_us`: measured latency plus the synthetic constrained-memory
  penalty, if enabled.
- `speedup_measured`: measured flat EnigMap latency divided by measured config
  latency.
- `speedup_modeled`: modeled flat EnigMap latency divided by modeled config
  latency.
- `hot_working_kb` and `cold_working_kb`: estimated trusted working-set sizes.
- `cold_pages_est` and `hot_pages_avg`: page-count estimates used by the model.

For paper claims, use `measured_*` for real hardware rows. Use `modeled_*` only
as a local sensitivity study before real hardware is available.
