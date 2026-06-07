# TEE Original EnigMap Full-Bulk Large-Memory Results

Date: 2026-06-07

This run evaluates SOMAP-style packing on top of the original EnigMap AVL/OBST
query path under the large trusted-memory setting. The benchmark repairs only
the setup path: it bulk-loads a valid balanced OBST into EnigMap's Path ORAM and
then executes queries through the original `OBST::Get()` implementation.

## Parameters

- Hardware setting: large trusted memory.
- Server: 256 GB memory machine with 128 GB intended trusted-memory budget.
- `value_size = 256` bytes.
- `n = 128` hot keys.
- Zipf skew `s = 1.2`.
- `Q = 200` sampled queries per row.
- Tested scales: `2^16`, `2^20`, `2^24`.

## Files

- Combined CSV: `tee_original_enigmap_large.csv`
- Per-scale raw CSVs: `log16.csv`, `log20.csv`, `log24.csv`

## Key Results

| logN | Flat us | FO us | FO speedup | TM us | TM speedup | Hit pct |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 16 | 2210.273 | 2374.084 | 0.931 | 667.174 | 3.313 | 75.000 |
| 20 | 3574.334 | 3706.316 | 0.964 | 1072.782 | 3.332 | 73.000 |
| 24 | 5188.552 | 5376.705 | 0.965 | 1607.261 | 3.228 | 71.000 |

The `2^24` run completed successfully. Its EnigMap backend estimate was
72.00 GiB and peak RSS was about 88.9 GB, so it stayed within the large-memory
server envelope.

## Interpretation

The TM configuration shows a stable benefit over the flat EnigMap baseline,
with roughly 3.2x to 3.3x measured speedup at these scales. This is the expected
large-memory TEE result: hot queries avoid the cold OBST path, while cold
queries still use EnigMap's original `OBST::Get()`.

The FO configuration is slightly slower than the flat baseline. This is also
expected for the full-oblivious template because each query still performs both
hot-side and cold-side work, so packing does not reduce the critical query work
unless the mode is allowed to exploit hot/cold branching or batching.

This run is therefore most useful as evidence for the large trusted-memory TM
case over EnigMap's optimized AVL/OBST baseline. FO should not be presented as
an acceleration result in this specific TEE/EnigMap setting.
