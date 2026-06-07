#!/usr/bin/env python3
"""
Compute skewness / hot-set-size reduction ratios analytically.

Input:  profile.csv  from  bench_paper --exp=profile  (WAN profiling).
        Columns: backend,logN,log_n,base_rnd,base_ms,
                 hot_rnd,hot_ms,cold_rnd,cold_ms,
                 base_bw_KB,hot_bw_KB,cold_bw_KB

Output: pgfplots-ready coordinates for implementation.tex figures.
"""

import csv, sys, math
from pathlib import Path
import numpy as np


def zipf_hit_rate(s: float, n: int, N: int) -> float:
    ranks = np.arange(1, N + 1, dtype=np.float64)
    probs = ranks ** (-s)
    total = probs.sum()
    return probs[:n].sum() / total


def load_profile(path: str) -> dict:
    """Return {(backend, log_n): row_dict} plus {backend: baseline_dict}."""
    rows = {}
    baselines = {}
    with open(path) as f:
        for r in csv.DictReader(f):
            be = r["backend"]
            log_n = int(r["log_n"])
            key = (be, log_n)
            rows[key] = {k: float(v) for k, v in r.items()
                         if k not in ("backend",)}
            rows[key]["backend"] = be
            if be not in baselines:
                baselines[be] = {
                    "base_rnd": float(r["base_rnd"]),
                    "base_ms": float(r["base_ms"]),
                    "base_bw_KB": float(r["base_bw_KB"]),
                }
    return rows, baselines


def fmt_coords(points: list[tuple[float, float]]) -> str:
    return " ".join(f"({x:.1f}, {y:.1f})" if isinstance(x, float)
                    else f"({x}, {y:.1f})" for x, y in points)


