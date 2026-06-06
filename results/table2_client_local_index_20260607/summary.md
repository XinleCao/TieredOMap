# Client-Local Index FO Results

Source baseline: `results/table2_client_fo_20260606_212627/client_fo.csv`

Generated files:
- `client_local_index_fo.csv`: Table-2-style performance rows.
- `client_local_index_storage.csv`: client storage composition.

Model assumptions:
- The hot index is stored directly by the client as `key -> data_block_ref`.
- There is no remote hot-index OMAP.
- The cold split-upper index is stored directly by the client as routing/index nodes.
- There is no remote cold split-upper ORAM entity.
- Every query still issues two data-ORAM paths plus a fixed cold-index lower template.
- The server therefore sees a fixed full-oblivious shape; it does not learn whether the key is hot.
- For `DaBplus`, the current implementation has no split-upper cold index, so only the hot index is local.

Key response-time speedups over the fair baseline:

| Backend | 2^16 | 2^20 | 2^24 |
|---|---:|---:|---:|
| AVL | 3.40x | 2.35x | 1.94x |
| BPlus | 3.44x | 2.42x | 2.00x |
| DaBplus | 2.84x | 2.04x | 1.73x |

Client storage:

| Backend | 2^16 | 2^20 | 2^24 |
|---|---:|---:|---:|
| AVL | 35.97 KB | 35.97 KB | 35.97 KB |
| BPlus | 9.96 KB | 11.97 KB | 9.02 KB |
| DaBplus | 8.00 KB | 8.00 KB | 8.00 KB |

At `N=2^24`, this is far below a full 4-byte-per-entry position map:
- AVL: 0.0549% of a full position map.
- BPlus: 0.0138% of a full position map.
- DaBplus: 0.0122% of a full position map.
