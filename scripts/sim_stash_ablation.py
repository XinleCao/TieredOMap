#!/usr/bin/env python3
"""Simulate FO-safe early answers from ordinary Path-ORAM stashes.

This is an ablation for the client state that Path ORAM already keeps to avoid
overflow.  It is not an LRU/value cache.  Each query observes the local data and
index stashes before the fixed server-visible template is executed:

    data pre-access + full index access + second data access

If the data stash contains the value, the client can answer at local time 0.
If only the index stash contains the position metadata, the first data access is
real and the client can answer after that path.  Otherwise the first data access
is dummy and the client answers after the full template.
"""

import argparse
import csv
import math
from pathlib import Path

import numpy as np


class PathORAMSim:
    def __init__(self, n, bucket_size=4, seed=1):
        self.n = n
        self.bucket_size = bucket_size
        self.level = int(math.ceil(math.log2(max(n, 1)))) + 1
        self.leaf_range = 1 << (self.level - 1)
        self.rng = np.random.default_rng(seed)
        self.pos = self.rng.integers(0, self.leaf_range, size=n, dtype=np.int32)
        self.tree = {}
        self.stash = []
        self.stash_set = set()
        self._bulk_load()

    def contains_in_stash(self, key):
        return key in self.stash_set

    def random_leaf(self):
        return int(self.rng.integers(0, self.leaf_range))

    def _path_nodes(self, leaf):
        node = self.leaf_range - 1 + int(leaf)
        path = []
        while True:
            path.append(node)
            if node == 0:
                break
            node = (node - 1) // 2
        path.reverse()
        return path

    def _is_ancestor(self, node, leaf):
        cur = self.leaf_range - 1 + int(leaf)
        while True:
            if cur == node:
                return True
            if cur == 0:
                return False
            cur = (cur - 1) // 2

    def _add_to_stash(self, key):
        if key not in self.stash_set:
            self.stash.append(key)
            self.stash_set.add(key)

    def _remove_from_stash_at(self, idx):
        key = self.stash[idx]
        last = self.stash.pop()
        if idx < len(self.stash):
            self.stash[idx] = last
        self.stash_set.remove(key)
        return key

    def _bulk_load(self):
        for key in range(self.n):
            placed = False
            for node in reversed(self._path_nodes(self.pos[key])):
                bucket = self.tree.setdefault(node, [])
                if len(bucket) < self.bucket_size:
                    bucket.append(key)
                    placed = True
                    break
            if not placed:
                self._add_to_stash(key)

    def _read_path_to_stash(self, leaf):
        for node in self._path_nodes(leaf):
            bucket = self.tree.get(node)
            if not bucket:
                continue
            for key in bucket:
                self._add_to_stash(key)
            bucket.clear()

    def _evict_to_path(self, leaf):
        path = self._path_nodes(leaf)
        for node in reversed(path):
            bucket = self.tree.setdefault(node, [])
            idx = 0
            while len(bucket) < self.bucket_size and idx < len(self.stash):
                key = self.stash[idx]
                if self._is_ancestor(node, self.pos[key]):
                    placed_key = self._remove_from_stash_at(idx)
                    bucket.append(placed_key)
                else:
                    idx += 1

    def access(self, key):
        old_leaf = int(self.pos[key])
        new_leaf = self.random_leaf()
        self.pos[key] = new_leaf
        self._read_path_to_stash(old_leaf)
        if key not in self.stash_set:
            raise RuntimeError(f"key {key} missing after reading its path")
        self._evict_to_path(old_leaf)

    def dummy_access(self):
        leaf = self.random_leaf()
        self._read_path_to_stash(leaf)
        self._evict_to_path(leaf)

    def stash_size(self):
        return len(self.stash)


def zipf_cdf(n, alpha):
    ranks = np.arange(1, n + 1, dtype=np.float64)
    weights = ranks ** (-alpha)
    return np.cumsum(weights / weights.sum())


def top_mass(n, hot_n, alpha):
    ranks = np.arange(1, n + 1, dtype=np.float64)
    weights = ranks ** (-alpha)
    return float(weights[:hot_n].sum() / weights.sum())


def sample_zipf(cdf, rng, count, batch):
    left = count
    while left > 0:
        take = min(left, batch)
        yield np.searchsorted(cdf, rng.random(take), side="left")
        left -= take


def execute_template(index_oram, data_oram, key, data_path_rounds,
                     tail_rounds):
    data_hit = data_oram.contains_in_stash(key)
    index_hit = index_oram.contains_in_stash(key)
    baseline_answer_rounds = data_path_rounds + tail_rounds

    if data_hit:
        data_oram.dummy_access()
        index_oram.access(key)
        data_oram.access(key)
        return "data", 0.0
    if index_hit:
        data_oram.access(key)
        index_oram.access(key)
        data_oram.dummy_access()
        return "index", data_path_rounds

    data_oram.dummy_access()
    index_oram.access(key)
    data_oram.access(key)
    return "miss", baseline_answer_rounds


