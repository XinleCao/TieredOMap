#include "tiered_omap/tee/packed_directory.h"

namespace tiered_omap {
namespace tee {

PackedDirectory::PackedDirectory(int capacity)
    : capacity_(capacity), entries_(capacity) {}

int PackedDirectory::lookup(int key) const {
    int result = INVALID_LEAF;
    for (int i = 0; i < capacity_; ++i) {
        int match = o_equal(entries_[i].key, key);
        o_mov_i(match, result, entries_[i].pos_label);
    }
    return result;
}

bool PackedDirectory::update_pos(int key, int new_pos) {
    int found = 0;
    for (int i = 0; i < capacity_; ++i) {
        int match = o_equal(entries_[i].key, key);
        o_mov_i(match, entries_[i].pos_label, new_pos);
        found |= match;
    }
    return found != 0;
}

bool PackedDirectory::insert(int key, int pos_label) {
    int real = 1 - o_equal(key, INVALID_KEY);
    int inserted = 0;
    for (int i = 0; i < capacity_; ++i) {
        int not_inserted = 1 - inserted;
        int is_empty = o_equal(entries_[i].key, INVALID_KEY);
        int is_slot = real & not_inserted & is_empty;
        o_mov_i(is_slot, entries_[i].key, key);
        o_mov_i(is_slot, entries_[i].pos_label, pos_label);
        inserted |= is_slot;
    }
    count_ += inserted;
    return inserted != 0;
}

bool PackedDirectory::remove(int key) {
    int real = 1 - o_equal(key, INVALID_KEY);
    int removed = 0;
    for (int i = 0; i < capacity_; ++i) {
        int match = real & o_equal(entries_[i].key, key);
        int inv_key = INVALID_KEY;
        int inv_leaf = INVALID_LEAF;
        o_mov_i(match, entries_[i].key, inv_key);
        o_mov_i(match, entries_[i].pos_label, inv_leaf);
        int zero = 0;
        o_mov_i(match, entries_[i].freq_count, zero);
        o_mov_i(match, entries_[i].freq_epoch, zero);
        removed |= match;
    }
    count_ -= removed;
    return removed != 0;
}

int PackedDirectory::lookup_and_update(int key, int new_pos) {
    int old_pos = INVALID_LEAF;
    for (int i = 0; i < capacity_; ++i) {
        int match = o_equal(entries_[i].key, key);
        o_mov_i(match, old_pos, entries_[i].pos_label);
        o_mov_i(match, entries_[i].pos_label, new_pos);
    }
    return old_pos;
}

void PackedDirectory::bump_freq(int key, int current_epoch) {
    for (int i = 0; i < capacity_; ++i) {
        int match = o_equal(entries_[i].key, key);
        int stale = match & o_less(entries_[i].freq_epoch, current_epoch);
        int fresh = match & (1 - stale);
        // Stale → reset to count=1, update epoch.
        int one = 1;
        o_mov_i(stale, entries_[i].freq_count, one);
        o_mov_i(stale, entries_[i].freq_epoch, current_epoch);
        // Fresh → increment count.
        int inc = entries_[i].freq_count + 1;
        o_mov_i(fresh, entries_[i].freq_count, inc);
    }
}

int PackedDirectory::find_demote_candidate(int scan_idx, int current_epoch,
                                           int staleness_windows,
                                           int demote_threshold) const {
    int result = INVALID_KEY;
    int cap = capacity_;
    for (int i = 0; i < cap; ++i) {
        int idx = (scan_idx + i) % cap;
        auto& e = entries_[idx];
        int is_real = 1 - o_equal(e.key, INVALID_KEY);
        int is_stale = is_real
                     & (o_less(e.freq_epoch, current_epoch - staleness_windows + 1)
                        | o_equal(e.freq_epoch, 0));
        int is_cold = is_real & o_less(e.freq_count, demote_threshold);
        int should = is_stale | is_cold;
        int not_found = o_equal(result, INVALID_KEY);
        o_mov_i(should & not_found, result, e.key);
    }
    return result;
}

}  // namespace tee
}  // namespace tiered_omap
