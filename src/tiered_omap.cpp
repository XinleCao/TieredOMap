#include "tiered_omap/tiered_omap.h"
#include "tiered_omap/omap/avl_omap.h"
#include <algorithm>
#include <stdexcept>

namespace tiered_omap {

TieredOMap::TieredOMap(const TieredOMapConfig& config)
    : config_(config) {}

void TieredOMap::init(
    const std::vector<std::pair<int, Bytes>>& all_data,
    const std::vector<int>& hot_keys) {
    hot_keys_ = {hot_keys.begin(), hot_keys.end()};

    std::vector<std::pair<int, Bytes>> hot_data, cold_data;
    for (auto& [k, v] : all_data) {
        if (hot_keys_.count(k))
            hot_data.emplace_back(k, v);
        else
            cold_data.emplace_back(k, v);
    }

    int n = static_cast<int>(hot_data.size());
    int N_cold = static_cast<int>(cold_data.size());
    int bs = config_.bucket_size;

    // Hot OMAP: standard AVL OMAP with n entries.
    hot_omap_ = std::make_unique<AVLOmap>(std::max(n, 1), bs);
    hot_omap_->init(hot_data);

    if (config_.use_split_oram && n > 0) {
        int split_depth = ceil_log2(std::max(n, 2));
        cold_omap_ = std::make_unique<AVLOmap>(
            std::max(N_cold, 1), bs, split_depth, std::max(n, 1));
    } else {
        cold_omap_ = std::make_unique<AVLOmap>(std::max(N_cold, 1), bs);
    }
    cold_omap_->init(cold_data);
}

AccessResult TieredOMap::access(int key, const Bytes* new_value) {
    AccessResult result;
    bool is_hot = hot_keys_.count(key) > 0;

    // Phase 1: Search hot OMAP.
    if (is_hot) {
        result.value = hot_omap_->search(key, new_value);
        result.found_in_hot = true;
    } else {
        hot_omap_->dummy_access();
        result.found_in_hot = false;
    }
    result.hot_bw = hot_omap_->last_stats();

    // Phase 2: Search cold OMAP.
    if (config_.mode == SecurityMode::FullOblivious) {
        // Full obliviousness: always complete both traversals.
        if (is_hot) {
            cold_omap_->dummy_access();
        } else {
            result.value = cold_omap_->search(key, new_value);
        }
    } else {
        // Tier-membership privacy: hot queries traverse cold upper + 1 dummy lower.
        if (is_hot) {
            cold_omap_->partial_dummy_access();
        } else {
            result.value = cold_omap_->search(key, new_value);
        }
    }
    result.cold_bw = cold_omap_->last_stats();

    // Hot and cold OMAPs run in parallel: rounds = max, bandwidth = sum.
    result.total_bw.bytes_downloaded =
        result.hot_bw.bytes_downloaded + result.cold_bw.bytes_downloaded;
    result.total_bw.bytes_uploaded =
        result.hot_bw.bytes_uploaded + result.cold_bw.bytes_uploaded;
    result.total_bw.rounds =
        std::max(result.hot_bw.rounds, result.cold_bw.rounds);

    if (is_hot)
        result.rounds_to_answer = static_cast<int>(result.hot_bw.rounds);
    else
        result.rounds_to_answer = static_cast<int>(result.total_bw.rounds);

    return result;
}

void TieredOMap::promote(int key) {
    if (hot_keys_.count(key)) return;  // Already hot.

    Bytes val = cold_omap_->search(key);
    cold_omap_->remove(key);
    hot_omap_->insert(key, val);
    hot_keys_.insert(key);
}

void TieredOMap::demote(int key) {
    if (!hot_keys_.count(key)) return;  // Already cold.

    Bytes val = hot_omap_->search(key);
    hot_omap_->remove(key);
    cold_omap_->insert(key, val);
    hot_keys_.erase(key);
}

}  // namespace tiered_omap
