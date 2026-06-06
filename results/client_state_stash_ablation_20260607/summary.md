# Stash-Only Client-State Ablation

This experiment tests whether ordinary Path-ORAM stashes can replace a dedicated local hot index. The server-visible FO template is fixed for every query.

Template:

```text
first data-ORAM path + full index OMAP lookup/update + second data-ORAM path
```

A data-stash hit can answer locally at round 0. An index-only stash hit can answer after the first data path. A miss answers after the full template.

## Parameters

- logNs: `16,20`
- alphas: `0.8,1.0,1.2`
- Q: `100000`
- warmup: `10000`
- hot_n reference: `1024`
- bucket size: `4`
- value bytes: `4096`
- baseline answer rounds: `53.0`

## Result Table

| logN | alpha | data stash hit % | index-only hit % | early hit % | top-hot mass % | mean stash data/index | max stash data/index | mean answer rounds | reduction % |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 16 | 0.8 | 0.0000 | 0.0010 | 0.0010 | 37.50 | 0.000/0.022 | 3/7 | 53.000 | 0.0007 |
| 16 | 1.0 | 0.0000 | 0.0120 | 0.0120 | 64.36 | 0.000/0.015 | 2/7 | 52.996 | 0.0084 |
| 16 | 1.2 | 0.0000 | 0.0280 | 0.0280 | 86.02 | 0.000/0.010 | 3/5 | 52.990 | 0.0195 |
| 20 | 0.8 | 0.0000 | 0.0010 | 0.0010 | 20.60 | 0.000/0.029 | 2/9 | 53.000 | 0.0007 |
| 20 | 1.0 | 0.0000 | 0.0100 | 0.0100 | 52.00 | 0.001/0.020 | 3/8 | 52.996 | 0.0070 |
| 20 | 1.2 | 0.0030 | 0.0270 | 0.0300 | 82.24 | 0.000/0.009 | 1/5 | 52.988 | 0.0218 |

## Interpretation

- The largest ordinary-stash early hit rate in this run is `0.0300%`, while the corresponding top-hot mass can reach `86.02%`.
- This supports using the experiment as an appendix ablation: ordinary ORAM stash state is governed by eviction pressure, not workload popularity, so it does not make the dedicated hot index redundant.
