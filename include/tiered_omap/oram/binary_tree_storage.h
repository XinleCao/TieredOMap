#pragma once

#include "tiered_omap/common.h"
#include <set>
#include <unordered_map>

namespace tiered_omap {

class BinaryTreeStorage {
public:
    BinaryTreeStorage() = default;
    BinaryTreeStorage(int num_data, int bucket_size);

    int level() const { return level_; }
    int leaf_range() const { return leaf_range_; }
    int bucket_size() const { return bucket_size_; }

    std::vector<int> get_path_indices(int leaf) const;

    static std::vector<int> get_merged_path_indices(
        int level, const std::vector<int>& leaves);

    void fill_data_to_leaf(const Block& block);

    std::unordered_map<int, std::vector<Block>>
    read_path(int leaf) const;

    void write_path(int leaf,
                    const std::unordered_map<int, std::vector<Block>>& buckets);

    std::unordered_map<int, std::vector<Block>>
    read_multiple_paths(const std::vector<int>& leaves) const;

    void write_multiple_paths(
        const std::unordered_map<int, std::vector<Block>>& buckets);

    static bool fill_block_to_path(
        const Block& block,
        std::unordered_map<int, std::vector<Block>>& path,
        const std::vector<int>& leaves,
        int level, int bucket_size);

private:
    int leaf_to_node(int leaf) const { return leaf_range_ - 1 + leaf; }
    static int parent(int node) { return (node - 1) / 2; }
    static int left_child(int node) { return 2 * node + 1; }
    static int right_child(int node) { return 2 * node + 2; }

    int num_data_ = 0;
    int level_ = 0;
    int leaf_range_ = 0;
    int bucket_size_ = 0;
    int total_nodes_ = 0;
    std::vector<std::vector<Block>> storage_;
};

}  // namespace tiered_omap
