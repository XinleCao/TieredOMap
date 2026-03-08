#pragma once

#include "tiered_omap/common.h"
#include "tiered_omap/tee/oblivious.h"
#include <cstdint>
#include <cstring>
#include <unordered_map>
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
        uint64_t accesses = 0;
        void reset() { pages_touched = accesses = 0; }
    };

    EnclaveOram() = default;
    EnclaveOram(int num_data, int value_size, int bucket_size = 4,
                int stash_scale = 7);

    void init(const std::vector<std::pair<int, Bytes>>& data);

    // Standard ORAM access (read or read-modify-write).
    Bytes access(int key, const Bytes* new_value = nullptr);

    // Dummy access (touch a random path, no real key).
    void dummy_access();

    int random_leaf() const;
    int level() const { return level_; }
    int leaf_range() const { return leaf_range_; }

    const Stats& last_stats() const { return last_stats_; }
    const Stats& total_stats() const { return total_stats_; }
    void reset_stats() { last_stats_.reset(); total_stats_.reset(); }

    // Position map access (client-side, in-enclave).
    int get_leaf(int key) const;
    void set_leaf(int key, int leaf);

    int stash_size() const { return static_cast<int>(stash_.size()); }

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
    std::unordered_map<int, int> pos_map_;

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

    void begin_page_tracking();
    uint64_t end_page_tracking();

    // Path helpers.
    static int parent(int i) { return (i - 1) / 2; }
    static int left_child(int i) { return 2 * i + 1; }
    static int right_child(int i) { return 2 * i + 2; }
    int leaf_to_node(int leaf) const { return leaf_range_ - 1 + leaf; }
    int node_depth(int i) const;
    bool path_contains(int leaf, int node) const;
};

}  // namespace tee
}  // namespace tiered_omap
