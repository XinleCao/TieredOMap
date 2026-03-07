#!/usr/bin/env python3
"""Generate pgfplots coordinate strings from experiment CSV files."""

import csv
import os
import sys

RESULTS_DIR = os.path.join(os.path.dirname(__file__), "..", "results")


def read_csv(name):
    path = os.path.join(RESULTS_DIR, name)
    if not os.path.exists(path):
        print(f"  [skip] {name} not found")
        return []
    with open(path) as f:
        return list(csv.DictReader(f))


def pgf_coords(pairs):
    return " ".join(f"({x}, {y})" for x, y in pairs)


def fig_bandwidth():
    """Fig 1: Expected bandwidth vs N."""
    rows = read_csv("bandwidth_vs_N.csv")
    if not rows:
        return
    print("% --- Fig 1: Bandwidth vs N ---")

    import math
    bl = [(int(math.log2(int(r["N"]))), int(r["baseline_bw"]) // 1024) for r in rows]
    fo_ns = [(int(math.log2(int(r["N"]))), int(r["fo_nosplit_bw"]) // 1024) for r in rows]
    fo = [(int(math.log2(int(r["N"]))), int(r["fo_bw"]) // 1024) for r in rows]

    print(f"% Single OMAP: \\addplot coordinates {{ {pgf_coords(bl)} }};")
    print(f"% TieredOMap (no split): \\addplot coordinates {{ {pgf_coords(fo_ns)} }};")
    print(f"% TieredOMap (full): \\addplot coordinates {{ {pgf_coords(fo)} }};")
    print()


def fig_skewness():
    """Fig 2: Speedup vs skewness."""
    rows = read_csv("skewness.csv")
    if not rows:
        return
    print("% --- Fig 2: Skewness ---")
    speedup = [(float(r["s"]), float(r["speedup_bw"])) for r in rows]
    hit = [(float(r["s"]), float(r["hit_pct"])) for r in rows]
    print(f"% Speedup: \\addplot coordinates {{ {pgf_coords(speedup)} }};")
    print(f"% Hit rate: \\addplot coordinates {{ {pgf_coords(hit)} }};")
    print()


def fig_hotsize():
    """Fig 3: Hot-set size."""
    rows = read_csv("hotsize.csv")
    if not rows:
        return
    print("% --- Fig 3: Hot-set size ---")
    import math
    tm = [(int(math.log2(int(r["n"]))), int(r["tm_bw"]) // 1024) for r in rows]
    print(f"% TieredOMap (full): \\addplot coordinates {{ {pgf_coords(tm)} }};")
    if rows:
        bl_bw = int(rows[0]["baseline_bw"]) // 1024
        print(f"% Single OMAP baseline: {bl_bw} KB (horizontal line)")
    print()


def tab_ablation():
    """Table 4: Split-ORAM ablation."""
    rows = read_csv("ablation_split.csv")
    if not rows:
        return
    print("% --- Table 4: Split-ORAM Ablation ---")
    for r in rows:
        ns = int(r["nosplit_bw"]) // 1024
        sp = int(r["split_bw"]) // 1024
        imp = float(r["improvement"])
        print(f"% N={r['N']:>8}  nosplit={ns}KB  split={sp}KB  {imp:.2f}x")
    print()


def tab_write():
    """Table 5: Read vs write."""
    rows = read_csv("read_vs_write.csv")
    if not rows:
        return
    print("% --- Table 5: Read vs Write ---")
    for r in rows:
        rb = int(r["read_bw"]) // 1024
        wb = int(r["write_bw"]) // 1024
        print(f"% {r['config']:>16}  read={rb}KB  write={wb}KB")
    print()


def tab_modes():
    """Table 3: Mode comparison."""
    rows = read_csv("modes.csv")
    if not rows:
        return
    print("% --- Table 3: Mode Comparison ---")
    for r in rows:
        bw = int(r["avg_bw"]) // 1024
        rnd = float(r["avg_rnd"])
        w80l = int(float(r["wan80_latency_ms"]))
        w80a = int(float(r["wan80_ans_ms"]))
        print(f"% {r['mode']:>16}  bw~{bw}KB  rnd={rnd:.1f}  "
              f"W80_lat~{w80l}ms  W80_ans~{w80a}ms")
    print()


if __name__ == "__main__":
    if len(sys.argv) > 1:
        RESULTS_DIR = sys.argv[1]
    print(f"% Reading from: {RESULTS_DIR}\n")
    fig_bandwidth()
    fig_skewness()
    fig_hotsize()
    tab_ablation()
    tab_write()
    tab_modes()
