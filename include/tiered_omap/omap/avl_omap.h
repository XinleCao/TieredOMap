#pragma once

#include "tiered_omap/common.h"
#include "tiered_omap/omap/omap_interface.h"
#include "tiered_omap/oram/path_oram.h"
#include <functional>
#include <memory>
#include <vector>

namespace tiered_omap {

constexpr int AVL_HEADER_SIZE = 6 * sizeof(int);  // 24 bytes

struct AVLNodeData {
    Bytes data;
    int l_key = INVALID_KEY, l_leaf = INVALID_LEAF, l_height = 0;
    int r_key = INVALID_KEY, r_leaf = INVALID_LEAF, r_height = 0;

    int height() const { return 1 + std::max(l_height, r_height); }
    int balance() const { return l_height - r_height; }

    Bytes encode() const;
    static AVLNodeData decode(const Bytes& raw);
};

class AVLOmap : public OmapInterface {
public:
    explicit AVLOmap(int capacity, int bucket_size = 4,
                     StorageCreator storage_creator = nullptr);

    AVLOmap(int capacity, int bucket_size, int split_depth, int upper_capacity,
            StorageCreator storage_creator = nullptr);

    void init(const std::vector<std::pair<int, Bytes>>& data) override;

    Bytes export_state() const;
    static std::unique_ptr<AVLOmap> from_state(
        const uint8_t*& p, std::shared_ptr<TcpChannel> channel);
    PathORAM& upper_oram() { return upper_oram_; }
    int capacity() const { return capacity_; }
    int split_depth() const { return split_depth_; }

    Bytes search(int key, const Bytes* update = nullptr) override;
    void insert(int key, const Bytes& value) override;
    void remove(int key) override;
    void dummy_access() override;
    void partial_dummy_access() override;

    void set_round_delay_us(int us) override {
        oram_.set_round_delay_us(us);
        if (split_depth_ > 0) upper_oram_.set_round_delay_us(us);
    }

    const BandwidthStats& last_stats() const override { return last_bw_; }
    const BandwidthStats& total_stats() const override { return total_bw_; }
    void reset_stats() override { last_bw_.reset(); total_bw_.reset(); }

    int root_key() const { return root_key_; }
    int max_height() const { return max_height_; }
    bool is_split() const { return split_depth_ > 0; }

    bool supports_interleaved() const override { return true; }
    void begin_step_search(int key, const Bytes* update = nullptr) override;
    void begin_step_dummy() override;
    void begin_step_partial_dummy() override;
    OramStepRound step_next_round() override;
    void step_apply_reads(const std::vector<PathData>& results) override;
    void step_process() override;
    std::vector<StepWriteReq> step_prepare_writes() override;
    bool step_done() const override;
    Bytes step_finish() override;
    void step_abort() override;

    // ── Piggyback interface (concurrent second operation) ──
    void begin_piggyback_search(int key) override;
    void begin_piggyback_insert(int key, const Bytes& value) override;
    void begin_piggyback_dummy() override;
    Bytes finish_piggyback() override;
    bool piggyback_needs_decision() const override;
    Bytes piggyback_get_traverse_result() override;
    void piggyback_commit_remove() override;
    void piggyback_commit_noop() override;

    // Non-step piggyback: two sequential operations sharing 6h budget
    Bytes search_piggyback(int key, const Bytes* update,
                           int extra_key, char extra_op,
                           const Bytes* extra_value,
                           Bytes* extra_result) override;

    // ODS mode: used as inner tree by DaOstOmap.
    void set_ods_mode(int tree_height_bound) {
        ods_mode_ = true;
        max_height_ = tree_height_bound;
    }
    void set_root(int key, int leaf) { root_key_ = key; root_leaf_ = leaf; }
    std::pair<int,int> get_root() const { return {root_key_, root_leaf_}; }
    int last_op_count() const { return op_count_; }
    PathORAM& oram() { return oram_; }

private:
    struct LocalNode {
        int key;
        int leaf;
        AVLNodeData avl;
        int parent_key;
        int depth;          // tree depth for split ORAM routing
    };

    PathORAM& oram_for_depth(int depth);
    void move_to_local(int key, int leaf, int parent_key, int depth);
    void flush_local_to_stash();
    void reassign_leaves();
    void pad_to_budget(int budget);
    void tracked_dummy(int depth);

    void update_heights();
    void rebalance();
    std::tuple<int, int, int> balance_node(int idx);
    std::tuple<int, int, int> rotate(int idx, bool left);

    int build_balanced(const std::vector<std::pair<int, Bytes>>& sorted,
                       int lo, int hi,
                       std::unordered_map<int, Bytes>& oram_data);

    void finalize_bw();
    void reset_op_counts();

    enum class StepPhase { TRAVERSE, PAD, DONE };
    struct StepState {
        StepPhase phase = StepPhase::DONE;
        int key = INVALID_KEY;
        const Bytes* update = nullptr;
        bool is_dummy = false;
        int cur_key = INVALID_KEY;
        int cur_leaf = INVALID_LEAF;
        int depth = 0;
        int budget = 0;
        int ops = 0;
        int pad_remaining = 0;
        int dummy_step = 0;
        int dummy_split_boundary = 0;
        PathORAM* cur_round_oram = nullptr;
        int cur_round_leaf = INVALID_LEAF;
        bool round_read = false;
        Bytes result;
    };
    StepState ss_;

    struct PBState {
        bool active = false;
        StepPhase phase = StepPhase::DONE;
        int key = INVALID_KEY;
        int cur_key = INVALID_KEY;
        int cur_leaf = INVALID_LEAF;
        int depth = 0;
        int ops = 0;
        int budget = 0;
        int pad_remaining = 0;
        int dummy_step = 0;
        int dummy_split_boundary = 0;
        PathORAM* cur_round_oram = nullptr;
        int cur_round_leaf = INVALID_LEAF;
        bool round_read = false;
        Bytes result;
        bool node_in_local = false;
        bool traverse_done = false;
        bool is_insert = false;
        Bytes insert_value;
    };
    PBState pb_;

    int capacity_ = 0;
    int max_height_ = 0;
    int root_key_ = INVALID_KEY;
    int root_leaf_ = INVALID_LEAF;

    int split_depth_ = 0;
    StorageCreator storage_creator_;
    PathORAM upper_oram_;
    PathORAM oram_;

    std::vector<LocalNode> local_;
    bool ods_mode_ = false;
    int op_count_ = 0;          // used in non-split mode
    int upper_op_count_ = 0;    // split mode: ops on upper_oram
    int lower_op_count_ = 0;    // split mode: ops on oram_ (lower)
    BandwidthStats last_bw_;
    BandwidthStats total_bw_;
};

}  // namespace tiered_omap
