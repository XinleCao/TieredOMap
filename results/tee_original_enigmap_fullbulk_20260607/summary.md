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

| logN | Flat us | FO answer us | FO total us | FO answer speedup | FO total speedup | TM us | TM speedup | Hit pct |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 16 | 2203.495 | 704.140 | 2361.215 | 3.129 | 0.933 | 664.918 | 3.314 | 75.000 |
| 20 | 3584.886 | 1106.723 | 3703.745 | 3.239 | 0.968 | 1069.465 | 3.352 | 73.000 |
| 24 | 5189.755 | 1658.419 | 5370.560 | 3.129 | 0.966 | 1607.158 | 3.229 | 71.000 |

The `2^24` run completed successfully. Its EnigMap backend estimate was
72.00 GiB and peak RSS was about 88.9 GB, so it stayed within the large-memory
server envelope.

## Interpretation

The TM configuration shows a stable completion-time benefit over the flat
EnigMap baseline, with roughly 3.2x to 3.4x measured speedup at these scales.
This is the expected large-memory TEE result: hot queries avoid the cold OBST
path, while cold queries still use EnigMap's original `OBST::Get()`.

The FO configuration has two distinct metrics. Its server completion time is
slightly slower than the flat baseline because FO still performs both hot-side
and cold-side work. Its answer latency is much better, however: hot queries can
release the response after the hot phase while the oblivious cold-side work
continues. The measured FO answer speedup is about 3.1x to 3.2x in this run.

This run is therefore useful for two claims: TM improves server completion time,
and FO improves time-to-answer without exposing tier membership in the query
shape.
