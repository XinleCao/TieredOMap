# TEE two-scope EnigMap results

Generated on 2026-06-07. Each point uses `n=128`, Zipf `s=1.2`, `Q=200`, and server-side time on the Alibaba Cloud machine.

- `256B map-side`: EnigMap-style comparison with 256B map values and no separate data ORAM.
- `4KB full-query`: OMAP stores compact refs (`value_size=8`) and retrieves 4KB payloads from a disk-backed data ORAM. Total and answer time include both map and data ORAM time.
- The official 4KB numbers below use `tee_two_scope_4kb_rerun_20260607_140503`, after the reusable data-ORAM setup cache already existed.

## 256B map-side

| logN | Flat total us | FO total us | FO answer us | TM total us | Hit % | FO answer speedup | TM total speedup | Avg map us (flat) | Avg data us (flat) |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 14 | 1665.941 | 1819.974 | 472.261 | 440.445 | 81.000 | 3.528 | 3.782 | 1665.941 | 0.000 |
| 16 | 2201.673 | 2353.993 | 701.618 | 662.509 | 75.000 | 3.138 | 3.323 | 2201.673 | 0.000 |
| 18 | 2794.104 | 2953.826 | 893.106 | 853.489 | 73.500 | 3.129 | 3.274 | 2794.104 | 0.000 |
| 20 | 3577.414 | 3693.582 | 1103.666 | 1067.307 | 73.000 | 3.241 | 3.352 | 3577.414 | 0.000 |
| 22 | 4294.725 | 4452.399 | 1354.732 | 1322.368 | 72.000 | 3.170 | 3.248 | 4294.725 | 0.000 |
| 24 | 5186.317 | 5375.088 | 1659.280 | 1608.154 | 71.000 | 3.126 | 3.225 | 5186.317 | 0.000 |

## 4KB full-query

| logN | Flat total us | FO total us | FO answer us | TM total us | Hit % | FO answer speedup | TM total speedup | Avg map us (flat) | Avg data us (flat) |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 14 | 1878.962 | 1976.868 | 494.741 | 465.867 | 81.000 | 3.798 | 4.033 | 1687.755 | 190.667 |
| 16 | 2749.955 | 3680.050 | 1440.420 | 1435.460 | 75.000 | 1.909 | 1.916 | 2226.241 | 523.120 |
| 18 | 3071.024 | 3217.747 | 998.670 | 962.766 | 73.500 | 3.075 | 3.190 | 2829.335 | 241.112 |
| 20 | 4406.805 | 6114.625 | 2450.400 | 4451.702 | 73.000 | 1.798 | 0.990 | 3604.370 | 801.776 |
| 22 | 4729.460 | 4883.529 | 1546.415 | 1507.105 | 72.000 | 3.058 | 3.138 | 4438.565 | 290.239 |
| 24 | 6193.589 | 8221.219 | 3438.781 | 3052.861 | 71.000 | 1.801 | 2.029 | 5244.512 | 948.352 |
