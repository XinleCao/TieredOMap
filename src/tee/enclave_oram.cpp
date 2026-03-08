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

void EnclaveOram::init(const std::vector<std::pair<int, Bytes>>& data) {
    stash_.clear();

    // Preserve pre-assigned leaves (set via set_leaf before init).
    for (auto& [key, val] : data) {
        if (pos_map_.find(key) == pos_map_.end())
            pos_map_[key] = SecureRandom::rand_below(leaf_range_);
    }

    // Simple sequential fill: place each block along its assigned path.
    for (auto& [key, val] : data) {
        int leaf = pos_map_[key];
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
}

Bytes EnclaveOram::access(int key, const Bytes* new_value) {
    last_stats_.reset();
    begin_page_tracking();

    auto it = pos_map_.find(key);
    if (it == pos_map_.end())
        throw std::runtime_error("EnclaveOram::access: key not found");

    int old_leaf = it->second;
    int new_leaf = random_leaf();
    pos_map_[key] = new_leaf;

    auto path_nodes = read_path(old_leaf);

    // Find block in stash (oblivious linear scan).
    Bytes result(value_size_, 0);
    bool found = false;
    for (auto& sb : stash_) {
        bool match = (sb.key == key);
        o_mov_bytes(match, result, sb.value);
        // Update value if writing.
        if (new_value) {
            Bytes padded = pad_bytes(*new_value, value_size_);
            o_mov_bytes(match, sb.value, padded);
        }
        o_mov_i(match, sb.leaf, new_leaf);
        if (match) found = true;
    }
    if (!found)
        throw std::runtime_error("EnclaveOram::access: key " +
                                 std::to_string(key) + " not in stash after path read");

    evict_path(old_leaf, path_nodes);

    last_stats_.accesses = 1;
    last_stats_.pages_touched = end_page_tracking();
    total_stats_.accesses += last_stats_.accesses;
    total_stats_.pages_touched += last_stats_.pages_touched;

    return result;
}

void EnclaveOram::dummy_access() {
    last_stats_.reset();
    begin_page_tracking();

    int leaf = random_leaf();
    auto path_nodes = read_path(leaf);
    evict_path(leaf, path_nodes);

    last_stats_.accesses = 1;
    last_stats_.pages_touched = end_page_tracking();
    total_stats_.accesses += last_stats_.accesses;
    total_stats_.pages_touched += last_stats_.pages_touched;
}

int EnclaveOram::random_leaf() const {
    return SecureRandom::rand_below(leaf_range_);
}

int EnclaveOram::get_leaf(int key) const {
    auto it = pos_map_.find(key);
    return (it != pos_map_.end()) ? it->second : INVALID_LEAF;
}

void EnclaveOram::set_leaf(int key, int leaf) {
    pos_map_[key] = leaf;
}

// ── Path read ────────────────────────────────────────────────────────────────

std::vector<int> EnclaveOram::read_path(int leaf) {
    std::vector<int> nodes;
    int node = leaf_to_node(leaf);
    while (node >= 0) {
        nodes.push_back(node);
        record_node_access(node);

        auto& bucket = tree_[node];
        for (int j = 0; j < bucket_size_; ++j) {
            if (bucket.blocks[j].key != INVALID_KEY) {
                stash_.push_back(std::move(bucket.blocks[j]));
                bucket.blocks[j] = {};
                bucket.blocks[j].value.resize(value_size_, 0);
            }
        }
        if (node == 0) break;
        node = parent(node);
    }
    return nodes;
}

// ── Oblivious eviction ──────────────────────────────────────────────────────

void EnclaveOram::evict_path(int /*leaf*/, const std::vector<int>& path_nodes) {
    // For each node on the path (deepest first), try to fill its bucket
    // from stash using oblivious selection.
    for (int pi = 0; pi < static_cast<int>(path_nodes.size()); ++pi) {
        int node = path_nodes[pi];
        record_node_access(node);

        auto& bucket = tree_[node];
        for (int j = 0; j < bucket_size_; ++j) {
            // Oblivious scan: pick the first stash block whose path
            // intersects this node.
            bool filled = false;
            for (auto& sb : stash_) {
                bool candidate = (sb.key != INVALID_KEY)
                              && path_contains(sb.leaf, node)
                              && !filled;
                // Conditionally move block into bucket slot.
                o_mov_i(candidate, bucket.blocks[j].key, sb.key);
                o_mov_i(candidate, bucket.blocks[j].leaf, sb.leaf);
                o_mov_bytes(candidate, bucket.blocks[j].value, sb.value);
                // Conditionally remove from stash.
                int inv = INVALID_KEY;
                o_mov_i(candidate, sb.key, inv);
                if (candidate) filled = true;
            }
        }
    }

    // Compact stash: remove invalidated entries.
    stash_.erase(
        std::remove_if(stash_.begin(), stash_.end(),
                        [](const TreeBlock& b) { return b.key == INVALID_KEY; }),
        stash_.end());

    if (static_cast<int>(stash_.size()) > stash_max_)
        throw std::runtime_error(
            "EnclaveOram: stash overflow (" + std::to_string(stash_.size()) +
            " > " + std::to_string(stash_max_) + ")");
}

// ── Low-level interface for multi-node operations ───────────────────────────

void EnclaveOram::read_path_to_stash(int leaf) {
    read_path(leaf);   // moves all blocks from the path into stash_
}

Block EnclaveOram::extract_from_stash(int key) {
    for (auto it = stash_.begin(); it != stash_.end(); ++it) {
        if (it->key == key) {
            Block b{it->key, it->leaf, std::move(it->value)};
            stash_.erase(it);
            return b;
        }
    }
    return Block{};  // not found
}

void EnclaveOram::add_to_stash(int key, int leaf, const Bytes& value) {
    stash_.push_back({key, leaf, pad_bytes(value, value_size_)});
}

void EnclaveOram::evict_one_path(int leaf) {
    std::vector<int> path_nodes;
    int node = leaf_to_node(leaf);
    while (node >= 0) {
        path_nodes.push_back(node);
        if (node == 0) break;
        node = parent(node);
    }
    evict_path(leaf, path_nodes);
}

// ── Path geometry ───────────────────────────────────────────────────────────

int EnclaveOram::node_depth(int i) const {
    int d = 0;
    int n = i;
    while (n > 0) { n = parent(n); ++d; }
    return d;
}

bool EnclaveOram::path_contains(int leaf, int node) const {
    int cur = leaf_to_node(leaf);
    while (cur >= node) {
        if (cur == node) return true;
        if (cur == 0) break;
        cur = parent(cur);
    }
    return false;
}

// ── Page tracking ───────────────────────────────────────────────────────────

int EnclaveOram::node_byte_size() const {
    return bucket_size_ * (static_cast<int>(sizeof(int)) * 2 + value_size_);
}

void EnclaveOram::begin_page_tracking() {
    touched_pages_.clear();
}

uint64_t EnclaveOram::end_page_tracking() {
    return touched_pages_.size();
}

void EnclaveOram::record_node_access(int node_idx) {
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