def main():
    if len(sys.argv) < 2:
        print("Usage: python compute_skewness_hotsize.py <profile.csv>")
        sys.exit(1)
    profile_path = sys.argv[1]
    rows, baselines = load_profile(profile_path)

    N = None
    for r in rows.values():
        N = int(2 ** r["logN"])
        break
    assert N is not None

    print(f"N = {N}  (logN = {int(math.log2(N))})")
    print(f"Baselines: {baselines}\n")
    print("=" * 70)

    s_vals = [0.5, 0.7, 0.9, 1.0, 1.1, 1.3, 1.5]
    skew_n_logs = [10, 12, 14]
    hot_n_logs = [6, 8, 10, 12, 14, 16]

    be = "AVL"
    # ── (a) Average round reduction vs skewness ──
    print("\n=== Fig (a): Avg round reduction vs skewness (AVL) ===")
    for log_n in skew_n_logs:
        n = 1 << log_n
        r = rows.get((be, log_n))
        if r is None:
            print(f"  n=2^{log_n}: MISSING DATA")
            continue
        base_rnd = r["base_rnd"]
        hot_rnd = r["hot_rnd"]
        cold_rnd = r["cold_rnd"]
        pts = []
        for s in s_vals:
            h = zipf_hit_rate(s, n, N)
            avg = h * hot_rnd + (1 - h) * cold_rnd
            red = (base_rnd - avg) / base_rnd * 100
            pts.append((s, red))
        print(f"  n=2^{log_n}: {fmt_coords(pts)}")

    # ── (a') Average ms reduction vs skewness ──
    print("\n=== Fig (a'): Avg ms reduction vs skewness (AVL) ===")
    for log_n in skew_n_logs:
        n = 1 << log_n
        r = rows.get((be, log_n))
        if r is None:
            continue
        base_ms = r["base_ms"]
        hot_ms = r["hot_ms"]
        cold_ms = r["cold_ms"]
        pts = []
        for s in s_vals:
            h = zipf_hit_rate(s, n, N)
            avg = h * hot_ms + (1 - h) * cold_ms
            red = (base_ms - avg) / base_ms * 100 if base_ms > 0 else 0
            pts.append((s, red))
        print(f"  n=2^{log_n}: {fmt_coords(pts)}")

    # ── (b) Hot-query round reduction vs skewness ──
    print("\n=== Fig (b): Hot-query round reduction vs skewness (AVL) ===")
    for log_n in skew_n_logs:
        r = rows.get((be, log_n))
        if r is None:
            continue
        base_rnd = r["base_rnd"]
        hot_rnd = r["hot_rnd"]
        red = (base_rnd - hot_rnd) / base_rnd * 100
        pts = [(s, red) for s in s_vals]
        print(f"  n=2^{log_n}: {fmt_coords(pts)}")

    # ── (b') Hot-query ms reduction vs skewness ──
    print("\n=== Fig (b'): Hot-query ms reduction vs skewness (AVL) ===")
    for log_n in skew_n_logs:
        r = rows.get((be, log_n))
        if r is None:
            continue
        base_ms = r["base_ms"]
        hot_ms = r["hot_ms"]
        red = (base_ms - hot_ms) / base_ms * 100 if base_ms > 0 else 0
        pts = [(s, red) for s in s_vals]
        print(f"  n=2^{log_n}: {fmt_coords(pts)}")

    # ── (c) Average round reduction vs hot-set size ──
    print("\n=== Fig (c): Avg round reduction vs hot-set size (AVL) ===")
    hot_s_vals = [0.9, 1.0, 1.3]
    for s in hot_s_vals:
        pts = []
        for log_n in hot_n_logs:
            n = 1 << log_n
            r = rows.get((be, log_n))
            if r is None:
                continue
            base_rnd = r["base_rnd"]
            hot_rnd = r["hot_rnd"]
            cold_rnd = r["cold_rnd"]
            h = zipf_hit_rate(s, n, N)
            avg = h * hot_rnd + (1 - h) * cold_rnd
            red = (base_rnd - avg) / base_rnd * 100
            pts.append((log_n, red))
        print(f"  s={s}: {fmt_coords(pts)}")

    # ── (c') Average ms reduction vs hot-set size ──
    print("\n=== Fig (c'): Avg ms reduction vs hot-set size (AVL) ===")
    for s in hot_s_vals:
        pts = []
        for log_n in hot_n_logs:
            n = 1 << log_n
            r = rows.get((be, log_n))
            if r is None:
                continue
            base_ms = r["base_ms"]
            hot_ms = r["hot_ms"]
            cold_ms = r["cold_ms"]
            h = zipf_hit_rate(s, n, N)
            avg = h * hot_ms + (1 - h) * cold_ms
            red = (base_ms - avg) / base_ms * 100 if base_ms > 0 else 0
            pts.append((log_n, red))
        print(f"  s={s}: {fmt_coords(pts)}")

    # ── Appendix: BPlus and DaBplus ──
    for be_app in ["BPlus", "DaBplus"]:
        if be_app not in baselines:
            continue
        print(f"\n{'='*70}")
        print(f"=== Appendix: {be_app} ===")

        print(f"\n  -- Avg round reduction vs skewness --")
        for log_n in skew_n_logs:
            n = 1 << log_n
            r = rows.get((be_app, log_n))
            if r is None:
                continue
            base_rnd_app = r["base_rnd"]
            pts = []
            for s in s_vals:
                h = zipf_hit_rate(s, n, N)
                avg = h * r["hot_rnd"] + (1 - h) * r["cold_rnd"]
                red = (base_rnd_app - avg) / base_rnd_app * 100
                pts.append((s, red))
            print(f"    n=2^{log_n}: {fmt_coords(pts)}")

        print(f"\n  -- Avg ms reduction vs skewness --")
        for log_n in skew_n_logs:
            n = 1 << log_n
            r = rows.get((be_app, log_n))
            if r is None:
                continue
            base_ms_app = r["base_ms"]
            pts = []
            for s in s_vals:
                h = zipf_hit_rate(s, n, N)
                avg = h * r["hot_ms"] + (1 - h) * r["cold_ms"]
                red = (base_ms_app - avg) / base_ms_app * 100
                pts.append((s, red))
            print(f"    n=2^{log_n}: {fmt_coords(pts)}")

        print(f"\n  -- Hot-query round reduction vs skewness --")
        for log_n in skew_n_logs:
            r = rows.get((be_app, log_n))
            if r is None:
                continue
            base_rnd_app = r["base_rnd"]
            red = (base_rnd_app - r["hot_rnd"]) / base_rnd_app * 100
            pts = [(s, red) for s in s_vals]
            print(f"    n=2^{log_n}: {fmt_coords(pts)}")

    # ── Summary table: per-n metrics ──
    print(f"\n{'='*70}")
    print("=== Per-config metrics ===")
    print(f"{'backend':<10} {'log_n':>5} {'base_rnd':>9} {'hot_rnd':>8} "
          f"{'cold_rnd':>9} {'hot_red%':>8} {'base_ms':>8} {'hot_ms':>8} "
          f"{'cold_ms':>8}")
    for (be_k, log_n), r in sorted(rows.items()):
        h_red = (r["base_rnd"] - r["hot_rnd"]) / r["base_rnd"] * 100
        print(f"{be_k:<10} {log_n:>5} {r['base_rnd']:>9.1f} "
              f"{r['hot_rnd']:>8.1f} {r['cold_rnd']:>9.1f} {h_red:>8.1f} "
              f"{r['base_ms']:>8.1f} {r['hot_ms']:>8.1f} "
              f"{r['cold_ms']:>8.1f}")


if __name__ == "__main__":
    main()
