#pragma once

#include "tiered_omap/common.h"
#include "tiered_omap/crypto.h"
#include "tiered_omap/network/storage_interface.h"
#include "tiered_omap/oram/binary_tree_storage.h"
#include <chrono>
#include <thread>
#include <unordered_map>

namespace tiered_omap {

class PathORAM {
public:
    PathORAM() = default;
    PathORAM(int num_data, int bucket_size = 4, int stash_scale = 7,
             StorageCreator storage_creator = nullptr);

    void init(const std::unordered_map<int, Bytes>& data);

    Bytes access(int key, const Bytes* new_value = nullptr);
    void dummy_access();

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
    int level() const { return storage_ ? storage_->level() : 0; }
    int leaf_range() const { return storage_ ? storage_->leaf_range() : 0; }
    int bucket_size() const { return bucket_size_; }
    int stash_size() const { return static_cast<int>(stash_.size()); }
    int block_size_bytes() const { return block_size_bytes_; }

    int path_bandwidth_bytes() const {
        if (!storage_) return 0;
        return storage_->level() * bucket_size_ *
               (block_size_bytes_ + 2 * static_cast<int>(sizeof(int)));
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
    std::unique_ptr<StorageInterface> storage_;
    StorageCreator storage_creator_;
    std::vector<Block> stash_;
    std::unordered_map<int, int> pos_map_;
    int block_size_bytes_ = 0;
    int round_delay_us_ = 0;
    BandwidthStats last_bw_;
    BandwidthStats total_bw_;
    CryptoKey aes_key_;
    void encrypt_bucket(std::vector<Block>& bucket);
    void decrypt_bucket(std::vector<Block>& bucket);
};

}  // namespace tiered_omap
