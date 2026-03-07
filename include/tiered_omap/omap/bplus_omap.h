#pragma once

#include "tiered_omap/common.h"
#include "tiered_omap/omap/omap_interface.h"
#include "tiered_omap/oram/path_oram.h"
#include <functional>
#include <memory>
#include <vector>

namespace tiered_omap {

struct BPlusNode {
    bool is_leaf = true;
    std::vector<int> keys;           // search keys
    std::vector<int> child_ids;      // (internal only) ORAM keys of children
    std::vector<int> child_leaves;   // (internal only) ORAM leaves of children
    std::vector<Bytes> values;       // (leaf only) data values

    Bytes encode() const;
    static BPlusNode decode(const Bytes& raw);
};

class BPlusOmap : public OmapInterface {
public:
    // capacity = max number of key-value pairs.
    // order = B+ tree branching factor (max children per internal node).
    BPlusOmap(int capacity, int order = 8, int bucket_size = 4);

    void init(const std::vector<std::pair<int, Bytes>>& data) override;

    Bytes search(int key, const Bytes* update = nullptr) override;
    void insert(int key, const Bytes& value) override;
    void remove(int key) override;
    void dummy_access() override;

    Bytes search_piggyback(int key, const Bytes* update,
                           int extra_key, char extra_op,
                           const Bytes* extra_value,
                           Bytes* extra_result) override;

    void set_round_delay_us(int us) override { oram_.set_round_delay_us(us); }

    const BandwidthStats& last_stats() const override { return last_bw_; }
    const BandwidthStats& total_stats() const override { return total_bw_; }
    void reset_stats() override { last_bw_.reset(); total_bw_.reset(); }

    // ODS mode: used as inner tree by DaOstOmap.
    void set_ods_mode(int tree_height_bound) {
        ods_mode_ = true;
        max_height_ = tree_height_bound;
    }
    void set_root(int id, int leaf) { root_id_ = id; root_leaf_ = leaf; }
    std::pair<int,int> get_root() const { return {root_id_, root_leaf_}; }
    int last_op_count() const { return op_count_; }
    PathORAM& oram() { return oram_; }
    void set_next_block_id(int id) { next_block_id_ = id; }
    int next_block_id() const { return next_block_id_; }

private:
    struct LocalNode {
        int id;
        int leaf;
        BPlusNode node;
        int parent_id;
    };

    struct CachedSibling {
        int id = INVALID_KEY;
        int leaf = INVALID_LEAF;
        BPlusNode node;
        int parent_local_idx = -1;
        int child_idx_in_parent = -1;
    };

    void move_to_local(int id, int leaf, int parent_id);
    void move_to_sibling_cache(int id, int leaf, int parent_local_idx, int child_idx);
    int traverse_with_siblings(int key);
    void flush_all_to_stash();
    void reassign_all_leaves();
    void do_dummy_ops(int count);

    static int find_child_index(const BPlusNode& node, int key);
    static int find_leaf_index(const BPlusNode& node, int key);

    int split_leaf(LocalNode& leaf_node);
    int split_internal(LocalNode& internal_node);

    int min_leaf_keys() const;
    void handle_delete_underflow();

    int build_tree(const std::vector<std::pair<int, Bytes>>& sorted,
                   std::unordered_map<int, Bytes>& oram_data);

    int order_ = 8;
    int max_height_ = 0;
    int root_id_ = INVALID_KEY;
    int root_leaf_ = INVALID_LEAF;
    int next_block_id_ = 0;

    void finalize_bw();

    PathORAM oram_;
    std::vector<LocalNode> local_;
    std::vector<CachedSibling> sibling_cache_;
    bool ods_mode_ = false;
    int op_count_ = 0;
    BandwidthStats last_bw_;
    BandwidthStats total_bw_;
};

}  // namespace tiered_omap
