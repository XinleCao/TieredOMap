#include "tiered_omap/tiered_omap.h"
#include "tiered_omap/omap/avl_omap.h"
#include "tiered_omap/omap/bplus_omap.h"
#include "tiered_omap/omap/da_ost_omap.h"
#include <algorithm>
#include <stdexcept>

namespace tiered_omap {

TieredOMap::TieredOMap(const TieredOMapConfig& config)
    : config_(config) {
    if (config_.maintenance.enabled)
        maint_ = std::make_unique<MaintenanceManager>(config_.maintenance);
}

void TieredOMap::init(
    const std::vector<std::pair<int, Bytes>>& all_data,
    const std::vector<int>& hot_keys) {
    hot_keys_ = {hot_keys.begin(), hot_keys.end()};
    phys_hot_keys_ = hot_keys_;
    hot_key_list_ = hot_keys;
    std::sort(hot_key_list_.begin(), hot_key_list_.end());

    bool use_epoch = config_.maintenance.enabled;

    std::vector<std::pair<int, Bytes>> hot_data, cold_data;
    for (auto& [k, v] : all_data) {
        Bytes stored = v;
        if (use_epoch)
            stored = encode_with_epoch(v, {0, 0});

        if (phys_hot_keys_.count(k))
            hot_data.emplace_back(k, stored);
        else
            cold_data.emplace_back(k, stored);
    }

    int n = static_cast<int>(hot_data.size());
    int N_cold = static_cast<int>(cold_data.size());
    int bs = config_.bucket_size;

    int hot_cap = use_epoch ? std::max(n * 2, n + 64) : std::max(n, 1);
    int cold_cap = use_epoch ? std::max(N_cold, config_.total_keys) : std::max(N_cold, 1);
    hot_capacity_ = hot_cap;

    switch (config_.backend) {
    case OmapBackend::BPlus:
        hot_omap_ = std::make_unique<BPlusOmap>(hot_cap, config_.bplus_order, bs);
        cold_omap_ = std::make_unique<BPlusOmap>(cold_cap, config_.bplus_order, bs);
        break;
    case OmapBackend::DaAvl:
        hot_omap_ = std::make_unique<DaOstOmap>(hot_cap, OdsTreeType::AVL, 0, bs);
        cold_omap_ = std::make_unique<DaOstOmap>(cold_cap, OdsTreeType::AVL, 0, bs);
        break;
    case OmapBackend::DaBplus:
        hot_omap_ = std::make_unique<DaOstOmap>(
            hot_cap, OdsTreeType::BPlus, 0, bs, config_.bplus_order);
        cold_omap_ = std::make_unique<DaOstOmap>(
            cold_cap, OdsTreeType::BPlus, 0, bs, config_.bplus_order);
        break;
    default:  // AVL
        hot_omap_ = std::make_unique<AVLOmap>(hot_cap, bs);
        if (config_.use_split_oram && n > 0) {
            int split_depth = ceil_log2(std::max(n, 2));
            cold_omap_ = std::make_unique<AVLOmap>(
                cold_cap, bs, split_depth, std::max(n, 1));
        } else {
            cold_omap_ = std::make_unique<AVLOmap>(cold_cap, bs);
        }
        break;
    }
    hot_omap_->init(hot_data);
    cold_omap_->init(cold_data);
}

AccessResult TieredOMap::access(int key, const Bytes* new_value) {
    AccessResult result;
    bool is_hot_logical = hot_keys_.count(key) > 0;
    bool is_hot_physical = phys_hot_keys_.count(key) > 0;
    bool use_epoch = maint_ != nullptr;
    bool piggyback_on = use_epoch && config_.maintenance.piggyback
                        && config_.backend == OmapBackend::BPlus;

    result.found_in_hot = is_hot_logical;

    bool maint_due = use_epoch && maint_->should_maintain_next();
    bool pig_active = piggyback_on && maint_due && is_hot_physical
                      && !hot_key_list_.empty();

    if (use_epoch) {
        OmapInterface* target = is_hot_physical ? hot_omap_.get()
                                                : cold_omap_.get();
        OmapInterface* other  = is_hot_physical ? cold_omap_.get()
                                                : hot_omap_.get();

        // Determine scan key for piggybacked demotion.
        int scan_key = INVALID_KEY;
        if (pig_active) {
            int idx = maint_->scan_index();
            if (idx >= static_cast<int>(hot_key_list_.size())) idx = 0;
            scan_key = hot_key_list_[idx];
        }

        // Phase 1: read main value (+ piggyback scan-read of scan_key).
        Bytes scan_raw;
        Bytes raw;
        if (pig_active && scan_key != INVALID_KEY) {
            raw = target->search_piggyback(
                key, nullptr, scan_key, 's', nullptr, &scan_raw);
        } else {
            raw = target->search(key);
        }

        auto [val, meta] = decode_epoch(raw);
        bool promote_flag = false;
        meta = maint_->on_access(key, is_hot_logical, meta, promote_flag);
        Bytes wb = encode_with_epoch(new_value ? *new_value : val, meta);
        result.value = new_value ? *new_value : val;

        // Decide demotion from scanned metadata.
        bool demote = false;
        if (pig_active && scan_key != INVALID_KEY && !scan_raw.empty()) {
            auto [sv, sm] = decode_epoch(scan_raw);
            demote = maint_->should_demote(sm);
        }

        // Phase 2: write-back main (+ piggyback conditional delete).
        if (pig_active && scan_key != INVALID_KEY) {
            target->search_piggyback(
                key, &wb, scan_key, demote ? 'd' : 's', nullptr, nullptr);
        } else {
            target->search(key, &wb);
        }
        result.hot_bw = hot_omap_->last_stats();

        // Phase 3: other OMAP — insert demoted entry or dummy.
        if (pig_active && demote && !scan_raw.empty()) {
            other->insert_replacing_dummy(scan_key, scan_raw);
        } else {
            other->dummy_access();
        }

        // Update logical/physical hot sets after piggybacked demotion.
        if (pig_active && demote && scan_key != INVALID_KEY) {
            hot_keys_.erase(scan_key);
            phys_hot_keys_.erase(scan_key);
            auto it = std::lower_bound(
                hot_key_list_.begin(), hot_key_list_.end(), scan_key);
            if (it != hot_key_list_.end() && *it == scan_key)
                hot_key_list_.erase(it);
        }
        if (pig_active)
            maint_->advance_scan(static_cast<int>(hot_key_list_.size()));

    } else {
        if (is_hot_physical) {
            result.value = hot_omap_->search(key, new_value);
            result.hot_bw = hot_omap_->last_stats();
            if (config_.mode == SecurityMode::FullOblivious)
                cold_omap_->dummy_access();
            else
                cold_omap_->partial_dummy_access();
        } else {
            hot_omap_->dummy_access();
            result.hot_bw = hot_omap_->last_stats();
            result.value = cold_omap_->search(key, new_value);
        }
    }
    result.cold_bw = cold_omap_->last_stats();

    result.total_bw.bytes_downloaded =
        result.hot_bw.bytes_downloaded + result.cold_bw.bytes_downloaded;
    result.total_bw.bytes_uploaded =
        result.hot_bw.bytes_uploaded + result.cold_bw.bytes_uploaded;
    result.total_bw.rounds =
        std::max(result.hot_bw.rounds, result.cold_bw.rounds);

    if (is_hot_logical)
        result.rounds_to_answer = static_cast<int>(result.hot_bw.rounds);
    else
        result.rounds_to_answer = static_cast<int>(result.total_bw.rounds);

    // Standalone maintenance when piggyback is off.
    if (maint_ && !piggyback_on && maint_->should_maintain())
        do_maintenance_step();

    // Piggyback mode: promotions still need standalone handling.
    if (pig_active && maint_->has_pending_promotions())
        do_promotion_standalone();

    return result;
}

void TieredOMap::do_maintenance_step() {
    if (hot_key_list_.empty()) return;

    bool physical = true;

    int idx = maint_->scan_index();
    if (idx >= static_cast<int>(hot_key_list_.size()))
        idx = 0;

    int scan_key = hot_key_list_[idx];

    // ─── Demotion scan ──────────────────────────────────────────────
    // Read the scanned hot entry's metadata.
    Bytes raw = hot_omap_->search(scan_key);
    auto [val, meta] = decode_epoch(raw);
    bool demote = maint_->should_demote(meta);

    if (physical) {
        // B+ tree backend: physically migrate or produce identical dummy ops.
        // Pattern: 2 hot ops + 1 cold op (search already counted as 1st hot).
        if (demote) {
            hot_omap_->remove(scan_key);
            cold_omap_->insert(scan_key, raw);
        } else {
            hot_omap_->dummy_access();
            cold_omap_->dummy_access();
        }
    } else {
        // AVL virtual maintenance: always 1 cold dummy to hide the decision.
        cold_omap_->dummy_access();
    }

    if (demote) {
        hot_keys_.erase(scan_key);
        if (physical) phys_hot_keys_.erase(scan_key);
        auto it = std::lower_bound(
            hot_key_list_.begin(), hot_key_list_.end(), scan_key);
        if (it != hot_key_list_.end() && *it == scan_key)
            hot_key_list_.erase(it);
    }

    maint_->advance_scan(static_cast<int>(hot_key_list_.size()));

    // ─── Promotion check ────────────────────────────────────────────
    int promo_key = INVALID_KEY;
    bool has_promo = maint_->pop_promotion(promo_key);
    bool do_promote = has_promo
                      && !hot_keys_.count(promo_key)
                      && static_cast<int>(hot_keys_.size()) < hot_capacity_;

    if (physical) {
        // Pattern: 1 hot op + 2 cold ops.
        if (do_promote) {
            Bytes cold_raw = cold_omap_->search(promo_key);
            cold_omap_->remove(promo_key);
            hot_omap_->insert(promo_key, cold_raw);
        } else {
            cold_omap_->dummy_access();
            cold_omap_->dummy_access();
            hot_omap_->dummy_access();
        }
    }

    if (do_promote) {
        hot_keys_.insert(promo_key);
        if (physical) phys_hot_keys_.insert(promo_key);
        auto it = std::lower_bound(
            hot_key_list_.begin(), hot_key_list_.end(), promo_key);
        hot_key_list_.insert(it, promo_key);
    }
}

void TieredOMap::do_promotion_standalone() {
    bool physical = true;

    int promo_key = INVALID_KEY;
    bool has_promo = maint_->pop_promotion(promo_key);
    bool do_promote = has_promo
                      && !hot_keys_.count(promo_key)
                      && static_cast<int>(hot_keys_.size()) < hot_capacity_;

    if (physical) {
        if (do_promote) {
            Bytes cold_raw = cold_omap_->search(promo_key);
            cold_omap_->remove(promo_key);
            hot_omap_->insert(promo_key, cold_raw);
        } else {
            cold_omap_->dummy_access();
            cold_omap_->dummy_access();
            hot_omap_->dummy_access();
        }
    }

    if (do_promote) {
        hot_keys_.insert(promo_key);
        if (physical) phys_hot_keys_.insert(promo_key);
        auto it = std::lower_bound(
            hot_key_list_.begin(), hot_key_list_.end(), promo_key);
        hot_key_list_.insert(it, promo_key);
    }
}

void TieredOMap::promote(int key) {
    if (hot_keys_.count(key)) return;

    Bytes raw = cold_omap_->search(key);
    cold_omap_->remove(key);
    hot_omap_->insert(key, raw);
    hot_keys_.insert(key);
    phys_hot_keys_.insert(key);
    auto it = std::lower_bound(hot_key_list_.begin(), hot_key_list_.end(), key);
    hot_key_list_.insert(it, key);
}

void TieredOMap::demote(int key) {
    if (!hot_keys_.count(key)) return;

    Bytes raw = hot_omap_->search(key);
    hot_omap_->remove(key);
    cold_omap_->insert(key, raw);
    hot_keys_.erase(key);
    phys_hot_keys_.erase(key);
    auto it = std::lower_bound(hot_key_list_.begin(), hot_key_list_.end(), key);
    if (it != hot_key_list_.end() && *it == key)
        hot_key_list_.erase(it);
}

}  // namespace tiered_omap
