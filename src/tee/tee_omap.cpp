#include "tiered_omap/tee/tee_omap.h"
#include <algorithm>
#include <stdexcept>

namespace tiered_omap {
namespace tee {

TeeOmap::TeeOmap(const TeeOmapConfig& config) : config_(config) {
    int n = config_.hot_set_size;
    int N = config_.total_keys;

    hot_dir_ = std::make_unique<PackedDirectory>(n);
    hot_oram_ = std::make_unique<EnclaveOram>(
        n, config_.value_size, config_.bucket_size);

    if (config_.use_split_oram) {
        split_depth_ = std::max(1, ceil_log2(n));
        int upper_cap = (1 << split_depth_) - 1;
        cold_up_ = std::make_unique<EnclaveOram>(
            std::max(upper_cap, 1), config_.value_size, config_.bucket_size);
    }

    cold_low_ = std::make_unique<EnclaveOram>(
        N, config_.value_size, config_.bucket_size);
}

void TeeOmap::init(const std::vector<std::pair<int, Bytes>>& all_data,
                   const std::vector<int>& hot_keys) {
    hot_keys_.clear();
    hot_keys_.insert(hot_keys.begin(), hot_keys.end());

    std::vector<std::pair<int, Bytes>> hot_data, cold_data;
    for (auto& [k, v] : all_data) {
        if (hot_keys_.count(k))
            hot_data.push_back({k, v});
        else
            cold_data.push_back({k, v});
    }

    // Init hot ORAM and directory.
    hot_oram_->init(hot_data);
    for (auto& [k, v] : hot_data) {
        int leaf = hot_oram_->get_leaf(k);
        hot_dir_->insert(k, leaf);
    }

    // Init cold ORAM(s).
    // Phase 1: use cold_store_ for correctness; cold ORAMs get the data too.
    cold_low_->init(cold_data);
    for (auto& [k, v] : cold_data)
        cold_store_[k] = v;

    if (cold_up_) {
        // In split mode, cold_up_ gets a subset of cold data (the upper
        // tree levels).  For Phase 1, we initialise it with dummy data
        // and use cold_store_ for actual lookups.
        std::vector<std::pair<int, Bytes>> dummy_up;
        int up_cap = (1 << split_depth_) - 1;
        for (int i = 0; i < std::min(up_cap, static_cast<int>(cold_data.size())); ++i)
            dummy_up.push_back(cold_data[i]);
        if (!dummy_up.empty())
            cold_up_->init(dummy_up);
    }
}

TeeAccessResult TeeOmap::access(int key, const Bytes* new_value,
                                ResponseCallback early_cb,
                                ResponseCallback final_cb) {
    TeeAccessResult result;

    // ── Phase 1: Hot tier (directory scan + hot ORAM) ───────────────────

    // Read-only directory scan: check if key is hot.
    int old_pos = hot_dir_->lookup(key);
    bool hot_hit = (old_pos != INVALID_LEAF);

    if (hot_hit) {
        // Real hot ORAM access.  The ORAM manages pos_map internally:
        // it reads the path at the old leaf and remaps to a new random leaf.
        result.value = hot_oram_->access(key, new_value);
        result.found_in_hot = true;

        // Update directory with the ORAM's new leaf assignment.
        int new_leaf = hot_oram_->get_leaf(key);
        hot_dir_->update_pos(key, new_leaf);
    } else {
        // Dummy hot ORAM access (obliviousness: always touch hot ORAM).
        hot_oram_->dummy_access();
        // Dummy directory write scan (same page footprint as a real update).
        hot_dir_->update_pos(key, INVALID_LEAF);
    }

    int dir_pages = (hot_dir_->capacity() * 12 + EnclaveOram::PAGE_SIZE - 1)
                  / EnclaveOram::PAGE_SIZE;
    result.hot_pages = static_cast<uint64_t>(dir_pages) * 2  // read + write scan
                     + hot_oram_->last_stats().pages_touched;

    // ── Early response ──────────────────────────────────────────────────

    if (early_cb)
        early_cb(result.value, result.found_in_hot);

    // ── Phase 2: Cold tier ──────────────────────────────────────────────

    bool need_cold_real = !hot_hit;  // cold access is real only on miss

    if (config_.mode == TeeSecurityMode::FullOblivious) {
        // Full obliv: always do the full cold access regardless of hot result.
        if (need_cold_real) {
            Bytes cold_val = cold_search(key, new_value);
            result.value = cold_val;
        } else {
            cold_dummy_access();
        }
    } else {
        // Tier-membership: adversary knows which tier was hit.
        // On hot hit → skip cold entirely (zero cold pages).
        // On cold hit → do the real cold access.
        if (need_cold_real) {
            Bytes cold_val = cold_search(key, new_value);
            result.value = cold_val;
        }
        // else: no cold access at all — tier membership already leaked.
    }

    result.cold_up_pages = cold_up_ ? cold_up_->last_stats().pages_touched : 0;
    result.cold_low_pages = cold_low_->last_stats().pages_touched;
    result.total_pages = result.hot_pages + result.cold_up_pages
                       + result.cold_low_pages;

    // ── Final response ──────────────────────────────────────────────────

    if (final_cb)
        final_cb(result.value, !result.found_in_hot);

    return result;
}

// ── Cold tier helpers (Phase 1: backed by hash map for correctness) ─────

Bytes TeeOmap::cold_search(int key, const Bytes* new_value) {
    // Touch both cold ORAMs for obliviousness.
    if (cold_up_) cold_up_->dummy_access();
    cold_low_->dummy_access();

    // Phase 1 correctness: use cold_store_.
    auto it = cold_store_.find(key);
    if (it == cold_store_.end()) return Bytes(config_.value_size, 0);
    Bytes val = it->second;
    if (new_value) it->second = *new_value;
    return val;
}

void TeeOmap::cold_insert(int key, const Bytes& value) {
    cold_store_[key] = value;
    // TODO: proper oblivious AVL insert into cold ORAM(s).
}

void TeeOmap::cold_remove(int key) {
    cold_store_.erase(key);
    // TODO: proper oblivious AVL remove from cold ORAM(s).
}

void TeeOmap::cold_dummy_access() {
    if (cold_up_) cold_up_->dummy_access();
    cold_low_->dummy_access();
}

void TeeOmap::promote(int key) {
    auto it = cold_store_.find(key);
    if (it == cold_store_.end()) return;

    Bytes val = it->second;
    cold_remove(key);

    // Insert into hot ORAM + directory.
    hot_oram_->set_leaf(key, hot_oram_->random_leaf());
    // Access to place the block in the tree.
    hot_oram_->access(key, &val);
    int leaf = hot_oram_->get_leaf(key);
    hot_dir_->insert(key, leaf);
    hot_keys_.insert(key);
}

void TeeOmap::demote(int key) {
    if (!hot_keys_.count(key)) return;

    Bytes val = hot_oram_->access(key);
    hot_dir_->remove(key);
    hot_keys_.erase(key);

    cold_insert(key, val);
}

}  // namespace tee
}  // namespace tiered_omap
