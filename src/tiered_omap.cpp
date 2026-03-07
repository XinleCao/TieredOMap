#include "tiered_omap/tiered_omap.h"
#include "tiered_omap/omap/avl_omap.h"
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

    // When maintenance is enabled, keys migrate between tiers,
    // so both OMAPs need headroom beyond their initial partition size.
    int hot_cap = use_epoch ? std::max(n * 2, n + 64) : std::max(n, 1);
    int cold_cap = use_epoch ? std::max(N_cold, config_.total_keys) : std::max(N_cold, 1);

    hot_omap_ = std::make_unique<AVLOmap>(hot_cap, bs);
    hot_omap_->init(hot_data);

    if (config_.use_split_oram && n > 0) {
        int split_depth = ceil_log2(std::max(n, 2));
        cold_omap_ = std::make_unique<AVLOmap>(
            cold_cap, bs, split_depth, std::max(n, 1));
    } else {
        cold_omap_ = std::make_unique<AVLOmap>(cold_cap, bs);
    }
    cold_omap_->init(cold_data);
}

AccessResult TieredOMap::access(int key, const Bytes* new_value) {
    AccessResult result;
    bool is_hot_logical = hot_keys_.count(key) > 0;
    bool is_hot_physical = phys_hot_keys_.count(key) > 0;
    bool use_epoch = maint_ != nullptr;

    result.found_in_hot = is_hot_logical;

    if (use_epoch) {
        // Route based on physical location; track epoch metadata.
        OmapInterface* target = is_hot_physical ? hot_omap_.get() : cold_omap_.get();
        OmapInterface* other  = is_hot_physical ? cold_omap_.get() : hot_omap_.get();

        auto raw = target->search(key);
        auto [val, meta] = decode_epoch(raw);
        bool promote_flag = false;
        meta = maint_->on_access(key, is_hot_logical, meta, promote_flag);
        if (new_value) {
            Bytes wb = encode_with_epoch(*new_value, meta);
            target->search(key, &wb);
            result.value = *new_value;
        } else {
            Bytes wb = encode_with_epoch(val, meta);
            target->search(key, &wb);
            result.value = val;
        }
        result.hot_bw = hot_omap_->last_stats();
        other->dummy_access();
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

    // Maintenance step at epoch boundary.
    if (maint_ && maint_->should_maintain())
        do_maintenance_step();

    return result;
}

void TieredOMap::do_maintenance_step() {
    if (hot_key_list_.empty()) return;

    int idx = maint_->scan_index();
    if (idx >= static_cast<int>(hot_key_list_.size()))
        idx = 0;

    int scan_key = hot_key_list_[idx];

    // Virtual maintenance: we read the scan target's metadata to make
    // the demotion decision, but only update the logical hot_keys_ set
    // without modifying the actual ORAM trees. This avoids AVL remove
    // stash-consistency issues while accurately tracking hit rate.
    Bytes raw = hot_omap_->search(scan_key);
    auto [val, meta] = decode_epoch(raw);

    if (maint_->should_demote(meta)) {
        hot_keys_.erase(scan_key);
        auto it = std::lower_bound(hot_key_list_.begin(), hot_key_list_.end(), scan_key);
        if (it != hot_key_list_.end() && *it == scan_key)
            hot_key_list_.erase(it);
    } else {
        cold_omap_->dummy_access();
    }

    maint_->advance_scan(static_cast<int>(hot_key_list_.size()));

    int promo_key = INVALID_KEY;
    if (maint_->pop_promotion(promo_key)) {
        if (!hot_keys_.count(promo_key)) {
            hot_keys_.insert(promo_key);
            auto it = std::lower_bound(hot_key_list_.begin(), hot_key_list_.end(), promo_key);
            hot_key_list_.insert(it, promo_key);
        }
    }
}

void TieredOMap::promote(int key) {
    if (hot_keys_.count(key)) return;

    Bytes raw = cold_omap_->search(key);
    cold_omap_->remove(key);
    hot_omap_->insert(key, raw);
    hot_keys_.insert(key);
    auto it = std::lower_bound(hot_key_list_.begin(), hot_key_list_.end(), key);
    hot_key_list_.insert(it, key);
}

void TieredOMap::demote(int key) {
    if (!hot_keys_.count(key)) return;

    Bytes raw = hot_omap_->search(key);
    hot_omap_->remove(key);
    cold_omap_->insert(key, raw);
    hot_keys_.erase(key);
    auto it = std::lower_bound(hot_key_list_.begin(), hot_key_list_.end(), key);
    if (it != hot_key_list_.end() && *it == key)
        hot_key_list_.erase(it);
}

}  // namespace tiered_omap
