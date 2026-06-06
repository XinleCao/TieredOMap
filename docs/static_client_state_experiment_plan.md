# Static Client-State Experiments

This note fixes the experiment design for the static client/server setting.
All variants preserve full obliviousness at the server-visible protocol level:
the server observes a fixed access template independent of whether the query is
answered early.

## Variants

### 1. Remote Hot Index

This is the current main static client/server experiment. The hot index is an
OMAP stored at the server. A query executes the complete hot-side and cold-side
templates. The client may obtain a hot answer after the hot-side traversal, but
the complete FO template is still executed.

Metrics:
- full-template bandwidth
- hot/cold/mean answer rounds
- hot/cold/mean WAN response time
- observed and theoretical hot-hit rate

### 2. Local Hot Index

The client stores a small hot-key routing table:

```text
key -> current data-ORAM path label
```

The client does not store hot values and does not store the full data position
map. The server stores a full index OMAP over all keys and a full data ORAM over
all payloads.

Every query executes the fixed FO template:

```text
first data-ORAM path
+ full index OMAP lookup/update
+ second data-ORAM path
```

Case handling:
- Local hot-index hit:
  - first data path is real
  - client obtains the value after the first data path
  - data ORAM remaps the block to a new path
  - full index OMAP updates the key to the new data path
  - second data path is dummy
- Local hot-index miss:
  - first data path is dummy
  - full index OMAP returns the current data path
  - second data path is real
  - client obtains the value after the complete template

Metrics:
- local hot-index storage: `n * (key bytes + path-label bytes + metadata bytes)`
- reference full-position-map storage: `N * path-label bytes`
- hit rate of local hot index
- hot/local-hit answer rounds and response time
- miss answer rounds and response time
- mean answer rounds and response time
- full-template bandwidth

Recommended sweep:
- `N in {2^16, 2^20, 2^24}`
- `n in {2^8, 2^10, 2^12}` for storage/performance sensitivity
- `s in {0.8, 1.0, 1.2}` for skewness sensitivity
- default value size 256 B, bucket size `Z=4`

Main expected claim to test:
local hot index adds only kilobytes to low megabytes of client state, far less
than an `O(N)` position map, while letting hot queries answer after one data
path under the same FO server-visible template.

### 3. Stash-Only Ablation

This ablation checks whether the ordinary ORAM stash already provides enough
local state to replace the explicit hot index. The stash is the overflow buffer
required by Path ORAM eviction; it is not a popularity-aware cache.

Every query uses the same template as the local-hot-index variant:

```text
first data-ORAM path
+ full index OMAP lookup/update
+ second data-ORAM path
```

Case handling:
- Data-stash hit:
  - requested value is already in the data ORAM stash
  - answer latency is 0
  - first data path is dummy
  - full template still executes for obliviousness and state refresh
- Index-stash-only hit:
  - requested path metadata is in the index stash, but value is not in data stash
  - first data path is real
  - client obtains the value after the first data path
  - data path metadata must be updated after remapping
- Miss:
  - first data path is dummy
  - full index lookup and second data path retrieve the value

Metrics:
- data-stash hit rate
- index-stash-only hit rate
- total early-answer hit rate
- mean data/index stash occupancy
- mean and max data/index stash occupancy
- mean answer rounds and response time, normalized against the full answer
  template
- full-template bandwidth

Runner:

```bash
bash scripts/run_stash_ablation_experiments.sh
```

The runner writes `stash_ablation.csv` and `summary.md`. The reduction metric is
computed against the no-early-answer baseline:

```text
first data-ORAM path + full index OMAP lookup/update + second data-ORAM path
```

Recommended placement:
report as an appendix ablation. Its role is not to compete with the local hot
index, but to justify why ordinary ORAM protocol state does not make the
dedicated hot index redundant.

## Reporting Layout

Main text:
- current remote-hot-index FO table
- local-hot-index storage/performance table
- one storage comparison figure or table: local hot index vs full position map

Appendix:
- stash-only hit-rate table
- stash occupancy statistics
- short explanation that stash behavior is governed by eviction pressure rather
  than workload popularity
