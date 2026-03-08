#pragma once

#include "tiered_omap/common.h"
#include "tiered_omap/tee/enclave_oram.h"
#include "tiered_omap/tee/packed_directory.h"
#include "tiered_omap/tee/tee_avl_omap.h"
#include <functional>
#include <memory>
#include <unordered_set>
#include <vector>

namespace tiered_omap {
namespace tee {

enum class TeeSecurityMode {
    FullOblivious,
    TierMembership
};

struct TeeOmapConfig {
    int total_keys = 0;        // N
    int hot_set_size = 0;      // n
    int value_size = 256;      // bytes per value
    int bucket_size = 4;       // Z
    TeeSecurityMode mode = TeeSecurityMode::FullOblivious;

    // Split-ORAM: upper levels of the cold AVL tree use a small ORAM
    // (fits in EPC).  The lower portion uses a large ORAM (may exceed EPC).
    bool use_split_oram = true;
};

struct TeeAccessResult {
    Bytes value;
    bool found_in_hot = false;

    // Page-fault metrics (the paper's primary TEE metric).
    uint64_t hot_pages = 0;     // directory scan + hot ORAM
    uint64_t cold_up_pages = 0; // cold-upper (should be ~0 if in EPC)
    uint64_t cold_low_pages = 0;// cold-lower (may page)
    uint64_t total_pages = 0;
};

// TEE-mode TieredOMAP.
//
// Architecture (all inside the enclave):
//
//   ┌── EPC ─────────────────────────────────────────────────┐
//   │  PackedDirectory  hot_dir_     (n × 12 B)              │
//   │  EnclaveOram      hot_oram_    (capacity n)             │
//   │  TeeAvlOmap       cold_omap_   (split: upper in EPC)    │
//   └────────────────────────────────────────────────────────┘
//            │ (cold_omap_'s lower ORAM may exceed EPC)
//
// Access flow:
//   1) Client sends request over WAN.
//   2) Enclave scans hot_dir_, accesses hot_oram_.
//   3) → Send EARLY RESPONSE (hot result) to client.
//   4) Access cold_omap_ (real search or dummy, depending on mode).
//   5) → Send FINAL RESPONSE to client.
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

private:
    TeeOmapConfig config_;

    std::unique_ptr<PackedDirectory> hot_dir_;
    std::unique_ptr<EnclaveOram> hot_oram_;
    std::unique_ptr<TeeAvlOmap> cold_omap_;

    std::unordered_set<int> hot_keys_;
};

}  // namespace tee
}  // namespace tiered_omap
