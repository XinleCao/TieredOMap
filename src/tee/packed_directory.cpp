#include "tiered_omap/tee/packed_directory.h"

namespace tiered_omap {
namespace tee {

PackedDirectory::PackedDirectory(int capacity)
    : capacity_(capacity), entries_(capacity) {}

int PackedDirectory::lookup(int key) const {
    int result = INVALID_LEAF;
    for (int i = 0; i < capacity_; ++i) {
        bool match = (entries_[i].key == key);
        o_mov_i(match, result, entries_[i].pos_label);
    }
    return result;
}

bool PackedDirectory::update_pos(int key, int new_pos) {
    bool found = false;
    for (int i = 0; i < capacity_; ++i) {
        bool match = (entries_[i].key == key);
        o_mov_i(match, entries_[i].pos_label, new_pos);
        found = found || match;
    }
    return found;
}

bool PackedDirectory::insert(int key, int pos_label) {
    bool inserted = false;
    for (int i = 0; i < capacity_; ++i) {
        bool is_slot = !inserted && (entries_[i].key == INVALID_KEY);
        o_mov_i(is_slot, entries_[i].key, key);
        o_mov_i(is_slot, entries_[i].pos_label, pos_label);
        inserted = inserted || is_slot;
    }
    if (inserted) ++count_;
    return inserted;
}

bool PackedDirectory::remove(int key) {
    bool removed = false;
    for (int i = 0; i < capacity_; ++i) {
        bool match = (entries_[i].key == key);
        o_mov_i(match, entries_[i].key, INVALID_KEY);
        o_mov_i(match, entries_[i].pos_label, INVALID_LEAF);
        removed = removed || match;
    }
    if (removed) --count_;
    return removed;
}

int PackedDirectory::lookup_and_update(int key, int new_pos) {
    int old_pos = INVALID_LEAF;
    for (int i = 0; i < capacity_; ++i) {
        bool match = (entries_[i].key == key);
        o_mov_i(match, old_pos, entries_[i].pos_label);
        o_mov_i(match, entries_[i].pos_label, new_pos);
    }
    return old_pos;
}

}  // namespace tee
}  // namespace tiered_omap
