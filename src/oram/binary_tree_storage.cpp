#include "tiered_omap/oram/binary_tree_storage.h"
#include <algorithm>
#include <cstdint>
#include <ios>
#include <istream>
#include <ostream>
#include <stdexcept>

namespace tiered_omap {

namespace {

void write_i32(std::ostream& out, int v) {
    out.write(reinterpret_cast<const char*>(&v), sizeof(v));
    if (!out) throw std::runtime_error("BinaryTreeStorage::write_state failed");
}

int read_i32(std::istream& in) {
    int v = 0;
    in.read(reinterpret_cast<char*>(&v), sizeof(v));
    if (!in) throw std::runtime_error("BinaryTreeStorage::read_state failed");
    return v;
}

void write_u64(std::ostream& out, uint64_t v) {
    out.write(reinterpret_cast<const char*>(&v), sizeof(v));
    if (!out) throw std::runtime_error("BinaryTreeStorage::write_state failed");
}

uint64_t read_u64(std::istream& in) {
    uint64_t v = 0;
    in.read(reinterpret_cast<char*>(&v), sizeof(v));
    if (!in) throw std::runtime_error("BinaryTreeStorage::read_state failed");
    return v;
}

}  // namespace

BinaryTreeStorage::BinaryTreeStorage(int num_data, int bucket_size)
    : bucket_size_(bucket_size) {
    level_ = ceil_log2(num_data) + 1;
    leaf_range_ = 1 << (level_ - 1);
    total_nodes_ = (1 << level_) - 1;
    storage_.resize(total_nodes_);
}

void BinaryTreeStorage::reset(int num_data, int bucket_size) {
    bucket_size_ = bucket_size;
    level_ = ceil_log2(num_data) + 1;
    leaf_range_ = 1 << (level_ - 1);
    total_nodes_ = (1 << level_) - 1;
    storage_.clear();
    storage_.resize(total_nodes_);
}

std::unordered_set<int>
BinaryTreeStorage::bulk_load(const std::vector<Block>& blocks) {
    for (auto& block : blocks)
        fill_data_to_leaf(block);

    std::unordered_set<int> placed;
    for (auto& bucket : storage_)
        for (auto& block : bucket)
            if (!block.is_dummy())
                placed.insert(block.key);
    return placed;
}

std::vector<int> BinaryTreeStorage::get_path_indices(int leaf) const {
    std::vector<int> path;
    path.reserve(level_);
    int node = leaf_to_node(leaf);
    while (node >= 0) {
        path.push_back(node);
        if (node == 0) break;
        node = parent(node);
    }
    std::reverse(path.begin(), path.end());
    return path;
}

std::vector<int> BinaryTreeStorage::get_merged_path_indices(
    int level, const std::vector<int>& leaves) {
    int leaf_range = 1 << (level - 1);
    std::set<int> node_set;
    for (int leaf : leaves) {
        int node = leaf_range - 1 + leaf;
        while (node >= 0) {
            node_set.insert(node);
            if (node == 0) break;
            node = parent(node);
        }
    }
    return {node_set.begin(), node_set.end()};
}

void BinaryTreeStorage::fill_data_to_leaf(const Block& block) {
    auto path = get_path_indices(block.leaf);
    for (int i = static_cast<int>(path.size()) - 1; i >= 0; --i) {
        int node = path[i];
        if (static_cast<int>(storage_[node].size()) < bucket_size_) {
            storage_[node].push_back(block);
            return;
        }
    }
    // Could not fit in tree; caller should handle by adding to stash.
}

std::unordered_map<int, std::vector<Block>>
BinaryTreeStorage::read_path(int leaf) const {
    auto indices = get_path_indices(leaf);
    std::unordered_map<int, std::vector<Block>> result;
    for (int idx : indices) {
        if (idx < 0 || idx >= static_cast<int>(storage_.size()))
            throw std::runtime_error("BinaryTreeStorage::read_path: node out of range");
        result[idx] = storage_[idx];
    }
    return result;
}

void BinaryTreeStorage::write_path(
    int /*leaf*/, const std::unordered_map<int, std::vector<Block>>& buckets) {
    for (auto& [node, blocks] : buckets) {
        if (node < 0 || node >= static_cast<int>(storage_.size()))
            throw std::runtime_error("BinaryTreeStorage::write_path: node out of range");
        storage_[node] = blocks;
    }
}

std::unordered_map<int, std::vector<Block>>
BinaryTreeStorage::read_multiple_paths(const std::vector<int>& leaves) const {
    auto indices = get_merged_path_indices(level_, leaves);
    std::unordered_map<int, std::vector<Block>> result;
    for (int idx : indices) {
        if (idx < 0 || idx >= static_cast<int>(storage_.size()))
            throw std::runtime_error("BinaryTreeStorage::read_multiple_paths: node out of range");
        result[idx] = storage_[idx];
    }
    return result;
}

void BinaryTreeStorage::write_multiple_paths(
    const std::unordered_map<int, std::vector<Block>>& buckets) {
    for (auto& [node, blocks] : buckets) {
        if (node < 0 || node >= static_cast<int>(storage_.size()))
            throw std::runtime_error("BinaryTreeStorage::write_multiple_paths: node out of range");
        storage_[node] = blocks;
    }
}

bool BinaryTreeStorage::fill_block_to_path(
    const Block& block,
    std::unordered_map<int, std::vector<Block>>& path,
    const std::vector<int>& /*leaves*/,
    int level, int bucket_size) {
    int leaf_range = 1 << (level - 1);

    // For each node on the path (sorted deepest first), check if block's leaf
    // is in the subtree rooted at that node.
    // Collect path nodes sorted by depth (deepest first).
    std::vector<int> sorted_nodes;
    sorted_nodes.reserve(path.size());
    for (auto& [node, _] : path)
        sorted_nodes.push_back(node);
    std::sort(sorted_nodes.rbegin(), sorted_nodes.rend());

    for (int node : sorted_nodes) {
        if (static_cast<int>(path[node].size()) >= bucket_size)
            continue;

        // Check if block.leaf is in the subtree of node.
        int target_node = leaf_range - 1 + block.leaf;
        int cur = target_node;
        bool is_ancestor = false;
        while (cur >= 0) {
            if (cur == node) { is_ancestor = true; break; }
            if (cur == 0) break;
            cur = parent(cur);
        }
        if (is_ancestor) {
            path[node].push_back(block);
            return true;
        }
    }
    return false;
}

void BinaryTreeStorage::write_state(std::ostream& out) const {
    write_i32(out, level_);
    write_i32(out, leaf_range_);
    write_i32(out, bucket_size_);
    write_i32(out, total_nodes_);
    write_u64(out, static_cast<uint64_t>(storage_.size()));
    for (const auto& bucket : storage_) {
        write_u64(out, static_cast<uint64_t>(bucket.size()));
        for (const auto& block : bucket) {
            write_i32(out, block.key);
            write_i32(out, block.leaf);
            write_u64(out, static_cast<uint64_t>(block.value.size()));
            if (!block.value.empty()) {
                out.write(reinterpret_cast<const char*>(block.value.data()),
                          static_cast<std::streamsize>(block.value.size()));
                if (!out)
                    throw std::runtime_error("BinaryTreeStorage::write_state failed");
            }
        }
    }
}

std::unique_ptr<BinaryTreeStorage>
BinaryTreeStorage::read_state(std::istream& in) {
    auto store = std::make_unique<BinaryTreeStorage>();
    store->level_ = read_i32(in);
    store->leaf_range_ = read_i32(in);
    store->bucket_size_ = read_i32(in);
    store->total_nodes_ = read_i32(in);
    uint64_t node_count = read_u64(in);
    if (node_count > static_cast<uint64_t>(store->total_nodes_) ||
        node_count > static_cast<uint64_t>(1ULL << 32)) {
        throw std::runtime_error("BinaryTreeStorage::read_state invalid node count");
    }
    store->storage_.clear();
    store->storage_.resize(static_cast<size_t>(node_count));
    for (auto& bucket : store->storage_) {
        uint64_t bucket_count = read_u64(in);
        if (bucket_count > static_cast<uint64_t>(store->bucket_size_)) {
            throw std::runtime_error("BinaryTreeStorage::read_state invalid bucket size");
        }
        bucket.reserve(static_cast<size_t>(bucket_count));
        for (uint64_t i = 0; i < bucket_count; ++i) {
            Block block;
            block.key = read_i32(in);
            block.leaf = read_i32(in);
            uint64_t value_size = read_u64(in);
            if (value_size > static_cast<uint64_t>(1ULL << 32)) {
                throw std::runtime_error("BinaryTreeStorage::read_state value too large");
            }
            block.value.resize(static_cast<size_t>(value_size));
            if (!block.value.empty()) {
                in.read(reinterpret_cast<char*>(block.value.data()),
                        static_cast<std::streamsize>(block.value.size()));
                if (!in)
                    throw std::runtime_error("BinaryTreeStorage::read_state failed");
            }
            bucket.push_back(std::move(block));
        }
    }
    return store;
}

}  // namespace tiered_omap
