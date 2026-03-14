#include "tiered_omap/tee/enclave_oram.h"
#include <algorithm>
#include <stdexcept>

namespace tiered_omap {
namespace tee {

EnclaveOram::EnclaveOram(int num_data, int value_size, int bucket_size,
                         int stash_scale)
    : value_size_(value_size),
      bucket_size_(bucket_size) {
    level_ = std::max(1, ceil_log2(num_data));
    leaf_range_ = 1 << (level_ - 1);
    num_nodes_ = (1 << level_) - 1;
    stash_max_ = stash_scale * level_;

    tree_.resize(num_nodes_);
    for (auto& bucket : tree_) {
        bucket.blocks.resize(bucket_size_);
        for (auto& b : bucket.blocks)
            b.value.resize(value_size_, 0);
    }
}

std::unordered_map<int, int> EnclaveOram::init(
        const std::vector<std::pair<int, Bytes>>& data,
        const std::unordered_map<int, int>& preset_leaves) {
    stash_.clear();

    std::unordered_map<int, int> leaf_map;
    for (auto& [key, val] : data) {
        if (leaf_map.count(key)) continue;
        auto it = preset_leaves.find(key);
        leaf_map[key] = (it != preset_leaves.end())
                      ? it->second
                      : SecureRandom::rand_below(leaf_range_);
    }

    for (auto& [key, val] : data) {
        int leaf = leaf_map[key];
        Bytes padded = pad_bytes(val, value_size_);

        int node = leaf_to_node(leaf);
        bool placed = false;
        while (node >= 0) {
            auto& bucket = tree_[node];
            for (int j = 0; j < bucket_size_; ++j) {
                if (bucket.blocks[j].key == INVALID_KEY) {
                    bucket.blocks[j] = {key, leaf, padded};
                    placed = true;
                    break;
                }
            }
            if (placed) break;
            if (node == 0) break;
            node = parent(node);
        }
        if (!placed)
            stash_.push_back({key, leaf, padded});
    }

    pad_stash();
    return leaf_map;
}

// ── Doubly-oblivious access ─────────────────────────────────────────────────
// Unified code path for real and dummy access. The control flow is identical
// regardless of `real`, `key`, or whether the key is found.

Bytes EnclaveOram::access(int key, int old_leaf, int new_leaf,
                          const Bytes* new_value) {
    return access_or_dummy(key, old_leaf, new_leaf, true, new_value);
}

Bytes EnclaveOram::access_or_dummy(int key, int old_leaf, int new_leaf,
                                   bool real, const Bytes* new_value) {
    last_stats_.reset();
    begin_page_tracking();

    int dummy_leaf = random_leaf();
    int use_leaf = o_select_i(real ? 1 : 0, old_leaf, dummy_leaf);

    auto path_nodes = read_path(use_leaf);

    Bytes result(value_size_, 0);
    Bytes padded_new(value_size_, 0);
    if (new_value)
        padded_new = pad_bytes(*new_value, value_size_);

    int real_i = real ? 1 : 0;
    int has_new = (new_value != nullptr) ? 1 : 0;
    for (auto& sb : stash_) {
        int match = real_i & o_equal(sb.key, key);
        o_mov_bytes(match, result, sb.value);
        int do_write = match & has_new;
        o_mov_bytes(do_write, sb.value, padded_new);
        o_mov_i(match, sb.leaf, new_leaf);
    }

    evict_path(use_leaf, path_nodes);

    last_stats_.accesses = 1;
    last_stats_.pages_touched = end_page_tracking();
    last_stats_.node_accesses = node_access_count_;
    total_stats_.accesses += last_stats_.accesses;
    total_stats_.pages_touched += last_stats_.pages_touched;
    total_stats_.node_accesses += last_stats_.node_accesses;

    return result;
}

void EnclaveOram::dummy_access() {
    int rl = random_leaf();
    Bytes discard = access_or_dummy(INVALID_KEY, rl, rl, false, nullptr);
    (void)discard;
}

int EnclaveOram::random_leaf() const {
    return SecureRandom::rand_below(leaf_range_);
}

// ── Doubly-oblivious path read ──────────────────────────────────────────────
// Always reads exactly `level_` nodes (root to leaf). Every block in every
// bucket is unconditionally moved to stash (including dummies).

std::vector<int> EnclaveOram::read_path(int leaf) {
    std::vector<int> nodes;
    nodes.reserve(level_);

    int node = leaf_to_node(leaf);
    for (int d = 0; d < level_; ++d) {
        nodes.push_back(node);
        record_node_access(node);

        auto& bucket = tree_[node];
        for (int j = 0; j < bucket_size_; ++j) {
            // Unconditionally push every slot to stash — no branch on key.
            stash_.push_back(bucket.blocks[j]);
            bucket.blocks[j] = {};
            bucket.blocks[j].value.resize(value_size_, 0);
        }
        if (node == 0) break;
        node = parent(node);
    }
    return nodes;
}

// ── Doubly-oblivious eviction ───────────────────────────────────────────────

void EnclaveOram::pad_stash() {
    while (static_cast<int>(stash_.size()) < stash_max_) {
        TreeBlock dummy;
        dummy.value.resize(value_size_, 0);
        stash_.push_back(std::move(dummy));
    }
}

int EnclaveOram::stash_valid_count() const {
    int c = 0;
    for (auto& sb : stash_)
        c += 1 - o_equal(sb.key, INVALID_KEY);
    return c;
}

void EnclaveOram::evict_path(int /*leaf*/, const std::vector<int>& path_nodes) {
    // The stash may contain: stash_max_ base entries + level_*bucket_size_
    // from read_path + a small number from add_to_stash. Process all of them.
    int n = static_cast<int>(stash_.size());

    for (int pi = 0; pi < static_cast<int>(path_nodes.size()); ++pi) {
        int node = path_nodes[pi];
        record_node_access(node);

        auto& bucket = tree_[node];
        for (int j = 0; j < bucket_size_; ++j) {
            int filled_i = 0;
            for (int si = 0; si < n; ++si) {
                auto& sb = stash_[si];
                int is_real = 1 - o_equal(sb.key, INVALID_KEY);
                int on_path = path_contains_oblivious(sb.leaf, node) ? 1 : 0;
                int not_filled = 1 - filled_i;
                int candidate = is_real & on_path & not_filled;
                o_mov_i(candidate, bucket.blocks[j].key, sb.key);
                o_mov_i(candidate, bucket.blocks[j].leaf, sb.leaf);
                o_mov_bytes(candidate, bucket.blocks[j].value, sb.value);
                int inv = INVALID_KEY;
                o_mov_i(candidate, sb.key, inv);
                int one = 1;
                o_mov_i(candidate, filled_i, one);
            }
        }
    }

    // Oblivious compaction: bubble INVALID_KEY entries to end.
    for (int pass = 0; pass < n; ++pass) {
        for (int i = 0; i < n - 1; ++i) {
            int left_inv = o_equal(stash_[i].key, INVALID_KEY);
            int right_val = 1 - o_equal(stash_[i + 1].key, INVALID_KEY);
            int do_swap = left_inv & right_val;
            o_swap_i(do_swap, stash_[i].key, stash_[i + 1].key);
            o_swap_i(do_swap, stash_[i].leaf, stash_[i + 1].leaf);
            o_swap_bytes(do_swap, stash_[i].value, stash_[i + 1].value);
        }
    }

    // Count valid entries, shrink to stash_max_ (valid entries are at front).
    int valid = 0;
    for (auto& sb : stash_)
        valid += 1 - o_equal(sb.key, INVALID_KEY);
    stash_.resize(std::max(valid, stash_max_));
    pad_stash();

    if (valid > stash_max_)
        throw std::runtime_error(
            "EnclaveOram: stash overflow (" + std::to_string(valid) +
            " > " + std::to_string(stash_max_) + ")");
}

// ── Doubly-oblivious low-level interface ────────────────────────────────────

void EnclaveOram::read_path_to_stash(int leaf) {
    read_path(leaf);
}

Block EnclaveOram::extract_from_stash(int key) {
    Block result{};
    result.value.resize(value_size_, 0);
    for (auto& sb : stash_) {
        int key_match = o_equal(sb.key, key);
        int not_invalid = 1 - o_equal(sb.key, INVALID_KEY);
        int match = key_match & not_invalid;
        o_mov_i(match, result.key, sb.key);
        o_mov_i(match, result.leaf, sb.leaf);
        o_mov_bytes(match, result.value, sb.value);
        int inv = INVALID_KEY;
        o_mov_i(match, sb.key, inv);
    }
    return result;
}

void EnclaveOram::add_to_stash(int key, int leaf, const Bytes& value) {
    stash_.push_back({key, leaf, pad_bytes(value, value_size_)});
}

void EnclaveOram::evict_one_path(int leaf) {
    std::vector<int> path_nodes;
    path_nodes.reserve(level_);
    int node = leaf_to_node(leaf);
    for (int d = 0; d < level_; ++d) {
        path_nodes.push_back(node);
        if (node == 0) break;
        node = parent(node);
    }
    evict_path(leaf, path_nodes);
}

// ── Doubly-oblivious path geometry ──────────────────────────────────────────

int EnclaveOram::node_depth(int i) const {
    int d = 0;
    int n = i;
    while (n > 0) { n = parent(n); ++d; }
    return d;
}

bool EnclaveOram::path_contains(int leaf, int node) const {
    return path_contains_oblivious(leaf, node);
}

bool EnclaveOram::path_contains_oblivious(int leaf, int node) const {
    // Fixed-iteration loop: always walks exactly `level_` steps.
    int cur = leaf_to_node(leaf);
    int found = 0;
    for (int d = 0; d < level_; ++d) {
        int eq = o_equal(cur, node);
        found |= eq;
        // Move up unless already at root (cur stays 0).
        int par = (cur > 0) ? parent(cur) : 0;
        cur = par;
    }
    return found != 0;
}

// ── Page tracking ───────────────────────────────────────────────────────────

int EnclaveOram::node_byte_size() const {
    return bucket_size_ * (static_cast<int>(sizeof(int)) * 2 + value_size_);
}

void EnclaveOram::begin_page_tracking() {
    touched_pages_.clear();
    node_access_count_ = 0;
}

uint64_t EnclaveOram::end_page_tracking() {
    return touched_pages_.size();
}

void EnclaveOram::record_node_access(int node_idx) {
    ++node_access_count_;
    int bytes_per_node = node_byte_size();
    uint64_t byte_start = static_cast<uint64_t>(node_idx) * bytes_per_node;
    uint64_t byte_end = byte_start + bytes_per_node;
    uint64_t page_start = byte_start / PAGE_SIZE;
    uint64_t page_end = (byte_end - 1) / PAGE_SIZE;
    for (uint64_t p = page_start; p <= page_end; ++p)
        touched_pages_[p] = true;
}

}  // namespace tee
}  // namespace tiered_omap
