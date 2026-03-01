#pragma once

#include "tiered_omap/common.h"
#include "tiered_omap/omap/omap_interface.h"
#include <memory>
#include <unordered_set>
#include <vector>

namespace tiered_omap {

enum class SecurityMode {
    FullOblivious,     // Both OMAPs traversed fully on every query.
    TierMembership     // Hot queries can skip the cold lower ORAM.
};

struct TieredOMapConfig {
    int total_keys = 0;        // N
    int hot_set_size = 0;      // n
    SecurityMode mode = SecurityMode::FullOblivious;
    bool use_split_oram = false;
    int bucket_size = 4;
};

struct AccessResult {
    Bytes value;
    bool found_in_hot = false;
    BandwidthStats hot_bw;
    BandwidthStats cold_bw;
    BandwidthStats total_bw;
    int rounds_to_answer = 0;  // When the client actually got the answer.
};

class TieredOMap {
public:
    explicit TieredOMap(const TieredOMapConfig& config);

    // Initialize with all key-value pairs + the set of hot keys.
    void init(const std::vector<std::pair<int, Bytes>>& all_data,
              const std::vector<int>& hot_keys);

    // Oblivious access: always queries both hot and cold OMAPs.
    AccessResult access(int key, const Bytes* new_value = nullptr);

    // Promote a key from cold to hot tier.
    void promote(int key);

    // Demote a key from hot to cold tier.
    void demote(int key);

    SecurityMode mode() const { return config_.mode; }
    int hot_set_size() const { return static_cast<int>(hot_keys_.size()); }
    int cold_set_size() const { return config_.total_keys - hot_set_size(); }

private:
    TieredOMapConfig config_;
    std::unique_ptr<OmapInterface> hot_omap_;
    std::unique_ptr<OmapInterface> cold_omap_;
    std::unordered_set<int> hot_keys_;
};

}  // namespace tiered_omap
