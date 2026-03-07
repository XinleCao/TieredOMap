#pragma once

#include "tiered_omap/common.h"
#include "tiered_omap/oram/binary_tree_storage.h"
#include <chrono>
#include <thread>
#include <unordered_map>

namespace tiered_omap {

class PathORAM {
public:
    PathORAM() = default;
    PathORAM(int num_data, int bucket_size = 4, int stash_scale = 7);

    void init(const std::unordered_map<int, Bytes>& data);

    Bytes access(int key, const Bytes* new_value = nullptr);
    void dummy_access();

    // --- Low-level interface for ODS-based OMAPs ---

    // Read the path for `leaf`, move all real blocks to stash.
    void read_path_to_stash(int leaf);

    // Evict blocks from stash to path(s) and write back.
    void evict_and_write_path(int leaf);
    void evict_and_write_paths(const std::vector<int>& leaves);

    // Find a block in the stash (returns nullptr if not found).
    Block* find_in_stash(int key);

    // Extract a block from the stash (removes it). Throws if not found.
    Block extract_from_stash(int key);

    // Add a block to the stash.
    void add_to_stash(Block block);

    // Position map access.
    int get_leaf(int key) const;
    void set_leaf(int key, int leaf);
    int random_leaf() const;

    // Properties.
    int num_data() const { return num_data_; }
    int level() const { return storage_.level(); }
    int leaf_range() const { return storage_.leaf_range(); }
    int bucket_size() const { return bucket_size_; }
    int stash_size() const { return static_cast<int>(stash_.size()); }
    int block_size_bytes() const { return block_size_bytes_; }

    // Bytes for one full path read or write (level * Z * block_bytes).
    int path_bandwidth_bytes() const {
        return storage_.level() * bucket_size_ * (block_size_bytes_ + 2 * static_cast<int>(sizeof(int)));
    }

    void set_round_delay_us(int us) { round_delay_us_ = us; }
    void inject_round_delay() const {
        if (round_delay_us_ > 0)
            std::this_thread::sleep_for(std::chrono::microseconds(round_delay_us_));
    }

    const BandwidthStats& last_stats() const { return last_bw_; }
    const BandwidthStats& total_stats() const { return total_bw_; }
    void reset_stats() { last_bw_.reset(); total_bw_.reset(); }

private:
    void evict_stash(const std::vector<int>& leaves);

    int num_data_ = 0;
    int bucket_size_ = 4;
    int stash_max_size_ = 0;
    BinaryTreeStorage storage_;
    std::vector<Block> stash_;
    std::unordered_map<int, int> pos_map_;
    int block_size_bytes_ = 0;
    int round_delay_us_ = 0;
    BandwidthStats last_bw_;
    BandwidthStats total_bw_;
};

}  // namespace tiered_omap
