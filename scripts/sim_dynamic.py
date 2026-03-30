#!/usr/bin/env python3
"""
Simulate TieredOMap's cache-based dynamic hot-set maintenance.

Implements the new mechanism (Algorithm 2):
  - Local cache B of fixed size, initialized from I_hot
  - Hot scan swap: periodic extract from I_hot[ptr], write back cache best
  - Cold promotion swap: reactive when cold key's f_prev > min(cache f_prev)
  - State machine: idle / hot-pend / cold-pend (one swap at a time)

Produces pgfplots coordinates for:
  EXP-3a/3b: Static distribution convergence (N=2^24, 2^20)
  EXP-4a/4b: Workload drift recovery (N=2^24, 2^20)
"""

import csv
import os
import sys
import time

import numpy as np


def zipf_probs(N, s):
    ranks = np.arange(1, N + 1, dtype=np.float64)
    weights = ranks ** (-s)
    return weights / weights.sum()


class ZipfSampler:
    def __init__(self, N, s, seed=42):
        self.cdf = np.cumsum(zipf_probs(N, s))
        self.rng = np.random.RandomState(seed)

    def sample_batch(self, count):
        return np.searchsorted(self.cdf, self.rng.random(count)).astype(int)


class CacheMaintenanceSimulator:
    """New cache-based maintenance with strict one-swap-at-a-time state machine."""

    def __init__(self, N, n, B, B_obs, B_swap):
        self.N = N
        self.n = n
        self.B = B
        self.B_obs = B_obs
        self.B_swap = B_swap

        self.cnt = np.zeros(N, dtype=np.int32)
        self.ep = np.full(N, -2, dtype=np.int32)
        self.f_prev = np.zeros(N, dtype=np.int32)

        initial_hot = list(range(n))
        self.cache = set(initial_hot[:B])
        self.I_hot = set(initial_hot[B:])
        self.I_cold = set(range(N)) - set(initial_hot)

        self.ptr = -1
        self.st = 'idle'
        self.t = 0

    def effective_hot_set(self):
        return self.I_hot | self.cache

    def _update_epoch(self, k):
        e = self.t // self.B_obs
        if self.ep[k] < e:
            self.f_prev[k] = self.cnt[k] if self.ep[k] == e - 1 else 0
            self.cnt[k] = 1
            self.ep[k] = e
        else:
            self.cnt[k] += 1

    def _cache_min_fprev(self):
        return min(self.f_prev[c] for c in self.cache)

    def _cache_max_fprev(self):
        return max(self.f_prev[c] for c in self.cache)

    def access(self, k):
        self.t += 1
        self._update_epoch(k)

        in_cold = k in self.I_cold

        # Staggered: each B_swap boundary does ONE task (completion OR scan)
        at_boundary = (self.t % self.B_swap == 0 and self.t > 0
                       and self.t // self.B_obs > 0)
        if at_boundary:
            if self.st == 'hot-pend':
                best = max(self.cache, key=lambda c: self.f_prev[c])
                self.cache.remove(best)
                self.I_hot.add(best)
                self.st = 'idle'
            elif self.st == 'cold-pend':
                worst = min(self.cache, key=lambda c: self.f_prev[c])
                self.cache.remove(worst)
                self.I_cold.add(worst)
                self.st = 'idle'
            elif len(self.I_hot) > 0:
                sorted_hot = sorted(self.I_hot)
                from bisect import bisect_right
                pos = bisect_right(sorted_hot, self.ptr)
                if pos >= len(sorted_hot):
                    self.ptr = -1
                    pos = 0
                scan_key = sorted_hot[pos]
                self.ptr = scan_key
                if self.f_prev[scan_key] < self._cache_max_fprev():
                    self.I_hot.remove(scan_key)
                    self.cache.add(scan_key)
                    self.st = 'hot-pend'

        # Reactive cold promotion
        if in_cold and self.st == 'idle' and self.t > self.B_obs:
            if self.f_prev[k] > self._cache_min_fprev():
                self.I_cold.remove(k)
                self.cache.add(k)
                self.st = 'cold-pend'


class NoMaintenanceSimulator:
    """Baseline: no maintenance, hot set never changes."""

    def __init__(self, N, n):
        self.hot_set = set(range(n))

    def effective_hot_set(self):
        return self.hot_set

    def access(self, k):
        pass


def hit_rate_pct(hot_set, probs):
    return sum(probs[k] for k in hot_set) * 100.0


def hit_rate_shifted(hot_set, probs_shifted):
    return sum(probs_shifted[k] for k in hot_set) * 100.0


# ── EXP-3: Static distribution convergence ─────────────────────────────

def run_convergence(N, n, s, B, B_obs_list, B_swap,
                    total_queries, report_every, seed=42):
    probs = zipf_probs(N, s)
    optimal = sum(probs[:n]) * 100.0
    results = {}

    for B_obs in B_obs_list:
        print(f"  B_obs={B_obs}...", end=" ", flush=True)
        t0 = time.time()

        sim = CacheMaintenanceSimulator(N, n, B, B_obs, B_swap)
        z = ZipfSampler(N, s, seed)
        samples = z.sample_batch(total_queries)

        hr0 = hit_rate_pct(sim.effective_hot_set(), probs)
        data = [(0.0, hr0)]

        for q in range(total_queries):
            sim.access(int(samples[q]))
            if (q + 1) % report_every == 0:
                hr = hit_rate_pct(sim.effective_hot_set(), probs)
                data.append(((q + 1) / 1000.0, hr))

        results[B_obs] = data
        dt = time.time() - t0
        print(f"done ({dt:.1f}s, final={data[-1][1]:.1f}%)")

    return results, optimal


# ── EXP-4: Workload drift recovery ─────────────────────────────────────

def run_drift(N, n, s, B, B_swap_list, B_obs_fixed,
              post_onset, report_every, seed=42):
    probs_orig = zipf_probs(N, s)
    shift = N // 2
    probs_shifted = np.roll(probs_orig, shift)
    optimal_shifted = sorted(probs_shifted, reverse=True)[:n]
    optimal_hr = sum(optimal_shifted) * 100.0

    results = {}

    configs = [(bsw, True) for bsw in B_swap_list] + [(0, False)]

    for B_swap, maint_on in configs:
        label = f"B_swap={B_swap}" if maint_on else "no_maint"
        print(f"  {label}...", end=" ", flush=True)
        t0 = time.time()

        if maint_on:
            sim = CacheMaintenanceSimulator(N, n, B, B_obs_fixed, B_swap)
        else:
            sim = NoMaintenanceSimulator(N, n)

        z = ZipfSampler(N, s, seed)
        total = B_obs_fixed + post_onset
        samples = z.sample_batch(total)

        data = []
        for q in range(total):
            key = (int(samples[q]) + shift) % N
            sim.access(key)
            from_onset = q - B_obs_fixed
            if from_onset >= 0 and from_onset % report_every == 0:
                hr = hit_rate_shifted(sim.effective_hot_set(), probs_shifted)
                data.append((from_onset / 1000.0, hr))

        results[label] = data
        dt = time.time() - t0
        final = data[-1][1] if data else 0
        print(f"done ({dt:.1f}s, final={final:.1f}%)")

    return results, optimal_hr


# ── pgfplots output ────────────────────────────────────────────────────

def fmt_coords(data, step=None):
    pts = data if step is None else data[::step]
    return " ".join(f"({x:.0f},{y:.1f})" for x, y in pts)


def print_convergence(results, optimal, tag=""):
    print(f"\n% === Convergence{tag} (optimal={optimal:.1f}%) ===")
    for B_obs in sorted(results.keys()):
        coords = fmt_coords(results[B_obs])
        print(f"% B_obs={B_obs}")
        print(f"\\addplot coordinates {{ {coords} }};")
    print(f"% optimal line: (0,{optimal:.0f}) (510,{optimal:.0f})")


def print_drift(results, tag=""):
    print(f"\n% === Drift{tag} ===")
    for label in results:
        coords = fmt_coords(results[label])
        print(f"% {label}")
        print(f"\\addplot coordinates {{ {coords} }};")


# ── Main ────────────────────────────────────────────────────────────────

def main():
    outdir = sys.argv[1] if len(sys.argv) > 1 else "results_paper"
    os.makedirs(outdir, exist_ok=True)

    n = 1024
    s = 1.0
    B = 8
    B_obs_list = [16384, 32768, 65536]
    B_swap_convergence = 32
    B_swap_drift_list = [8, 16, 32]
    B_obs_drift = 65536
    total_queries = 500_000
    report_convergence = 20_000
    post_onset = 500_000
    report_drift = 5_000

    for logN, tag in [(24, ""), (20, "_N20")]:
        N = 1 << logN

        print(f"\n{'='*60}")
        print(f"EXP-3{tag}: Convergence  N=2^{logN}, n={n}, B={B}")
        print(f"{'='*60}")
        conv, opt = run_convergence(
            N, n, s, B, B_obs_list, B_swap_convergence,
            total_queries, report_convergence)
        print_convergence(conv, opt, tag)

        print(f"\n{'='*60}")
        print(f"EXP-4{tag}: Drift  N=2^{logN}, n={n}, B={B}")
        print(f"{'='*60}")
        drift, opt_shifted = run_drift(
            N, n, s, B, B_swap_drift_list, B_obs_drift,
            post_onset, report_drift)
        print_drift(drift, tag)
        print(f"% shifted optimal: {opt_shifted:.1f}%")


if __name__ == "__main__":
    main()
