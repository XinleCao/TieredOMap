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

    int split_depth = config_.use_split_oram
                    ? std::max(1, ceil_log2(n))
                    : 0;
    cold_omap_ = std::make_unique<TeeAvlOmap>(
        N, config_.value_size, config_.bucket_size, split_depth);
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

    hot_oram_->init(hot_data);
    for (auto& [k, v] : hot_data) {
        int leaf = hot_oram_->get_leaf(k);
        hot_dir_->insert(k, leaf);
    }

    cold_omap_->init(cold_data);
}

TeeAccessResult TeeOmap::access(int key, const Bytes* new_value,
                                ResponseCallback early_cb,
                                ResponseCallback final_cb) {
    TeeAccessResult result;

    // ── Phase 1: Hot tier (directory scan + hot ORAM) ───────────────────

    int old_pos = hot_dir_->lookup(key);
    bool hot_hit = (old_pos != INVALID_LEAF);

    if (hot_hit) {
        result.value = hot_oram_->access(key, new_value);
        result.found_in_hot = true;
        int new_leaf = hot_oram_->get_leaf(key);
        hot_dir_->update_pos(key, new_leaf);
    } else {
        hot_oram_->dummy_access();
        hot_dir_->update_pos(key, INVALID_LEAF);
    }

    int dir_pages = (hot_dir_->capacity() * 12 + EnclaveOram::PAGE_SIZE - 1)
                  / EnclaveOram::PAGE_SIZE;
    result.hot_pages = static_cast<uint64_t>(dir_pages) * 2
                     + hot_oram_->last_stats().pages_touched;

    // ── Early response ──────────────────────────────────────────────────

    if (early_cb)
        early_cb(result.value, result.found_in_hot);

    // ── Phase 2: Cold tier (TeeAvlOmap) ─────────────────────────────────

    if (config_.mode == TeeSecurityMode::FullOblivious) {
        if (!hot_hit) {
            Bytes cold_val = cold_omap_->search(key, new_value);
            result.value = cold_val;
        } else {
            cold_omap_->dummy_access();
        }
    } else {
        // Tier-membership: skip cold on hot hit.
        if (!hot_hit) {
            Bytes cold_val = cold_omap_->search(key, new_value);
            result.value = cold_val;
        }
    }

    result.cold_up_pages = cold_omap_->last_stats().upper_pages;
    result.cold_low_pages = cold_omap_->last_stats().lower_pages;
    result.total_pages = result.hot_pages + result.cold_up_pages
                       + result.cold_low_pages;

    // ── Final response ──────────────────────────────────────────────────

    if (final_cb)
        final_cb(result.value, !result.found_in_hot);

    return result;
}

void TeeOmap::promote(int key) {
    Bytes val = cold_omap_->search(key);
    if (val.empty()) return;

    cold_omap_->remove(key);

    // Place in hot ORAM.
    hot_oram_->set_leaf(key, hot_oram_->random_leaf());
    hot_oram_->access(key, &val);
    hot_dir_->insert(key, hot_oram_->get_leaf(key));
    hot_keys_.insert(key);
}

void TeeOmap::demote(int key) {
    if (!hot_keys_.count(key)) return;

    Bytes val = hot_oram_->access(key);
    hot_dir_->remove(key);
    hot_keys_.erase(key);

    cold_omap_->insert(key, val);
}

}  // namespace tee
}  // namespace tiered_omap
