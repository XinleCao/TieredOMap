#pragma once

#include "tiered_omap/common.h"
#include "tiered_omap/tee/enclave_oram.h"
#include "tiered_omap/tee/packed_directory.h"
#include "tiered_omap/tee/tee_avl_omap.h"
#include <functional>
#include <memory>
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

    void promote(int key, const Bytes& value);
    void demote(int key);

    const TeeOmapConfig& config() const { return config_; }
    int hot_count() const { return hot_dir_->size(); }
    bool is_hot(int key) const {
        return hot_dir_->lookup(key) != INVALID_LEAF;
    }

private:
    void do_maintenance_step();

    // Epoch-aware helpers: decode/encode user value + epoch metadata.
    int stored_value_size() const;
    Bytes wrap_value(const Bytes& val, const EpochMeta& m = {}) const;
    std::pair<Bytes, EpochMeta> unwrap_value(const Bytes& stored) const;
    EpochMeta bump_epoch(const EpochMeta& old_meta) const;

    TeeOmapConfig config_;

    std::unique_ptr<PackedDirectory> hot_dir_;
    std::unique_ptr<EnclaveOram> hot_oram_;
    std::unique_ptr<TeeAvlOmap> cold_omap_;

    int hot_capacity_ = 0;

    int current_epoch_ = 1;
    int access_counter_ = 0;
    int scan_ptr_ = 0;

    // Pending promotion: single key + value saved from a recent cold access.
    int pending_promo_key_ = INVALID_KEY;
    Bytes pending_promo_val_;
};

}  // namespace tee
}  // namespace tiered_omap
