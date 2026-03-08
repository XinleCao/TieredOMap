#pragma once

#include "tiered_omap/common.h"
#include <functional>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace tiered_omap {

class StorageInterface {
public:
    virtual ~StorageInterface() = default;

    virtual int level() const = 0;
    virtual int leaf_range() const = 0;
    virtual int bucket_size() const = 0;

    virtual void reset(int num_data, int bucket_size) = 0;
    virtual void fill_data_to_leaf(const Block& block) = 0;

    // Fills all blocks into the tree and returns keys that were placed.
    virtual std::unordered_set<int>
    bulk_load(const std::vector<Block>& blocks) = 0;

    virtual std::unordered_map<int, std::vector<Block>>
    read_path(int leaf) const = 0;

    virtual void write_path(
        int leaf,
        const std::unordered_map<int, std::vector<Block>>& buckets) = 0;

    virtual std::unordered_map<int, std::vector<Block>>
    read_multiple_paths(const std::vector<int>& leaves) const = 0;

    virtual void write_multiple_paths(
        const std::unordered_map<int, std::vector<Block>>& buckets) = 0;
};

using StorageCreator = std::function<
    std::unique_ptr<StorageInterface>(int num_data, int bucket_size)>;

}  // namespace tiered_omap
