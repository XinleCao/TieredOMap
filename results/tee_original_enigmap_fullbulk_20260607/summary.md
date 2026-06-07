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

Response latency follows the same hot/cold/average split as the client/server
experiments. `FO hot response` is the early hot-phase response time, `FO cold
response` is the cold-query response time, and `FO avg response` is the workload
average under the measured hit ratio.

| logN | Flat response us | FO hot response us | FO cold response us | FO avg response us | FO avg speedup | FO total us | FO total speedup | Hit pct |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 16 | 2203.495 | 160.431 | 2335.266 | 704.140 | 3.129 | 2361.215 | 0.933 | 75.000 |
| 20 | 3584.886 | 160.847 | 3664.092 | 1106.723 | 3.239 | 3703.745 | 0.968 | 73.000 |
| 24 | 5189.755 | 160.476 | 5325.795 | 1658.419 | 3.129 | 5370.560 | 0.966 | 71.000 |

For TM, response time and completion time coincide because the server skips the
unneeded tier once tier membership is public.

| logN | TM hot response us | TM cold response us | TM avg response us | TM speedup | Hit pct |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 16 | 159.781 | 2180.238 | 664.895 | 3.314 | 75.000 |
| 20 | 160.103 | 3528.029 | 1069.443 | 3.352 | 73.000 |
| 24 | 159.745 | 5150.748 | 1607.136 | 3.229 | 71.000 |

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
and cold-side work. Its response latency is much better, however: hot queries
can release the response after the hot phase while the oblivious cold-side work
continues. Under this workload, the hot/cold mix gives an average FO response
speedup of about 3.1x to 3.2x.

This run is therefore useful for two claims: TM improves server completion time,
and FO improves time-to-answer without exposing tier membership in the query
shape.
