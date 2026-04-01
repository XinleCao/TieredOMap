#pragma once

#include "tiered_omap/common.h"
#include "tiered_omap/network/storage_interface.h"
#include "tiered_omap/omap/omap_interface.h"
#include "tiered_omap/oram/binary_tree_storage.h"
#include "tiered_omap/oram/path_oram.h"
#include <functional>
#include <unordered_map>
#include <vector>

namespace tiered_omap {

class TcpChannel;

struct CounterBlock {
    uint64_t gc = 0;
    std::vector<uint8_t> ic;
    std::vector<uint8_t> backup;

    CounterBlock() = default;
    explicit CounterBlock(int num_ic)
        : ic(num_ic, 0), backup(num_ic, 0) {}

    Bytes encode() const;
    static CounterBlock decode(const Bytes& data, int num_ic);
};

class DAOram {
public:
    DAOram() = default;
    DAOram(int num_data, int bucket_size = 4, int stash_scale = 20,
           int num_ic = 64, int ic_max = 64,
           int on_chip_mem = 10,
           StorageCreator storage_creator = nullptr);

    void init(const std::unordered_map<int, Bytes>& data);

    Bytes access(int key, const Bytes* new_value = nullptr);
    void dummy_access();

    int num_pos_levels() const { return num_pos_levels_; }
    int num_access_rounds() const { return num_pos_levels_ + 1; }

    // ── Multi-round step interface for interleaved access ──────────────────
    void begin_step_access(int key);
    void begin_step_dummy();
    std::vector<StepReadReq> step_get_reads() const;
    void step_apply_reads(const std::vector<PathData>& results);
    void step_process();
    std::vector<StepWriteReq> step_get_writes();
    bool step_done() const;
    Bytes step_finish(const Bytes* new_value = nullptr);
    void step_finish_dummy();

    // ── Piggyback: concurrent second access sharing the same rounds ─────
    void begin_piggyback_access(int key);
    void begin_piggyback_dummy();
    Bytes piggyback_finish(const Bytes* new_value = nullptr);
    void piggyback_finish_dummy();

    // ── Legacy single-round interface (traverses pos_maps locally) ─────────
    struct PreparedAccess {
        int leaf1;
        int leaf2;
        int target_key;
        int new_leaf;
        int r_key;
        int r_new_leaf;
    };
    PreparedAccess prepare_interleaved_access(int key);
    PreparedAccess prepare_interleaved_dummy();
    void apply_fetched_paths(
        const std::unordered_map<int, std::vector<Block>>& merged);
    Bytes complete_interleaved_access(const PreparedAccess& prep,
                                     const Bytes* new_value);
    void complete_interleaved_dummy();
    std::unordered_map<int, std::vector<Block>>
        prepare_eviction_data(const std::vector<int>& leaves);

    // ── Store ID accessors ────────────────────────────────────────────────
    int get_store_id() const;
    int get_pos_map_store_id(int level) const;

    // ── State export / import ─────────────────────────────────────────────
    Bytes export_state(int store_id) const;
    static DAOram from_state_network(const uint8_t*& p,
                                     std::shared_ptr<TcpChannel> channel);

    std::unique_ptr<StorageInterface> detach_storage() {
        if (storage_) {
            cached_level_ = storage_->level();
            cached_leaf_range_ = storage_->leaf_range();
        }
        return std::move(storage_);
    }
    void attach_storage(std::unique_ptr<StorageInterface> s) {
        storage_ = std::move(s);
        if (storage_) {
            cached_level_ = storage_->level();
            cached_leaf_range_ = storage_->leaf_range();
        }
    }

    Bytes access_without_eviction(int key);
    void complete_eviction(int key, const Bytes& new_value);

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
    int level() const { return storage_ ? storage_->level() : cached_level_; }
    int leaf_range() const { return storage_ ? storage_->leaf_range() : cached_leaf_range_; }
    int bucket_size() const { return bucket_size_; }
    int stash_size() const { return static_cast<int>(stash_.size()); }
    int num_ic() const { return num_ic_; }
    int on_chip_mem() const { return on_chip_mem_; }

    PathORAM& pos_map_oram(int lvl) { return pos_maps_[lvl]; }
    const std::vector<int>& level_sizes() const { return level_sizes_; }

    const BandwidthStats& last_stats() const { return last_bw_; }
    const BandwidthStats& total_stats() const { return total_bw_; }

private:
    int prf_leaf(int key, uint64_t gc, uint8_t ic, int lr) const;
    int prf_leaf_data(int key, uint64_t gc, uint8_t ic) const;

    std::pair<int, int> update_counter_in(
        CounterBlock& cb, int offset, int managed_key, int lr);
    std::tuple<int, int, int> perform_reset_in(
        CounterBlock& cb, int base_key, int num_managed, int lr);
    void evict_stash(const std::vector<int>& leaves);

    struct PosMapResult {
        int leaf1, leaf2, new_leaf;
        int r_key, r_new_leaf;
    };
    PosMapResult traverse_pos_maps(int data_key);
    PosMapResult traverse_pos_maps_dummy();

    int num_data_ = 0;
    int bucket_size_ = 4;
    int stash_max_size_ = 0;
    int num_ic_ = 64;
    int ic_max_ = 64;
    int on_chip_mem_ = 10;

    std::unique_ptr<StorageInterface> storage_;
    StorageCreator storage_creator_;
    std::vector<Block> stash_;

    int num_pos_levels_ = 0;
    std::vector<int> level_sizes_;
    std::vector<PathORAM> pos_maps_;
    std::vector<CounterBlock> on_chip_;

    uint64_t prf_seed_ = 0;
    int block_size_bytes_ = 0;
    std::vector<int> pending_leaves_;
    int cached_level_ = 0;
    int cached_leaf_range_ = 0;

    struct StepState {
        bool active = false;
        bool is_dummy = false;
        int data_key = INVALID_KEY;
        int round = 0;
        bool done = false;

        int cur_leaf1 = INVALID_LEAF;
        int cur_leaf2 = INVALID_LEAF;
        int cur_new_leaf = INVALID_LEAF;
        int cur_r_key = INVALID_KEY;
        int cur_r_new_leaf = INVALID_LEAF;

        int write_level = -1;
        int write_leaf1 = INVALID_LEAF;
        int write_leaf2 = INVALID_LEAF;

        Bytes result;
    };
    StepState step_;

    struct PBStepState {
        bool active = false;
        bool is_dummy = false;
        int data_key = INVALID_KEY;

        int cur_leaf1 = INVALID_LEAF;
        int cur_leaf2 = INVALID_LEAF;
        int cur_new_leaf = INVALID_LEAF;
        int cur_r_key = INVALID_KEY;
        int cur_r_new_leaf = INVALID_LEAF;

        int write_leaf1 = INVALID_LEAF;
        int write_leaf2 = INVALID_LEAF;

        Bytes result;
    };
    PBStepState pb_step_;

    BandwidthStats last_bw_;
    BandwidthStats total_bw_;
};

}  // namespace tiered_omap
