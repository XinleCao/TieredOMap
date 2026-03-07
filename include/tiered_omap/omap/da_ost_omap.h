#pragma once

#include "tiered_omap/common.h"
#include "tiered_omap/omap/omap_interface.h"
#include "tiered_omap/omap/avl_omap.h"
#include "tiered_omap/omap/bplus_omap.h"
#include "tiered_omap/oram/da_oram.h"

namespace tiered_omap {

enum class OdsTreeType { AVL, BPlus };

class DaOstOmap : public OmapInterface {
public:
    // capacity  = total number of KV pairs.
    // num_positions = number of hash buckets (default: same as capacity).
    DaOstOmap(int capacity, OdsTreeType tree_type = OdsTreeType::AVL,
              int num_positions = 0, int bucket_size = 4, int bplus_order = 8);

    void init(const std::vector<std::pair<int, Bytes>>& data) override;

    Bytes search(int key, const Bytes* update = nullptr) override;
    void insert(int key, const Bytes& value) override;
    void remove(int key) override;
    void dummy_access() override;

    void set_round_delay_us(int us) override;

    const BandwidthStats& last_stats() const override { return last_bw_; }
    const BandwidthStats& total_stats() const override { return total_bw_; }
    void reset_stats() override { last_bw_.reset(); total_bw_.reset(); }

    int num_positions() const { return num_positions_; }
    int tree_height_bound() const { return tree_height_bound_; }
    int ods_budget() const { return ods_budget_; }
    OdsTreeType tree_type() const { return tree_type_; }

private:
    int hash_to_position(int key) const;

    static Bytes encode_root(int key, int leaf);
    static std::pair<int,int> decode_root(const Bytes& b);

    void do_avl_init(const std::vector<std::pair<int, Bytes>>& data);
    void do_bplus_init(const std::vector<std::pair<int, Bytes>>& data);

    // Runs ods_budget_ - actual_ops dummy ODS rounds.
    void pad_ods(int actual_ops);
    void finalize_bw(int ods_ops);

    int capacity_;
    int num_positions_;
    int tree_height_bound_;
    int ods_budget_;
    OdsTreeType tree_type_;
    int bucket_size_;
    int bplus_order_;

    DAOram daoram_;
    AVLOmap avl_ods_;
    BPlusOmap bplus_ods_;

    uint64_t hash_seed_ = 0;

    BandwidthStats last_bw_;
    BandwidthStats total_bw_;
};

}  // namespace tiered_omap
