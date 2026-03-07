#pragma once

#include "tiered_omap/common.h"
#include "tiered_omap/omap/omap_interface.h"
#include "tiered_omap/oram/path_oram.h"
#include <functional>
#include <memory>
#include <vector>

namespace tiered_omap {

constexpr int AVL_HEADER_SIZE = 6 * sizeof(int);  // 24 bytes

struct AVLNodeData {
    Bytes data;
    int l_key = INVALID_KEY, l_leaf = INVALID_LEAF, l_height = 0;
    int r_key = INVALID_KEY, r_leaf = INVALID_LEAF, r_height = 0;

    int height() const { return 1 + std::max(l_height, r_height); }
    int balance() const { return l_height - r_height; }

    Bytes encode() const;
    static AVLNodeData decode(const Bytes& raw);
};

class AVLOmap : public OmapInterface {
public:
    explicit AVLOmap(int capacity, int bucket_size = 4);

    // Split-ORAM constructor: top split_depth AVL levels → upper_oram(upper_cap),
    // remaining levels → oram_(capacity).
    AVLOmap(int capacity, int bucket_size, int split_depth, int upper_capacity);

    void init(const std::vector<std::pair<int, Bytes>>& data) override;

    Bytes search(int key, const Bytes* update = nullptr) override;
    void insert(int key, const Bytes& value) override;
    void remove(int key) override;
    void dummy_access() override;
    void partial_dummy_access() override;

    void set_round_delay_us(int us) override {
        oram_.set_round_delay_us(us);
        if (split_depth_ > 0) upper_oram_.set_round_delay_us(us);
    }

    const BandwidthStats& last_stats() const override { return last_bw_; }
    const BandwidthStats& total_stats() const override { return total_bw_; }
    void reset_stats() override { last_bw_.reset(); total_bw_.reset(); }

    int root_key() const { return root_key_; }
    int max_height() const { return max_height_; }
    bool is_split() const { return split_depth_ > 0; }

    // ODS mode: used as inner tree by DaOstOmap.
    void set_ods_mode(int tree_height_bound) {
        ods_mode_ = true;
        max_height_ = tree_height_bound;
    }
    void set_root(int key, int leaf) { root_key_ = key; root_leaf_ = leaf; }
    std::pair<int,int> get_root() const { return {root_key_, root_leaf_}; }
    int last_op_count() const { return op_count_; }
    PathORAM& oram() { return oram_; }

private:
    struct LocalNode {
        int key;
        int leaf;
        AVLNodeData avl;
        int parent_key;
        int depth;          // tree depth for split ORAM routing
    };

    PathORAM& oram_for_depth(int depth);
    void move_to_local(int key, int leaf, int parent_key, int depth);
    void flush_local_to_stash();
    void reassign_leaves();

    void update_heights();
    void rebalance();
    std::tuple<int, int, int> balance_node(int idx);
    std::tuple<int, int, int> rotate(int idx, bool left);

    int build_balanced(const std::vector<std::pair<int, Bytes>>& sorted,
                       int lo, int hi,
                       std::unordered_map<int, Bytes>& oram_data);

    void finalize_bw();
    void reset_op_counts();

    int capacity_ = 0;
    int max_height_ = 0;
    int root_key_ = INVALID_KEY;
    int root_leaf_ = INVALID_LEAF;

    int split_depth_ = 0;
    PathORAM upper_oram_;
    PathORAM oram_;              // lower ORAM (or sole ORAM when no split)

    std::vector<LocalNode> local_;
    bool ods_mode_ = false;
    int op_count_ = 0;          // used in non-split mode
    int upper_op_count_ = 0;    // split mode: ops on upper_oram
    int lower_op_count_ = 0;    // split mode: ops on oram_ (lower)
    BandwidthStats last_bw_;
    BandwidthStats total_bw_;
};

}  // namespace tiered_omap
