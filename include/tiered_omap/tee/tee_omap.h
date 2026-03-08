#pragma once

#include "tiered_omap/common.h"
#include "tiered_omap/tee/enclave_oram.h"
#include "tiered_omap/tee/packed_directory.h"
#include "tiered_omap/tee/tee_avl_omap.h"
#include <functional>
#include <memory>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace tiered_omap {
namespace tee {

enum class TeeSecurityMode {
    FullOblivious,
    TierMembership
};

struct TeeMaintenanceConfig {
    bool enabled = false;
    int epoch_length = 256;
    int promote_threshold = 5;
    int demote_threshold = 2;
    int staleness_epochs = 3;
};

struct TeeOmapConfig {
    int total_keys = 0;        // N
    int hot_set_size = 0;      // n
    int value_size = 256;      // bytes per value
    int bucket_size = 4;       // Z
    TeeSecurityMode mode = TeeSecurityMode::FullOblivious;
    bool use_split_oram = true;
    TeeMaintenanceConfig maintenance;
};

struct TeeAccessResult {
    Bytes value;
    bool found_in_hot = false;

    uint64_t hot_pages = 0;
    uint64_t cold_up_pages = 0;
    uint64_t cold_low_pages = 0;
    uint64_t total_pages = 0;
};

class TeeOmap {
public:
    using ResponseCallback = std::function<void(const Bytes& value, bool found)>;

    explicit TeeOmap(const TeeOmapConfig& config);

    void init(const std::vector<std::pair<int, Bytes>>& all_data,
              const std::vector<int>& hot_keys);

    TeeAccessResult access(int key, const Bytes* new_value = nullptr,
                           ResponseCallback early_cb = nullptr,
                           ResponseCallback final_cb = nullptr);

    void promote(int key);
    void demote(int key);

    const TeeOmapConfig& config() const { return config_; }
    int hot_count() const { return hot_dir_->size(); }
    const std::unordered_set<int>& hot_keys() const { return hot_keys_; }

private:
    // Maintenance helpers.
    void do_maintenance_step();

    TeeOmapConfig config_;

    std::unique_ptr<PackedDirectory> hot_dir_;
    std::unique_ptr<EnclaveOram> hot_oram_;
    std::unique_ptr<TeeAvlOmap> cold_omap_;

    std::unordered_set<int> hot_keys_;
    int hot_capacity_ = 0;

    // ── Enclave-local frequency tracking ────────────────────────────────
    // All in enclave memory → invisible to attacker → no oblivious scan needed.
    struct FreqEntry {
        int count = 0;
        int epoch = 0;
    };
    std::unordered_map<int, FreqEntry> freq_;    // key → frequency
    int current_epoch_ = 1;
    int access_counter_ = 0;
    int scan_ptr_ = 0;
    std::queue<int> promo_queue_;
};

}  // namespace tee
}  // namespace tiered_omap
