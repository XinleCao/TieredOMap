#pragma once

#include "tiered_omap/common.h"
#include "tiered_omap/omap/avl_omap.h"  // for AVLNodeData, AVL_HEADER_SIZE
#include "tiered_omap/tee/enclave_oram.h"
#include <functional>
#include <memory>
#include <vector>

namespace tiered_omap {
namespace tee {

// Oblivious AVL Map running inside a TEE, backed by EnclaveOram.
//
// Split-ORAM: when split_depth > 0, AVL nodes at depth < split_depth
// are stored in upper_oram_ (small, fits in EPC), and the rest in
// lower_oram_ (large, may exceed EPC → page faults).
class TeeAvlOmap {
public:
    struct Stats {
        uint64_t upper_pages = 0;
        uint64_t lower_pages = 0;
        uint64_t upper_node_accesses = 0;
        uint64_t lower_node_accesses = 0;
        uint64_t total_pages() const { return upper_pages + lower_pages; }
        uint64_t total_node_accesses() const { return upper_node_accesses + lower_node_accesses; }
    };

    TeeAvlOmap(int capacity, int value_size, int bucket_size = 4,
               int split_depth = 0);

    void init(const std::vector<std::pair<int, Bytes>>& data);

    // Transforms the found value in-place before flush. Always called
    // (including when not found) so the call pattern is data-independent.
    using ValueTransform = std::function<Bytes(const Bytes& old_value)>;

    Bytes search(int key, const Bytes* update = nullptr,
                 ValueTransform transform = nullptr);
    // Doubly-oblivious search: always same ORAM access pattern regardless of key.
    // When real==false, performs dummy access with identical pattern.
    // Result is always value_size_ bytes (zeros if not found).
    // If out_found != nullptr, *out_found is set to whether the key was found.
    Bytes search_or_dummy(int key, bool real, const Bytes* update = nullptr,
                          ValueTransform transform = nullptr,
                          bool* out_found = nullptr);

    void insert(int key, const Bytes& value);
    void remove(int key);
    void dummy_access();

    int root_key() const { return root_key_; }
    int max_height() const { return max_height_; }
    bool is_split() const { return split_depth_ > 0; }
    const Stats& last_stats() const { return last_stats_; }

private:
    struct LocalNode {
        int key;
        int leaf;
        AVLNodeData avl;
        int parent_key;
        int depth;
    };

    EnclaveOram& oram_for_depth(int depth);

    // Doubly-oblivious ORAM step: reads a path, extracts, and evicts.
    // When is_real==0, uses random leaf and non-matching key (same access pattern).
    void oram_step(int depth, int key, int leaf, int is_real, int parent_key);

    // Padding step: same ORAM access pattern as oram_step but doesn't modify local_.
    void pad_step(int depth);

    void move_to_local(int key, int leaf, int parent_key, int depth);
    void flush_local_to_stash();
    void reassign_leaves();

    void update_heights();
    void rebalance();
    std::tuple<int, int, int> balance_node(int idx);
    std::tuple<int, int, int> rotate(int idx, bool left);
    void rotate_cond(int idx, int active, int dir);

    void pad_ops(int ops_done);
    void begin_stats();
    void end_stats();

    int max_height_ = 0;
    int split_depth_ = 0;
    int value_size_ = 0;

    int root_key_ = INVALID_KEY;
    int root_leaf_ = INVALID_LEAF;

    EnclaveOram upper_oram_;
    EnclaveOram lower_oram_;

    std::vector<LocalNode> local_;
    int upper_ops_ = 0;
    int lower_ops_ = 0;
    Stats last_stats_;
};

}  // namespace tee
}  // namespace tiered_omap
