#include "tiered_omap/oram/binary_tree_storage.h"
#include <algorithm>

namespace tiered_omap {

BinaryTreeStorage::BinaryTreeStorage(int num_data, int bucket_size)
    : num_data_(num_data),
      bucket_size_(bucket_size) {
    level_ = ceil_log2(num_data) + 1;
    leaf_range_ = 1 << (level_ - 1);
    total_nodes_ = (1 << level_) - 1;
    storage_.resize(total_nodes_);
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
    for (int idx : indices)
        result[idx] = storage_[idx];
    return result;
}

void BinaryTreeStorage::write_path(
    int leaf, const std::unordered_map<int, std::vector<Block>>& buckets) {
    for (auto& [node, blocks] : buckets)
        storage_[node] = blocks;
}

std::unordered_map<int, std::vector<Block>>
BinaryTreeStorage::read_multiple_paths(const std::vector<int>& leaves) const {
    auto indices = get_merged_path_indices(level_, leaves);
    std::unordered_map<int, std::vector<Block>> result;
    for (int idx : indices)
        result[idx] = storage_[idx];
    return result;
}

void BinaryTreeStorage::write_multiple_paths(
    const std::unordered_map<int, std::vector<Block>>& buckets) {
    for (auto& [node, blocks] : buckets)
        storage_[node] = blocks;
}

bool BinaryTreeStorage::fill_block_to_path(
    const Block& block,
    std::unordered_map<int, std::vector<Block>>& path,
    const std::vector<int>& leaves,
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

}  // namespace tiered_omap
