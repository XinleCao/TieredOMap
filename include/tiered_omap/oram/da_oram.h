#pragma once

#include "tiered_omap/common.h"
#include "tiered_omap/oram/binary_tree_storage.h"
#include <functional>
#include <unordered_map>
#include <vector>

namespace tiered_omap {

// Counter block: stores GC, IC[num_ic], backup[num_ic] for a group.
struct CounterBlock {
    uint64_t gc = 0;
    std::vector<uint8_t> ic;      // individual counters (0..ic_max-1)
    std::vector<uint8_t> backup;  // backup indicators (0 or 1)

    CounterBlock() = default;
    explicit CounterBlock(int num_ic)
        : ic(num_ic, 0), backup(num_ic, 0) {}
};

class DAOram {
public:
    DAOram() = default;
    DAOram(int num_data, int bucket_size = 4, int stash_scale = 20,
           int num_ic = 64, int ic_max = 64);

    void init(const std::unordered_map<int, Bytes>& data);

    Bytes access(int key, const Bytes* new_value = nullptr);
    void dummy_access();

    // Split access: read without eviction, then evict with updated value.
    Bytes access_without_eviction(int key);
    void complete_eviction(int key, const Bytes& new_value);

    // Low-level interface for ODS OMAPs (same as PathORAM).
    void read_path_to_stash(int leaf);
    void evict_and_write_path(int leaf);
    void evict_and_write_paths(const std::vector<int>& leaves);
    Block* find_in_stash(int key);
    Block extract_from_stash(int key);
    void add_to_stash(Block block);
    int get_leaf(int key) const;
    void set_leaf(int key, int leaf);
    int random_leaf() const;

    int num_data() const { return num_data_; }
    int level() const { return storage_.level(); }
    int leaf_range() const { return storage_.leaf_range(); }
    int bucket_size() const { return bucket_size_; }
    int stash_size() const { return static_cast<int>(stash_.size()); }

    const BandwidthStats& last_stats() const { return last_bw_; }
    const BandwidthStats& total_stats() const { return total_bw_; }

private:
    // PRF-based leaf derivation.
    int prf_leaf(int data_key, uint64_t gc, uint8_t ic) const;

    // Counter management: returns (cur_leaf, new_leaf).
    std::pair<int, int> update_counter(int data_key);

    // Reset: find entry needing reset, returns (reset_data_key, cur_leaf, new_leaf).
    // reset_data_key = -1 if no reset needed.
    std::tuple<int, int, int> perform_reset(int group_key);

    void evict_stash(const std::vector<int>& leaves);

    int num_data_ = 0;
    int bucket_size_ = 4;
    int stash_max_size_ = 0;
    int num_ic_ = 64;    // entries per counter block
    int ic_max_ = 64;    // max IC value before overflow (2^ic_length)

    BinaryTreeStorage storage_;
    std::vector<Block> stash_;

    // Client-side counter storage: group_key → CounterBlock.
    // group_key = data_key / num_ic.
    std::vector<CounterBlock> counters_;

    uint64_t prf_seed_ = 0;
    int block_size_bytes_ = 0;
    std::vector<int> pending_leaves_;
    BandwidthStats last_bw_;
    BandwidthStats total_bw_;
};

}  // namespace tiered_omap
