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

On Linux servers, rebuild `libcommon.a` on the server instead of reusing a
locally copied archive:

```bash
cmake -S external/EnigMap -B external/EnigMap/build_noboost_linux \
  -DCMAKE_BUILD_TYPE=Release
cmake --build external/EnigMap/build_noboost_linux --target common -j$(nproc)
cp external/EnigMap/build_noboost_linux/ods/libcommon.a \
  external/EnigMap/build_noboost/ods/libcommon.a
cmake --build build --target bench_tee_original_enigmap -j$(nproc)
```

For large runs, also patch EnigMap's ORAM server frontend to use lazy
initialization. In
`external/EnigMap/ods/oram/common/oram_client_interface.hpp`, the
`NonCachedServerFrontendInstance` member should pass `true` for the final
`LATE_INIT` template parameter. Without this, the artifact eagerly encrypts and
writes every dummy large bucket during construction, which is prohibitive for
`2^24`.

The benchmark repairs only EnigMap artifact setup. It bulk-loads a balanced OBST
into a valid Path-ORAM state, then leaves query execution on the original
`OBST::Get()` path. It also resizes EnigMap's default memory backend per
database size; the original artifact default is only `1 << 28` bytes and aborts
around `2^15` keys.

The CSV separates response latency from full server completion time.
`measured_hot_us` records hot-query response latency, `measured_cold_us` records
cold-query response latency, and `measured_answer_us` records the workload
average under the observed hit ratio. In FO mode, hot queries can release an
early response after the hot phase; `measured_total_us` still records the full
server-side work, including the oblivious cold-side work that continues after
that early response.

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
  --min_logN 12 --max_logN 18 --n 1024 --Q 200 --s 1.0 \
  --env large --outdir results_local/enig_large
```

Constrained trusted memory model:

```bash
./build/bench_tee_original_enigmap \
  --min_logN 12 --max_logN 18 --n 1024 --Q 200 --s 1.0 \
  --env constrained --trusted_kb 8192 --page_us 8 \
  --outdir results_local/enig_constrained
```

Real trusted hardware:

```bash
./build/bench_tee_original_enigmap \
  --min_logN 12 --max_logN 18 --n 1024 --Q 200 --s 1.0 \
  --env hardware --trusted_kb 8192 \
  --outdir results_sgx/enig_hardware
```

For the current paper-facing TEE table, use the dedicated runner:

```bash
bash scripts/run_tee_original_enigmap_paper.sh results/tee_original_enigmap_n1024_s10
```

By default it runs $N \in \{2^{16},2^{20},2^{24}\}$ with
`n=1024`, Zipf `s=1.0`, and `Q=200`. It emits both the 256B map-side
comparison and the 4KB full-query comparison with a reusable disk data-ORAM
cache.

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
