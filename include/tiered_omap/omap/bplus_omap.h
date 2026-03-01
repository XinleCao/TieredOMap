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

    const BandwidthStats& last_stats() const override { return last_bw_; }
    const BandwidthStats& total_stats() const override { return total_bw_; }
    void reset_stats() override { last_bw_.reset(); total_bw_.reset(); }

private:
    struct LocalNode {
        int id;
        int leaf;
        BPlusNode node;
        int parent_id;
    };

    void move_to_local(int id, int leaf, int parent_id);
    void flush_local_to_stash();
    void reassign_leaves();
    void do_dummy_ops(int count);

    // Find the child index for key in an internal node.
    static int find_child_index(const BPlusNode& node, int key);
    // Find the value index for key in a leaf node.
    static int find_leaf_index(const BPlusNode& node, int key);

    // Split a full leaf node; returns the new sibling's ORAM id.
    int split_leaf(LocalNode& leaf_node);
    // Split a full internal node; returns the new sibling's ORAM id.
    int split_internal(LocalNode& internal_node);

    // Build B+ tree bottom-up from sorted data, returns root ORAM id.
    int build_tree(const std::vector<std::pair<int, Bytes>>& sorted,
                   std::unordered_map<int, Bytes>& oram_data);

    int capacity_ = 0;
    int order_ = 8;
    int max_height_ = 0;
    int root_id_ = INVALID_KEY;
    int root_leaf_ = INVALID_LEAF;
    int next_block_id_ = 0;

    void finalize_bw();

    PathORAM oram_;
    std::vector<LocalNode> local_;
    int op_count_ = 0;
    BandwidthStats last_bw_;
    BandwidthStats total_bw_;
};

}  // namespace tiered_omap