def run_one(n, alpha, queries, batch, seed, hot_n, bucket_size, warmup,
            data_path_rounds, tail_rounds, key_bytes, path_bytes,
            metadata_bytes, value_bytes):
    index_oram = PathORAMSim(n, bucket_size=bucket_size, seed=seed + 1)
    data_oram = PathORAMSim(n, bucket_size=bucket_size, seed=seed + 2)
    trace_rng = np.random.default_rng(seed + 3)
    cdf = zipf_cdf(n, alpha)
    baseline_answer_rounds = data_path_rounds + tail_rounds
    index_stash_entry_bytes = key_bytes + path_bytes + metadata_bytes
    data_stash_entry_bytes = key_bytes + path_bytes + metadata_bytes + value_bytes

    if warmup > 0:
        for keys in sample_zipf(cdf, trace_rng, warmup, batch):
            for key_np in keys:
                execute_template(index_oram, data_oram, int(key_np),
                                 data_path_rounds, tail_rounds)

    data_hits = 0
    index_only_hits = 0
    misses = 0
    total_answer_rounds = 0.0
    total_index_stash = 0
    total_data_stash = 0
    max_index_stash = 0
    max_data_stash = 0

    for keys in sample_zipf(cdf, trace_rng, queries, batch):
        for key_np in keys:
            key = int(key_np)
            hit_kind, answer_rounds = execute_template(
                index_oram, data_oram, key, data_path_rounds, tail_rounds)
            if hit_kind == "data":
                data_hits += 1
            elif hit_kind == "index":
                index_only_hits += 1
            else:
                misses += 1

            index_stash_size = index_oram.stash_size()
            data_stash_size = data_oram.stash_size()
            total_answer_rounds += answer_rounds
            total_index_stash += index_stash_size
            total_data_stash += data_stash_size
            max_index_stash = max(max_index_stash, index_stash_size)
            max_data_stash = max(max_data_stash, data_stash_size)

    early_hits = data_hits + index_only_hits
    mean_answer_rounds = total_answer_rounds / queries
    reduction = ((baseline_answer_rounds - mean_answer_rounds)
                 / baseline_answer_rounds)
    mean_data_stash_size = total_data_stash / queries
    mean_index_stash_size = total_index_stash / queries
    return {
        "N": n,
        "logN": int(math.log2(n)),
        "alpha": alpha,
        "Q": queries,
        "warmup": warmup,
        "bucket_size": bucket_size,
        "hot_n": hot_n,
        "data_stash_hit_pct": 100.0 * data_hits / queries,
        "index_only_stash_hit_pct": 100.0 * index_only_hits / queries,
        "early_stash_hit_pct": 100.0 * early_hits / queries,
        "miss_pct": 100.0 * misses / queries,
        "top_hot_index_hit_pct": 100.0 * top_mass(n, hot_n, alpha),
        "mean_data_stash_size": mean_data_stash_size,
        "mean_index_stash_size": mean_index_stash_size,
        "max_data_stash_size": max_data_stash,
        "max_index_stash_size": max_index_stash,
        "data_stash_entry_bytes": data_stash_entry_bytes,
        "index_stash_entry_bytes": index_stash_entry_bytes,
        "mean_data_stash_kib": mean_data_stash_size * data_stash_entry_bytes / 1024.0,
        "mean_index_stash_kib": mean_index_stash_size * index_stash_entry_bytes / 1024.0,
        "max_data_stash_kib": max_data_stash * data_stash_entry_bytes / 1024.0,
        "max_index_stash_kib": max_index_stash * index_stash_entry_bytes / 1024.0,
        "data_path_rounds": data_path_rounds,
        "tail_rounds": tail_rounds,
        "baseline_answer_rounds": baseline_answer_rounds,
        "mean_answer_rounds": mean_answer_rounds,
        "answer_round_reduction_pct": 100.0 * reduction,
        "round_reduction_pct": 100.0 * reduction,
    }


def fmt(row):
    return (
        f"N=2^{row['logN']} s={row['alpha']:.1f} | "
        f"data-stash={row['data_stash_hit_pct']:5.2f}% "
        f"index-only={row['index_only_stash_hit_pct']:5.2f}% "
        f"early={row['early_stash_hit_pct']:5.2f}% "
        f"top-hot={row['top_hot_index_hit_pct']:5.2f}% | "
        f"stash(data/index)={row['mean_data_stash_size']:5.1f}/"
        f"{row['mean_index_stash_size']:5.1f} "
        f"mean-rnd={row['mean_answer_rounds']:5.1f}/"
        f"{row['baseline_answer_rounds']:4.1f} "
        f"red={row['answer_round_reduction_pct']:6.2f}%"
    )


