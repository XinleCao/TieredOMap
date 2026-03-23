#!/usr/bin/env python3
"""
Simulate TieredOMap's epoch-based hot-set maintenance (Algorithm 2).

Replicates the exact logic from:
  - maintenance.h::MaintenanceManager::on_access
  - maintenance.h::MaintenanceManager::should_demote
  - tiered_omap.cpp::TieredOMap::do_maintenance_step

Produces CSV data for:
  EXP-3a/3b: Static distribution convergence (N=2^24, 2^20)
  EXP-4a/4b: Workload drift recovery (N=2^24, 2^20)
"""

import bisect
import csv
import math
import os
import sys
import time
from collections import deque

import numpy as np


# ── Zipf utilities ──────────────────────────────────────────────────────

def zipf_probs(N, s):
    """Return array of Zipf probabilities for ranks 0..N-1 (0-indexed)."""
    ranks = np.arange(1, N + 1, dtype=np.float64)
    weights = ranks ** (-s)
    H = weights.sum()
    return weights / H


def zipf_cdf(N, s):
    """Return CDF for inverse-transform Zipf sampling."""
    probs = zipf_probs(N, s)
    return np.cumsum(probs)


class ZipfSampler:
    def __init__(self, N, s, seed=42):
        self.N = N
        self.cdf = zipf_cdf(N, s)
        self.rng = np.random.RandomState(seed)

    def sample(self):
        u = self.rng.random()
        return int(np.searchsorted(self.cdf, u))

    def sample_batch(self, count):
        us = self.rng.random(count)
        return np.searchsorted(self.cdf, us).astype(int)


# ── Maintenance simulator ──────────────────────────────────────────────

class MaintenanceSimulator:
    """Exact replica of MaintenanceManager + TieredOMap::do_maintenance_step."""

    def __init__(self, N, n, B_obs, B_swap,
                 promote_threshold=5, demote_threshold=2, staleness_windows=1):
        self.N = N
        self.n = n
        self.B_obs = B_obs
        self.B_swap = B_swap
        self.promote_threshold = promote_threshold
        self.demote_threshold = demote_threshold
        self.staleness_windows = staleness_windows

        self.hot_set = set(range(n))
        self.hot_key_list = list(range(n))

        self.key_meta = {}  # key -> (cnt, ep)
        self.total_accesses = 0
        self.obs_epoch = 0
        self.scan_ptr = 0
        self.promotion_queue = deque()

    def on_access(self, key):
        """Replicate MaintenanceManager::on_access from maintenance.h."""
        is_hot = key in self.hot_set
        cnt, ep = self.key_meta.get(key, (0, 0))

        epoch_changed = (ep < self.obs_epoch)
        if epoch_changed:
            prev_freq = cnt
            cnt = 1
            ep = self.obs_epoch
        else:
            cnt += 1
            prev_freq = 0

        self.key_meta[key] = (cnt, ep)

        if epoch_changed and not is_hot and prev_freq >= self.promote_threshold:
            self.promotion_queue.append(key)

        self.total_accesses += 1
        self.obs_epoch = self.total_accesses // self.B_obs

        if (self.total_accesses % self.B_swap == 0
                and self.total_accesses > 0
                and self.obs_epoch > 0):
            self._maintenance_step()

    def _should_demote(self, cnt, ep):
        """Replicate MaintenanceManager::should_demote.
        Only use the previous completed epoch's data for decisions.
        """
        if ep == self.obs_epoch:
            return False
        if ep == self.obs_epoch - 1:
            return cnt < self.demote_threshold
        return True

    def _maintenance_step(self):
        """Replicate TieredOMap::do_maintenance_step.

        Bug fix: the C++ code returns early when hot_key_list is empty,
        which prevents promotions from ever being processed after a full
        hot-set eviction (e.g., after an abrupt drift). We split the
        demotion scan (requires non-empty list) from the promotion
        processing (should always run).
        """
        if self.hot_key_list:
            idx = self.scan_ptr % len(self.hot_key_list)
            scan_key = self.hot_key_list[idx]

            cnt, ep = self.key_meta.get(scan_key, (0, 0))
            demote = self._should_demote(cnt, ep)

            if demote:
                self.hot_set.discard(scan_key)
                try:
                    self.hot_key_list.remove(scan_key)
                except ValueError:
                    pass

            if self.hot_key_list:
                self.scan_ptr = (self.scan_ptr + 1) % len(self.hot_key_list)
            else:
                self.scan_ptr = 0

        promo_key = None
        while self.promotion_queue:
            candidate = self.promotion_queue.popleft()
            if candidate not in self.hot_set and len(self.hot_set) < self.n:
                promo_key = candidate
                break

        if promo_key is not None:
            self.hot_set.add(promo_key)
            bisect.insort(self.hot_key_list, promo_key)


# ── Hit rate computation ────────────────────────────────────────────────

def theoretical_hit_rate(hot_set, probs):
    """Sum of Zipf probabilities for keys in hot_set."""
    return sum(probs[k] for k in hot_set) * 100.0


def theoretical_hit_rate_shifted(hot_set, N, s, H, shift_offset):
    """Hit rate under shifted distribution: key k has rank (k - shift) % N."""
    total = 0.0
    for k in hot_set:
        rank0 = (k - shift_offset % N + N) % N
        total += (rank0 + 1) ** (-s)
    return 100.0 * total / H


