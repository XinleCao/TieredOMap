#pragma once

#include "tiered_omap/common.h"
#include "tiered_omap/tee/enclave_oram.h"
#include "tiered_omap/tee/packed_directory.h"
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

    // Split-ORAM: upper log2(n) levels of the cold AVL tree go into a
    // separate small ORAM (fits in EPC).  The remainder (cold-lower) is a
    // large ORAM that may exceed EPC.
    bool use_split_oram = true;
};

struct TeeAccessResult {
    Bytes value;
    bool found_in_hot = false;

    // Page-fault metrics (the paper's primary TEE metric).
    uint64_t hot_pages = 0;     // directory scan + hot ORAM
    uint64_t cold_up_pages = 0; // cold-upper (should be 0 if in EPC)
    uint64_t cold_low_pages = 0;// cold-lower (may page)
    uint64_t total_pages = 0;
};

// TEE-mode TieredOMAP.
//
// Architecture (all inside the enclave):
//
//   ┌── EPC ─────────────────────────────────────────┐
//   │  PackedDirectory  hot_dir_   (n × 12 B)        │
//   │  EnclaveOram      hot_oram_  (capacity n)       │
//   │  EnclaveOram      cold_up_   (capacity ~ 2^split_depth) │
//   └────────────────────────────────────────────────┘
//            │
//   ┌── Untrusted / EPC-overflow ───────────────────┐
//   │  EnclaveOram      cold_low_  (capacity N)       │
//   └────────────────────────────────────────────────┘
//
// Access flow:
//   1) Client sends request over WAN.
//   2) Enclave scans hot_dir_, accesses hot_oram_.
//   3) → Send EARLY RESPONSE (hot result) to client.
//   4) Access cold_up_ + cold_low_ (real or dummy depending on mode).
//   5) → Send FINAL RESPONSE to client.
//
// The early_response_cb / final_response_cb callbacks let the server
// layer send network responses at the right moments.
class TeeOmap {
public:
    using ResponseCallback = std::function<void(const Bytes& value, bool found)>;

    explicit TeeOmap(const TeeOmapConfig& config);

    // Bulk-load initial data.  hot_keys specifies which keys start in the
    // hot tier; the rest go into the cold tier.
    void init(const std::vector<std::pair<int, Bytes>>& all_data,
              const std::vector<int>& hot_keys);

    // Main access.  Calls early_cb after hot ORAM completes, then
    // final_cb after cold ORAM completes.
    TeeAccessResult access(int key, const Bytes* new_value = nullptr,
                           ResponseCallback early_cb = nullptr,
                           ResponseCallback final_cb = nullptr);

    // Promote key from cold → hot.
    void promote(int key);
    // Demote key from hot → cold.
    void demote(int key);

    const TeeOmapConfig& config() const { return config_; }
    int hot_count() const { return hot_dir_->size(); }

private:
    // Cold AVL tree operations (emulated with flat ORAM for now).
    // In a full implementation these would be oblivious AVL traversals.
    Bytes cold_search(int key, const Bytes* new_value = nullptr);
    void cold_insert(int key, const Bytes& value);
    void cold_remove(int key);
    void cold_dummy_access();

    TeeOmapConfig config_;
    int split_depth_ = 0;

    std::unique_ptr<PackedDirectory> hot_dir_;
    std::unique_ptr<EnclaveOram> hot_oram_;

    // Cold tier split: cold_up_ for upper tree levels (in EPC),
    // cold_low_ for lower levels (may page).
    // When use_split_oram = false, only cold_low_ is used.
    std::unique_ptr<EnclaveOram> cold_up_;
    std::unique_ptr<EnclaveOram> cold_low_;

    std::unordered_set<int> hot_keys_;
    // Cold data stored directly in the ORAM(s).

    // Simple cold key→value map for Phase 1 correctness testing.
    // Will be replaced by proper oblivious AVL once we have CMOV AVL ops.
    std::unordered_map<int, Bytes> cold_store_;
};

}  // namespace tee
}  // namespace tiered_omap