def write_summary(rows, path, args):
    if not rows:
        return
    out = Path(path)
    out.parent.mkdir(parents=True, exist_ok=True)
    max_early = max(row["early_stash_hit_pct"] for row in rows)
    max_top_hot = max(row["top_hot_index_hit_pct"] for row in rows)
    with out.open("w") as f:
        f.write("# Stash-Only Client-State Ablation\n\n")
        f.write("This experiment tests whether ordinary Path-ORAM stashes can "
                "replace a dedicated local hot index. The server-visible FO "
                "template is fixed for every query.\n\n")
        f.write("Template:\n\n")
        f.write("```text\n")
        f.write("first data-ORAM path + full index OMAP lookup/update + "
                "second data-ORAM path\n")
        f.write("```\n\n")
        f.write("A data-stash hit can answer locally at round 0. An index-only "
                "stash hit can answer after the first data path. A miss answers "
                "after the full template.\n\n")
        f.write("## Parameters\n\n")
        f.write(f"- logNs: `{args.logNs}`\n")
        f.write(f"- alphas: `{args.alphas}`\n")
        f.write(f"- Q: `{args.Q}`\n")
        f.write(f"- warmup: `{args.warmup}`\n")
        f.write(f"- hot_n reference: `{args.hot_n}`\n")
        f.write(f"- bucket size: `{args.bucket_size}`\n")
        f.write(f"- value bytes: `{args.value_bytes}`\n")
        f.write(f"- baseline answer rounds: "
                f"`{rows[0]['baseline_answer_rounds']:.1f}`\n\n")
        f.write("## Result Table\n\n")
        f.write("| logN | alpha | data stash hit % | index-only hit % | "
                "early hit % | top-hot mass % | mean stash data/index | "
                "max stash data/index | mean answer rounds | reduction % |\n")
        f.write("|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|\n")
        for row in rows:
            f.write(
                f"| {row['logN']} | {row['alpha']:.1f} | "
                f"{row['data_stash_hit_pct']:.4f} | "
                f"{row['index_only_stash_hit_pct']:.4f} | "
                f"{row['early_stash_hit_pct']:.4f} | "
                f"{row['top_hot_index_hit_pct']:.2f} | "
                f"{row['mean_data_stash_size']:.3f}/"
                f"{row['mean_index_stash_size']:.3f} | "
                f"{row['max_data_stash_size']}/"
                f"{row['max_index_stash_size']} | "
                f"{row['mean_answer_rounds']:.3f} | "
                f"{row['answer_round_reduction_pct']:.4f} |\n")
        f.write("\n## Interpretation\n\n")
        f.write(f"- The largest ordinary-stash early hit rate in this run is "
                f"`{max_early:.4f}%`, while the corresponding top-hot mass can "
                f"reach `{max_top_hot:.2f}%`.\n")
        f.write("- This supports using the experiment as an appendix ablation: "
                "ordinary ORAM stash state is governed by eviction pressure, "
                "not workload popularity, so it does not make the dedicated hot "
                "index redundant.\n")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--logNs", default="18,20")
    parser.add_argument("--alphas", default="0.8,1.0,1.2")
    parser.add_argument("--Q", type=int, default=100_000)
    parser.add_argument("--hot-n", type=int, default=1024)
    parser.add_argument("--bucket-size", type=int, default=4)
    parser.add_argument("--warmup", type=int, default=0)
    parser.add_argument("--data-path-rounds", type=float, default=16.0)
    parser.add_argument("--tail-rounds", type=float, default=37.0)
    parser.add_argument("--full-rounds", type=float, default=None,
                        help="Deprecated alias for --tail-rounds.")
    parser.add_argument("--key-bytes", type=int, default=16)
    parser.add_argument("--path-bytes", type=int, default=4)
    parser.add_argument("--metadata-bytes", type=int, default=16)
    parser.add_argument("--value-bytes", type=int, default=4096)
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--batch", type=int, default=100_000)
    parser.add_argument("--out", default="")
    parser.add_argument("--summary", default="")
    args = parser.parse_args()
    if args.full_rounds is not None:
        args.tail_rounds = args.full_rounds
    if args.Q <= 0:
        raise ValueError("--Q must be positive")
    if args.warmup < 0:
        raise ValueError("--warmup must be non-negative")

    rows = []
    for logn_s in args.logNs.split(","):
        n = 1 << int(logn_s)
        for alpha_s in args.alphas.split(","):
            row = run_one(
                n, float(alpha_s), args.Q, args.batch, args.seed, args.hot_n,
                args.bucket_size, args.warmup, args.data_path_rounds,
                args.tail_rounds, args.key_bytes, args.path_bytes,
                args.metadata_bytes, args.value_bytes)
            rows.append(row)
            print(fmt(row))

    if args.out and rows:
        out = Path(args.out)
        out.parent.mkdir(parents=True, exist_ok=True)
        with out.open("w", newline="") as f:
            writer = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
            writer.writeheader()
            writer.writerows(rows)
        print(f"\nwrote {out}")
    if args.summary:
        write_summary(rows, args.summary, args)
        print(f"wrote {args.summary}")


if __name__ == "__main__":
    main()
