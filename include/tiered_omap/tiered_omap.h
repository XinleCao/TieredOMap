#pragma once

#include "tiered_omap/common.h"
#include "tiered_omap/maintenance.h"
#include "tiered_omap/omap/omap_interface.h"
#include <memory>
#include <unordered_set>
#include <vector>

namespace tiered_omap {

enum class SecurityMode {
    FullOblivious,
    TierMembership
};

enum class OmapBackend {
    AVL,
    BPlus
};

struct TieredOMapConfig {
    int total_keys = 0;
    int hot_set_size = 0;
    SecurityMode mode = SecurityMode::FullOblivious;
    bool use_split_oram = false;
    int bucket_size = 4;
    OmapBackend backend = OmapBackend::AVL;
    int bplus_order = 8;
    MaintenanceConfig maintenance;
};

struct AccessResult {
    Bytes value;
    bool found_in_hot = false;
    BandwidthStats hot_bw;
    BandwidthStats cold_bw;
    BandwidthStats total_bw;
    int rounds_to_answer = 0;
};

class TieredOMap {
public:
    explicit TieredOMap(const TieredOMapConfig& config);

    void init(const std::vector<std::pair<int, Bytes>>& all_data,
              const std::vector<int>& hot_keys);

    AccessResult access(int key, const Bytes* new_value = nullptr);

    void promote(int key);
    void demote(int key);

    void set_round_delay_us(int us) {
        if (hot_omap_) hot_omap_->set_round_delay_us(us);
        if (cold_omap_) cold_omap_->set_round_delay_us(us);
    }

    SecurityMode mode() const { return config_.mode; }
    int hot_set_size() const { return static_cast<int>(hot_keys_.size()); }
    int cold_set_size() const { return config_.total_keys - hot_set_size(); }
    bool maintenance_enabled() const { return config_.maintenance.enabled; }

    // Ordered list of hot keys for sequential scan demotion.
    const std::vector<int>& hot_key_list() const { return hot_key_list_; }
    MaintenanceManager* maintenance_mgr() { return maint_.get(); }

private:
    void do_maintenance_step();
    void do_promotion_standalone();

    TieredOMapConfig config_;
    std::unique_ptr<OmapInterface> hot_omap_;
    std::unique_ptr<OmapInterface> cold_omap_;
    std::unordered_set<int> hot_keys_;
    std::unordered_set<int> phys_hot_keys_;
    std::vector<int> hot_key_list_;
    std::unique_ptr<MaintenanceManager> maint_;
    int hot_capacity_ = 0;
};

}  // namespace tiered_omap
