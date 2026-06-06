#pragma once

#include "tiered_omap/common.h"
#include "tiered_omap/omap/omap_interface.h"
#include "tiered_omap/omap/avl_omap.h"
#include "tiered_omap/omap/bplus_omap.h"
#include "tiered_omap/oram/da_oram.h"

namespace tiered_omap {

class TcpChannel;
enum class OdsTreeType { AVL, BPlus };

class DaOstOmap : public OmapInterface {
public:
    // capacity  = total number of KV pairs.
    // num_positions = number of hash buckets (default: same as capacity).
    DaOstOmap(int capacity, OdsTreeType tree_type = OdsTreeType::AVL,
              int num_positions = 0, int bucket_size = 4, int bplus_order = 8,
              StorageCreator storage_creator = nullptr);

    void init(const std::vector<std::pair<int, Bytes>>& data) override;

    Bytes search(int key, const Bytes* update = nullptr) override;
    void insert(int key, const Bytes& value) override;
    void remove(int key) override;
    void dummy_access() override;
    void partial_dummy_access() override;

    void set_round_delay_us(int us) override;

    const BandwidthStats& last_stats() const override { return last_bw_; }
    const BandwidthStats& total_stats() const override { return total_bw_; }
    void reset_stats() override { last_bw_.reset(); total_bw_.reset(); }

    bool supports_interleaved() const override { return true; }
    void begin_step_search(int key, const Bytes* update = nullptr) override;
    void begin_step_search_update(int key, const UpdateFn& update_fn) override;
    void begin_step_dummy() override;
    void begin_step_partial_dummy() override;
    OramStepRound step_next_round() override;
    void step_apply_reads(const std::vector<PathData>& results) override;
    void step_process() override;
    std::vector<StepWriteReq> step_prepare_writes() override;
    bool step_done() const override;
    Bytes step_finish() override;
    void step_abort() override;

    int num_positions() const { return num_positions_; }
    int tree_height_bound() const { return tree_height_bound_; }
    int ods_budget() const { return ods_budget_; }
    OdsTreeType tree_type() const { return tree_type_; }
    int bucket_size_val() const { return bucket_size_; }
    int bplus_order_val() const { return bplus_order_; }
    uint64_t hash_seed() const { return hash_seed_; }
    DAOram& daoram() { return daoram_; }
    AVLOmap& avl_ods() { return avl_ods_; }
    BPlusOmap& bplus_ods() { return bplus_ods_; }
    const std::unordered_map<int, std::pair<int,int>>& root_cache() const { return root_cache_; }

    Bytes export_state() const;
    static std::unique_ptr<DaOstOmap> from_state(
        const uint8_t*& p, std::shared_ptr<TcpChannel> channel);

    // ── Mid-access decision interface (delegates to inner ODS) ──
    void set_step_decision_enabled(bool enable) override;
    bool step_needs_decision() const override;
    Bytes step_get_traverse_result() override;
    void step_commit_remove() override;
    void step_commit_noop() override;

    // ── Piggyback interface (concurrent second operation on this OMAP) ──
    void begin_piggyback_insert(int key, const Bytes& value) override;
    void begin_piggyback_search(int key) override;
    void begin_piggyback_dummy() override;
    Bytes finish_piggyback() override;

    // Two-pointer piggyback scan: scans one DAORAM position per call.
    // Returns the root key and its stored value if the position is non-empty.
    ScanResult piggyback_scan_step();

    void begin_step_scan() override;
    ScanResult step_finish_scan() override;
    void reset_scan() { scan_pos_ = 0; }
    int scan_period() const { return num_positions_; }

private:
    int hash_to_position(int key) const;

    static Bytes encode_root(int key, int leaf);
    static std::pair<int,int> decode_root(const Bytes& b);

    void do_avl_init(const std::vector<std::pair<int, Bytes>>& data);
    void do_bplus_init(const std::vector<std::pair<int, Bytes>>& data);

    // Runs ods_budget_ - actual_ops dummy ODS rounds.
    void pad_ods(int actual_ops);
    void finalize_bw(int ods_ops);

    int capacity_;
    int num_positions_;
    int tree_height_bound_;
    int ods_budget_;
    OdsTreeType tree_type_;
    int bucket_size_;
    int bplus_order_;
    StorageCreator storage_creator_;

    DAOram daoram_;
    AVLOmap avl_ods_;
    BPlusOmap bplus_ods_;

    uint64_t hash_seed_ = 0;

    // Client-side root cache: the authoritative source of (root_key, root_leaf)
    // for each position. DAORAM access is used purely for obliviousness.
    std::unordered_map<int, std::pair<int,int>> root_cache_;

    int scan_pos_ = 0;

    enum class StepPhase { DAORAM, ODS, ODS_PAD, SCAN_DAORAM, SCAN_ODS, SCAN_ODS_PAD, DONE };
    struct StepState {
        StepPhase phase = StepPhase::DONE;
        StepPhase round_phase = StepPhase::DONE;
        bool is_dummy = false;
        bool is_partial_dummy = false;
        bool is_scan = false;
        int key = INVALID_KEY;
        const Bytes* update = nullptr;
        UpdateFn update_fn;
        int pos = -1;

        int ods_ops = 0;
        int pad_remaining = 0;
        int pad_round_leaf = INVALID_LEAF;

        int scan_pos = -1;
        int scan_root_key = INVALID_KEY;
        int scan_root_leaf = INVALID_LEAF;
        Bytes scan_result;

        Bytes result;
    };
    StepState ss_;

    struct PBState {
        bool active = false;
        bool is_insert = false;
        bool is_dummy = false;
        int key = INVALID_KEY;
        Bytes insert_value;
        int pos = -1;
        int saved_main_root_key = INVALID_KEY;
        int saved_main_root_leaf = INVALID_LEAF;
        Bytes result;
    };
    PBState pb_;

    OmapInterface& ods_omap();
    const OmapInterface& ods_omap() const;
    PathORAM& ods_oram();

    BandwidthStats last_bw_;
    BandwidthStats total_bw_;
};

}  // namespace tiered_omap
