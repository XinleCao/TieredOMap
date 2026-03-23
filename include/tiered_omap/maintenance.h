#pragma once

#include "tiered_omap/common.h"
#include <queue>

namespace tiered_omap {

struct MaintenanceConfig {
    int observation_window = 65536;   // B_obs: frequency estimation window
    int swap_interval = 32;           // B_swap: maintenance step frequency
    int promote_threshold = 5;        // θ_p
    int demote_threshold = 2;         // θ_d
    int staleness_windows = 1;        // Δ: stale if ep ≤ obs_epoch - Δ
    bool enabled = false;
    bool piggyback = false;
};

enum class MaintenanceAction { None, Promote, Demote };

struct MaintenanceDecision {
    MaintenanceAction action = MaintenanceAction::None;
    int key = INVALID_KEY;
};

class MaintenanceManager {
public:
    explicit MaintenanceManager(const MaintenanceConfig& cfg)
        : cfg_(cfg) {}

    // Called after each access. Updates epoch metadata and detects promotions.
    // The observation epoch e_obs = floor(total_accesses / B_obs) controls
    // frequency resets; the swap counter triggers maintenance every B_swap.
    EpochMeta on_access(int key, bool is_hot,
                        const EpochMeta& stored, bool& should_promote) {
        should_promote = false;
        EpochMeta updated = stored;

        bool epoch_changed = (updated.ep < obs_epoch_);
        if (epoch_changed) {
            prev_freq_ = updated.cnt;
            updated.cnt = 1;
            updated.ep = obs_epoch_;
        } else {
            updated.cnt = updated.cnt + 1;
        }

        if (epoch_changed && !is_hot
            && prev_freq_ >= cfg_.promote_threshold) {
            should_promote = true;
            promotion_queue_.push(key);
        }

        ++total_accesses_;
        obs_epoch_ = total_accesses_ / cfg_.observation_window;
        swap_counter_ = total_accesses_ % cfg_.swap_interval;

        return updated;
    }

    // Maintenance triggers right after a swap boundary (counter wraps to 0).
    // Skip epoch 0: no reliable frequency data until a full window completes.
    bool should_maintain() const {
        return swap_counter_ == 0 && total_accesses_ > 0 && obs_epoch_ > 0;
    }

    // Will the next access trigger maintenance?
    bool should_maintain_next() const {
        return (total_accesses_ + 1) % cfg_.swap_interval == 0;
    }

    int scan_index() const { return scan_ptr_; }

    void advance_scan(int hot_set_size) {
        if (hot_set_size > 0)
            scan_ptr_ = (scan_ptr_ + 1) % hot_set_size;
    }

    // Demotion: only use the previous completed epoch's data.
    bool should_demote(const EpochMeta& meta) const {
        if (meta.ep == obs_epoch_) return false;
        if (meta.ep == obs_epoch_ - 1)
            return meta.cnt < cfg_.demote_threshold;
        return true;
    }

    bool pop_promotion(int& key) {
        if (promotion_queue_.empty()) return false;
        key = promotion_queue_.front();
        promotion_queue_.pop();
        return true;
    }

    bool has_pending_promotions() const { return !promotion_queue_.empty(); }

    int obs_epoch() const { return obs_epoch_; }
    int total_access_count() const { return total_accesses_; }
    int swap_count() const { return swap_counter_; }
    int last_prev_freq() const { return prev_freq_; }

    const MaintenanceConfig& config() const { return cfg_; }

private:
    MaintenanceConfig cfg_;
    int obs_epoch_ = 0;
    int total_accesses_ = 0;
    int swap_counter_ = 0;
    int scan_ptr_ = 0;
    int prev_freq_ = 0;
    std::queue<int> promotion_queue_;
};

}  // namespace tiered_omap