# ── EXP-3: Static distribution convergence ─────────────────────────────

def run_convergence(N, n, s, B_obs_list, B_swap, total_queries,
                    report_every, seed=42):
    """Run EXP-3: static Zipf, measure hit rate over time."""
    probs = zipf_probs(N, s)
    optimal = sum(probs[i] for i in range(n)) * 100.0

    results = {}  # B_obs -> [(queries_K, hit_rate)]

    for B_obs in B_obs_list:
        print(f"  B_obs={B_obs}...", end=" ", flush=True)
        t0 = time.time()

        sim = MaintenanceSimulator(N, n, B_obs, B_swap)
        z = ZipfSampler(N, s, seed)

        hr0 = theoretical_hit_rate(sim.hot_set, probs)
        data = [(0.0, hr0)]

        samples = z.sample_batch(total_queries)
        for q in range(total_queries):
            sim.on_access(int(samples[q]))
            if (q + 1) % report_every == 0:
                hr = theoretical_hit_rate(sim.hot_set, probs)
                data.append(((q + 1) / 1000.0, hr))

        results[B_obs] = data
        elapsed = time.time() - t0
        final_hr = data[-1][1]
        print(f"done ({elapsed:.1f}s, final={final_hr:.2f}%)")

    return results, optimal


def run_drift(N, n, s, B_swap_list, B_obs_fixed, shift_offset,
              post_onset, report_every, seed=42):
    """Run EXP-4: shifted Zipf, measure recovery after onset."""
    H = sum((i + 1) ** (-s) for i in range(N))

    results = {}  # B_swap (or 'no_maint') -> [(onset_K, hit_rate)]

    configs = [(bsw, True) for bsw in B_swap_list] + [(0, False)]

    for B_swap, maint_on in configs:
        label = f"B_swap={B_swap}" if maint_on else "no_maint"
        print(f"  {label}...", end=" ", flush=True)
        t0 = time.time()

        if maint_on:
            sim = MaintenanceSimulator(N, n, B_obs_fixed, B_swap)
        else:
            sim = MaintenanceSimulator(N, n, B_obs_fixed, 1)
            sim.B_swap = 999999999

        z = ZipfSampler(N, s, seed)
        total = B_obs_fixed + post_onset

        data = []
        samples = z.sample_batch(total)
        for q in range(total):
            key = (int(samples[q]) + shift_offset) % N
            if maint_on:
                sim.on_access(key)
            else:
                sim.total_accesses += 1
                sim.obs_epoch = sim.total_accesses // B_obs_fixed

            from_onset = q - B_obs_fixed
            if from_onset >= 0 and from_onset % report_every == 0:
                hr = theoretical_hit_rate_shifted(
                    sim.hot_set, N, s, H, shift_offset)
                data.append((from_onset / 1000.0, hr))

        key_label = str(B_swap) if maint_on else "no_maint"
        results[key_label] = data
        elapsed = time.time() - t0
        final_hr = data[-1][1] if data else 0
        print(f"done ({elapsed:.1f}s, final={final_hr:.2f}%)")

    return results


# ── CSV output ──────────────────────────────────────────────────────────

def write_convergence_csv(path, results, optimal):
    with open(path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["queries_K", "B_obs", "hit_rate_pct"])
        for B_obs, data in sorted(results.items()):
            for qk, hr in data:
                w.writerow([f"{qk:.1f}", B_obs, f"{hr:.2f}"])
        w.writerow([f"# optimal", "", f"{optimal:.2f}"])
    print(f"  -> {path}")


def write_drift_csv(path, results):
    with open(path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["onset_queries_K", "B_swap", "hit_rate_pct"])
        for label, data in results.items():
            for qk, hr in data:
                w.writerow([f"{qk:.1f}", label, f"{hr:.2f}"])
    print(f"  -> {path}")


# ── Main ────────────────────────────────────────────────────────────────

def main():
    outdir = sys.argv[1] if len(sys.argv) > 1 else "results_paper"
    os.makedirs(outdir, exist_ok=True)

    n = 1024
    s = 1.0
    B_obs_list = [16384, 32768, 65536]
    B_swap_convergence = 32
    B_swap_drift_list = [16, 32, 64]
    B_obs_drift = 65536
    total_queries = 500000
    report_every = 2000
    post_onset = 150000

    for logN, tag in [(24, ""), (20, "_N20")]:
        N = 1 << logN
        shift_offset = N // 2

        print(f"\n=== EXP-3: Convergence N=2^{logN} ===")
        conv_results, optimal = run_convergence(
            N, n, s, B_obs_list, B_swap_convergence,
            total_queries, report_every)
        write_convergence_csv(
            os.path.join(outdir, f"dynamic{tag}.csv"), conv_results, optimal)
        print(f"  Static optimal: {optimal:.2f}%")

        print(f"\n=== EXP-4: Drift N=2^{logN} ===")
        drift_results = run_drift(
            N, n, s, B_swap_drift_list, B_obs_drift,
            shift_offset, post_onset, report_every)
        write_drift_csv(
            os.path.join(outdir, f"drift{tag}.csv"), drift_results)


if __name__ == "__main__":
    main()
