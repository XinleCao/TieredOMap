#pragma once

#include "tiered_omap/common.h"
#include "tiered_omap/maintenance.h"
#include "tiered_omap/network/storage_interface.h"
#include "tiered_omap/omap/omap_interface.h"
#include "tiered_omap/oram/path_oram.h"
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace tiered_omap {

class TcpChannel;

enum class SecurityMode {
    FullOblivious,
    TierMembership
};

enum class OmapBackend {
    AVL,
    BPlus,
    DaAvl,
    DaBplus
};

struct TieredOMapConfig {
    int total_keys = 0;
    int hot_set_size = 0;
    SecurityMode mode = SecurityMode::FullOblivious;
    bool use_split_oram = false;
    int bucket_size = 4;
    OmapBackend backend = OmapBackend::AVL;       // cold backend (and default hot)
    OmapBackend hot_backend = OmapBackend::AVL;    // hot backend (use_hot_backend=false → same as backend)
    bool use_hot_backend = false;                  // true → hot uses hot_backend instead of backend
    int bplus_order = 8;
    MaintenanceConfig maintenance;
    StorageCreator storage_creator;

    OmapBackend effective_hot_backend() const {
        return use_hot_backend ? hot_backend : backend;
    }
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

    void set_channel(std::shared_ptr<TcpChannel> ch) { channel_ = std::move(ch); }

    SecurityMode mode() const { return config_.mode; }
    int hot_set_size() const { return static_cast<int>(hot_keys_.size()); }
    int cold_set_size() const { return config_.total_keys - hot_set_size(); }
    bool maintenance_enabled() const { return config_.maintenance.enabled; }

    OmapInterface* hot_omap() { return hot_omap_.get(); }
    OmapInterface* cold_omap() { return cold_omap_.get(); }
    PathORAM& data_oram() { return data_oram_; }
    const TieredOMapConfig& config() const { return config_; }
    const std::unordered_set<int>& hot_keys() const { return hot_keys_; }

    Bytes export_state() const;
    static std::unique_ptr<TieredOMap> from_state(
        const uint8_t*& p, std::shared_ptr<TcpChannel> channel);

    // Ordered list of hot keys for sequential scan demotion.
    const std::vector<int>& hot_key_list() const { return hot_key_list_; }
    MaintenanceManager* maintenance_mgr() { return maint_.get(); }

private:
    Bytes data_access(int blk, const Bytes* new_value = nullptr) {
        if (blk < 0) { data_oram_.dummy_access(); return {}; }
        return data_oram_.access(blk, new_value);
    }

    void do_maintenance_step();
    void do_promotion_standalone();
    void do_da_maintenance_step();

    static bool is_da(OmapBackend be) {
        return be == OmapBackend::DaAvl || be == OmapBackend::DaBplus;
    }
    bool is_da_backend() const { return is_da(config_.backend); }
    bool is_da_hot() const { return is_da(config_.effective_hot_backend()); }

    AccessResult interleaved_access(int key, const Bytes* new_value,
                                    bool use_partial_dummy = false,
                                    bool epoch_mode = false);
    AccessResult interleaved_da_piggyback(int key, const Bytes* new_value);
    void run_interleaved_loop(OmapInterface* a, OmapInterface* b);

    void do_interleaved_maintenance();
    struct CachedItem { int key = INVALID_KEY; Bytes data_ref; };
    std::deque<CachedItem> pending_demotions_;
    std::deque<CachedItem> pending_promotions_;

    // Cache-based maintenance state (paper Algorithm 2)
    std::unordered_set<int> cache_keys_;
    std::unordered_map<int, Bytes> cache_data_refs_;
    int pending_insert_key_ = INVALID_KEY;
    Bytes pending_insert_ref_;

    TieredOMapConfig config_;
    std::shared_ptr<TcpChannel> channel_;
    std::unique_ptr<OmapInterface> hot_omap_;
    std::unique_ptr<OmapInterface> cold_omap_;
    PathORAM data_oram_;
    int next_data_block_id_ = 0;
    std::unordered_set<int> hot_keys_;
    std::unordered_set<int> phys_hot_keys_;
    std::vector<int> hot_key_list_;
    std::unique_ptr<MaintenanceManager> maint_;
    int hot_capacity_ = 0;

    std::deque<int> da_pending_demotions_;
};

}  // namespace tiered_omap
