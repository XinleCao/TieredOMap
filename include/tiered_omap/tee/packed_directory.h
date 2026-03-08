#pragma once

#include "tiered_omap/common.h"
#include "tiered_omap/tee/oblivious.h"
#include <vector>

namespace tiered_omap {
namespace tee {

// Packed-array hot-key directory for TEE deployment.
//
// Stores (key, pos_label) pairs in a flat array.  Every access performs a
// full oblivious linear scan so the set of touched pages is identical
// regardless of which key is queried.
//
// This replaces the OMAP-based hot index in the TEE setting.  With
// n = 1024 entries × 12 bytes, the directory is ≈ 12 KB (3 pages).
class PackedDirectory {
public:
    struct Entry {
        int key = INVALID_KEY;
        int pos_label = INVALID_LEAF;
    };

    explicit PackedDirectory(int capacity);

    // Oblivious lookup: scans the entire directory and returns the
    // pos_label for `key` (or INVALID_LEAF if absent).
    int lookup(int key) const;

    // Oblivious update of pos_label for an existing key.
    // Returns true if the key was found.
    bool update_pos(int key, int new_pos);

    // Oblivious insert: adds (key, pos_label).  Scans the full array
    // and writes into the first empty slot.  Returns false if full.
    bool insert(int key, int pos_label);

    // Oblivious remove: sets the matching entry to (INVALID_KEY, INVALID_LEAF).
    bool remove(int key);

    // Combined lookup + update in a single scan: reads old pos_label,
    // writes new_pos in its place.  Returns old pos_label.
    int lookup_and_update(int key, int new_pos);

    int capacity() const { return capacity_; }
    int size() const { return count_; }
    const std::vector<Entry>& entries() const { return entries_; }

    // Scan all entries and call fn(key, pos_label) for each non-empty slot.
    // Not oblivious — use only for testing / initialisation.
    template <typename Fn>
    void for_each(Fn&& fn) const {
        for (auto& e : entries_)
            if (e.key != INVALID_KEY)
                fn(e.key, e.pos_label);
    }

private:
    int capacity_;
    int count_ = 0;
    std::vector<Entry> entries_;
};

}  // namespace tee
}  // namespace tiered_omap
