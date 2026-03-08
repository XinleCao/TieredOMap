#include "tiered_omap/tee/tee_omap.h"
#include <algorithm>
#include <stdexcept>

namespace tiered_omap {
namespace tee {

TeeOmap::TeeOmap(const TeeOmapConfig& config) : config_(config) {
    int n = config_.hot_set_size;
    int N = config_.total_keys;

    hot_capacity_ = config_.maintenance.enabled
                  ? std::max(n * 2, n + 64)
                  : n;

    hot_dir_ = std::make_unique<PackedDirectory>(hot_capacity_);
    hot_oram_ = std::make_unique<EnclaveOram>(
        hot_capacity_, config_.value_size, config_.bucket_size);

    int cold_cap = config_.maintenance.enabled
                 ? std::max(N - n, N)
                 : N;
    int split_depth = config_.use_split_oram
                    ? std::max(1, ceil_log2(std::max(n, 2)))
                    : 0;
    cold_omap_ = std::make_unique<TeeAvlOmap>(
        cold_cap, config_.value_size, config_.bucket_size, split_depth);
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

    // Pad hot ORAM with dummies if capacity > actual hot keys (for maintenance).
    int real_hot = static_cast<int>(hot_data.size());
    for (int i = 0; i < hot_capacity_ - real_hot; ++i)
        hot_data.push_back({-(i + 1), Bytes(config_.value_size, 0)});

    hot_oram_->init(hot_data);
    for (auto& [k, v] : hot_data) {
        if (k >= 0) {
            int leaf = hot_oram_->get_leaf(k);
            hot_dir_->insert(k, leaf);
        }
    }

    cold_omap_->init(cold_data);

    // Init frequency table for all keys.
    if (config_.maintenance.enabled) {
        for (auto& [k, v] : all_data)
            freq_[k] = {0, 0};
    }
}

TeeAccessResult TeeOmap::access(int key, const Bytes* new_value,
                                ResponseCallback early_cb,
                                ResponseCallback final_cb) {
    TeeAccessResult result;
    auto& mcfg = config_.maintenance;

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

    // ── Phase 2: Cold tier ──────────────────────────────────────────────

    if (config_.mode == TeeSecurityMode::FullOblivious) {
        if (!hot_hit) {
            Bytes cold_val = cold_omap_->search(key, new_value);
            result.value = cold_val;
        } else {
            cold_omap_->dummy_access();
        }
    } else {
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

    // ── Frequency tracking & maintenance ────────────────────────────────

    if (mcfg.enabled) {
        auto& fe = freq_[key];
        if (fe.epoch < current_epoch_) {
            fe.count = 1;
            fe.epoch = current_epoch_;
        } else {
            ++fe.count;
        }

        // Cold key with high frequency → enqueue for promotion.
        if (!hot_hit && fe.count >= mcfg.promote_threshold
            && !hot_keys_.count(key)) {
            promo_queue_.push(key);
        }

        ++access_counter_;
        if (access_counter_ >= mcfg.epoch_length) {
            access_counter_ = 0;
            ++current_epoch_;
            do_maintenance_step();
        }
    }

    return result;
}

// ── Maintenance: one promote + one demote per epoch boundary ────────────

void TeeOmap::do_maintenance_step() {
    auto& mcfg = config_.maintenance;

    // ── Demotion: scan hot keys round-robin ─────────────────────────────
    // Enclave can inspect hot_dir_ directly (enclave memory, not oblivious).
    int demote_key = INVALID_KEY;
    auto& entries = hot_dir_->entries();
    int cap = hot_dir_->capacity();
    for (int i = 0; i < cap; ++i) {
        int idx = (scan_ptr_ + i) % cap;
        int k = entries[idx].key;
        if (k == INVALID_KEY) continue;

        auto it = freq_.find(k);
        if (it == freq_.end()) continue;
        auto& fe = it->second;

        bool stale = (fe.epoch <= current_epoch_ - mcfg.staleness_epochs);
        bool cold_freq = (fe.count < mcfg.demote_threshold);
        if (stale || cold_freq) {
            demote_key = k;
            scan_ptr_ = (idx + 1) % cap;
            break;
        }
    }
    if (demote_key == INVALID_KEY)
        scan_ptr_ = (scan_ptr_ + 1) % std::max(cap, 1);

    bool did_demote = false;
    if (demote_key != INVALID_KEY && hot_keys_.count(demote_key)) {
        demote(demote_key);
        did_demote = true;
    }

    // For FullOblivious: if no real demotion, do dummy ops to hide it.
    if (config_.mode == TeeSecurityMode::FullOblivious && !did_demote) {
        hot_oram_->dummy_access();
        cold_omap_->dummy_access();
    }

    // ── Promotion ───────────────────────────────────────────────────────
    int promo_key = INVALID_KEY;
    bool did_promote = false;
    while (!promo_queue_.empty()) {
        int pk = promo_queue_.front();
        promo_queue_.pop();
        if (!hot_keys_.count(pk)
            && static_cast<int>(hot_keys_.size()) < hot_capacity_) {
            promo_key = pk;
            break;
        }
    }

    if (promo_key != INVALID_KEY) {
        promote(promo_key);
        did_promote = true;
    }

    if (config_.mode == TeeSecurityMode::FullOblivious && !did_promote) {
        cold_omap_->dummy_access();
        hot_oram_->dummy_access();
    }
}

// ── Promote / Demote ────────────────────────────────────────────────────

void TeeOmap::promote(int key) {
    Bytes val = cold_omap_->search(key);
    if (val.empty()) return;

    cold_omap_->remove(key);

    // Insert into hot ORAM.
    // The hot ORAM needs a dummy slot for the new key.  We set up a pos_map
    // entry and add to stash, then do one access to place it.
    hot_oram_->set_leaf(key, hot_oram_->random_leaf());
    hot_oram_->add_to_stash(key, hot_oram_->get_leaf(key), val);
    hot_oram_->evict_one_path(hot_oram_->get_leaf(key));

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
