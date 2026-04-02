#pragma once

#include "tiered_omap/common.h"
#include "tiered_omap/tee/oblivious.h"
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace tiered_omap {
namespace tee {

// Flat-memory ORAM tree for TEE mode.
//
// In SGX the tree resides in untrusted memory (or in EPC if small enough).
// We simulate this with a flat byte array; in real SGX, reads/writes to
// this array become OCALLs.
//
// Page-fault tracking: every read/write to the backing store records which
// 4 KB pages are touched so we can report metrics matching the paper.
class EnclaveOram {
public:
    struct Stats {
        uint64_t pages_touched = 0;
        uint64_t node_accesses = 0;
        uint64_t accesses = 0;
        void reset() { pages_touched = node_accesses = accesses = 0; }
    };

    EnclaveOram() = default;
    EnclaveOram(int num_data, int value_size, int bucket_size = 4,
                int stash_scale = 7);

    // Init returns a map of key -> assigned leaf for the caller to record.
    // If preset_leaves is non-empty, those assignments are used instead of
    // generating random ones (needed by TeeAvlOmap where leaf pointers are
    // embedded in the AVL node data before init).
    std::unordered_map<int, int> init(
        const std::vector<std::pair<int, Bytes>>& data,
        const std::unordered_map<int, int>& preset_leaves = {});

    // Doubly-oblivious access. Caller supplies old_leaf (from its own
    // position tracking) and new_leaf (fresh random). The ORAM reads the
    // old_leaf path and re-randomises the block to new_leaf.
    Bytes access_or_dummy(int key, int old_leaf, int new_leaf,
                          bool real, const Bytes* new_value = nullptr);

    // Convenience wrappers.
    Bytes access(int key, int old_leaf, int new_leaf,
                 const Bytes* new_value = nullptr);
    void dummy_access();

    // ── Low-level interface for multi-node operations (AVL tree) ────────
    //
    // Pattern: read_path_to_stash → extract_from_stash → (modify locally)
    //          → add_to_stash (with new leaf) → evict_one_path
    //
    // Allows reading multiple nodes before writing them all back.

    // Read a leaf-to-root path; move all non-dummy blocks into stash.
    void read_path_to_stash(int leaf);

    // Remove a specific key's block from the stash and return it.
    // Returns a block with key==INVALID_KEY if not found.
    Block extract_from_stash(int key);

    // Add a block to the stash (e.g. after modifying it locally).
    void add_to_stash(int key, int leaf, const Bytes& value);

    // Evict stash along the specified leaf's path (should match the path
    // that was read).  Call once per read_path_to_stash.
    void evict_one_path(int leaf);

    // ────────────────────────────────────────────────────────────────────

    int random_leaf() const;
    int level() const { return level_; }
    int leaf_range() const { return leaf_range_; }

    const Stats& last_stats() const { return last_stats_; }
    const Stats& total_stats() const { return total_stats_; }
    void reset_stats() { last_stats_.reset(); total_stats_.reset(); }

    int stash_size() const { return static_cast<int>(stash_.size()); }
    int stash_valid_count() const;

    // DEBUG: check where a key is (0=nowhere, 1=stash, 2=tree)
    int locate_key(int key) const {
        for (auto& sb : stash_)
            if (sb.key == key) return 1;
        for (auto& b : tree_)
            for (auto& bl : b.blocks)
                if (bl.key == key) return 2;
        return 0;
    }

    void begin_page_tracking();
    uint64_t end_page_tracking();
    uint64_t tracked_node_accesses() const { return node_access_count_; }

    static constexpr int PAGE_SIZE = 4096;

private:
    struct TreeBlock {
        int key = INVALID_KEY;
        int leaf = INVALID_LEAF;
        Bytes value;  // fixed size, padded to value_size_
    };

    // Tree layout: node i has children 2i+1, 2i+2.
    // Node i at depth d corresponds to bucket index i.
    // Each node stores bucket_size_ blocks.
    struct Bucket {
        std::vector<TreeBlock> blocks;
    };

    int value_size_ = 0;
    int bucket_size_ = 4;
    int level_ = 0;
    int leaf_range_ = 0;
    int num_nodes_ = 0;
    int stash_max_ = 0;

    std::vector<Bucket> tree_;
    std::vector<TreeBlock> stash_;

    Stats last_stats_;
    Stats total_stats_;

    // Read a full root-to-leaf path into stash.  Returns the set of node
    // indices along the path (for later write-back).
    std::vector<int> read_path(int leaf);

    // Oblivious eviction: write stash blocks back along the path.
    void evict_path(int leaf, const std::vector<int>& path_nodes);

    // Page tracking helpers.
    int node_byte_size() const;
    void record_node_access(int node_idx);
    std::unordered_map<uint64_t, bool> touched_pages_;
    uint64_t node_access_count_ = 0;

    // Ensure stash_ has exactly stash_max_ entries, padding with INVALID_KEY.
    void pad_stash();

    // Path helpers.
    static int parent(int i) { return (i - 1) / 2; }
    static int left_child(int i) { return 2 * i + 1; }
    static int right_child(int i) { return 2 * i + 2; }
    int leaf_to_node(int leaf) const { return leaf_range_ - 1 + leaf; }
    int node_depth(int i) const;
    bool path_contains(int leaf, int node) const;
    bool path_contains_oblivious(int leaf, int node) const;
};

}  // namespace tee
}  // namespace tiered_omap
