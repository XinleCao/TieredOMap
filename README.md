# TieredOMap

Exploiting access skewness for efficient oblivious key-value stores.

## Overview

TieredOMap partitions keys into independent **hot** and **cold** oblivious maps (OMAPs) and searches both in parallel on every query. It offers two operating modes:

- **Full Obliviousness** — identical security to a standard OMAP (zero additional leakage), yet the client receives hot-key answers after only O(log n) rounds instead of O(log N).
- **Tier-Membership Privacy** — reveals one bit (hot or cold) per query, reducing hot-key bandwidth to O(log²n + log N) via Split-ORAM.

The system is agnostic to the underlying OMAP construction: any OMAP (AVL-based, B⁺-tree-based, etc.) can be used as a drop-in component.

## Building

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

Requires C++17 and [Google Test](https://github.com/google/googletest) (installed via Homebrew or system package manager).

## Running Tests

```bash
cd build
ctest --output-on-failure
```

## Running Benchmarks

```bash
cd build
./bench_tiered_omap
```

## Project Structure

```
include/tiered_omap/
  common.h              # Types, bandwidth stats, utilities
  oram/
    path_oram.h         # Path ORAM with bandwidth tracking
    da_oram.h           # De-amortized ORAM (DAORAM)
    binary_tree_storage.h
  omap/
    omap_interface.h    # OMAP interface (incl. partial_dummy_access)
    avl_omap.h          # AVL OMAP with Split-ORAM support
    bplus_omap.h        # B+ tree OMAP
  tiered_omap.h         # TieredOMap: parallel traversal + two security modes

src/                    # Implementations
tests/                  # Google Test suites
benchmark/              # Benchmark driver
```

## Citation

Paper in preparation for VLDB 2027.

## License

This project is for academic research purposes.
