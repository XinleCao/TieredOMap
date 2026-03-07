#pragma once

#include "tiered_omap/common.h"
#include <queue>

namespace tiered_omap {

struct MaintenanceConfig {
    int epoch_length = 256;
    int promote_threshold = 5;
    int demote_threshold = 2;
    int staleness_epochs = 3;
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

    // Called after each access. Returns updated epoch metadata to write back.
    // Sets should_promote if this cold key qualifies for promotion.
    EpochMeta on_access(int key, bool is_hot,
                        const EpochMeta& stored, bool& should_promote) {
        should_promote = false;
        EpochMeta updated = stored;

        if (updated.ep < current_epoch_) {
            prev_freq_ = updated.cnt;
            updated.cnt = 1;
            updated.ep = current_epoch_;
        } else {
            prev_freq_ = updated.cnt;
            updated.cnt = updated.cnt + 1;
        }

        if (!is_hot && prev_freq_ >= cfg_.promote_threshold) {
            should_promote = true;
            promotion_queue_.push(key);
        }

        ++access_counter_;
        if (access_counter_ >= cfg_.epoch_length) {
            access_counter_ = 0;
            ++current_epoch_;
        }

        return updated;
    }

    // Check if a maintenance step (scan+demote or promote) should run now.
    bool should_maintain() const {
        return access_counter_ == 0 && current_epoch_ > 0;
    }

    // Will the *next* access tick trigger maintenance?
    // (counter is at epoch_length-1 before the on_access that will wrap it)
    bool should_maintain_next() const {
        return access_counter_ == cfg_.epoch_length - 1;
    }

    // Get the next hot entry index to scan for demotion.
    int scan_index() const { return scan_ptr_; }

    // Advance scan pointer. Call after performing the scan step.
    void advance_scan(int hot_set_size) {
        if (hot_set_size > 0)
            scan_ptr_ = (scan_ptr_ + 1) % hot_set_size;
    }

    // Decide whether to demote based on the scanned entry's metadata.
    bool should_demote(const EpochMeta& meta) const {
        bool stale = meta.ep <= current_epoch_ - cfg_.staleness_epochs;
        bool cold = meta.cnt < cfg_.demote_threshold;
        return stale || cold;
    }

    // Pop a promotion candidate if available.
    bool pop_promotion(int& key) {
        if (promotion_queue_.empty()) return false;
        key = promotion_queue_.front();
        promotion_queue_.pop();
        return true;
    }

    bool has_pending_promotions() const { return !promotion_queue_.empty(); }

    int current_epoch() const { return current_epoch_; }
    int access_count() const { return access_counter_; }
    int last_prev_freq() const { return prev_freq_; }

    const MaintenanceConfig& config() const { return cfg_; }

private:
    MaintenanceConfig cfg_;
    int current_epoch_ = 1;
    int access_counter_ = 0;
    int scan_ptr_ = 0;
    int prev_freq_ = 0;
    std::queue<int> promotion_queue_;
};

}  // namespace tiered_omap
