#pragma once

#include "tiered_omap/common.h"
#include "tiered_omap/omap/omap_interface.h"
#include "tiered_omap/oram/path_oram.h"
#include <functional>
#include <memory>
#include <vector>

namespace tiered_omap {

struct BPlusNode {
    bool is_leaf = true;
    bool is_index_leaf = false;      // index mode: keys 1:1 with child_ids (data ORAM block_ids)
    std::vector<int> keys;           // search keys
    std::vector<int> child_ids;      // (internal / index_leaf) ORAM keys or data ORAM block_ids
    std::vector<int> child_leaves;   // (internal only) ORAM leaves of children
    std::vector<Bytes> values;       // (regular leaf only) data values

    Bytes encode() const;
    static BPlusNode decode(const Bytes& raw);
};

class BPlusOmap : public OmapInterface {
public:
    BPlusOmap(int capacity, int order = 8, int bucket_size = 4,
              StorageCreator storage_creator = nullptr);

    BPlusOmap(int capacity, int order, int bucket_size,
              int split_depth, int upper_capacity,
              StorageCreator storage_creator = nullptr);

    void init(const std::vector<std::pair<int, Bytes>>& data) override;

    Bytes export_state() const;
    static std::unique_ptr<BPlusOmap> from_state(
        const uint8_t*& p, std::shared_ptr<TcpChannel> channel);
    int order() const { return order_; }

    Bytes search(int key, const Bytes* update = nullptr) override;
    void insert(int key, const Bytes& value) override;
    void remove(int key) override;
    void dummy_access() override;
    void partial_dummy_access() override;

    Bytes search_piggyback(int key, const Bytes* update,
                           int extra_key, char extra_op,
                           const Bytes* extra_value,
                           Bytes* extra_result) override;

    void set_round_delay_us(int us) override {
        oram_.set_round_delay_us(us);
        if (split_depth_ > 0) upper_oram_.set_round_delay_us(us);
    }
    PathORAM& upper_oram() { return upper_oram_; }
    int split_depth() const { return split_depth_; }
    bool is_split() const { return split_depth_ > 0; }

    const BandwidthStats& last_stats() const override { return last_bw_; }
    const BandwidthStats& total_stats() const override { return total_bw_; }
    void reset_stats() override { last_bw_.reset(); total_bw_.reset(); }

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

    // ── Mid-access decision interface ──
    void set_step_decision_enabled(bool enable) override;
    bool step_needs_decision() const override;
    Bytes step_get_traverse_result() override;
    void step_commit_remove() override;
    void step_commit_noop() override;

    // ── Piggyback interface (concurrent second operation) ──
    void begin_piggyback_search(int key) override;
    void begin_piggyback_insert(int key, const Bytes& value) override;
    void begin_piggyback_dummy() override;
    Bytes finish_piggyback() override;
    void set_piggyback_decision_enabled(bool enable) override;
    bool piggyback_needs_decision() const override;
    Bytes piggyback_get_traverse_result() override;
    void piggyback_commit_remove() override;
    void piggyback_commit_noop() override;

    // ODS mode: used as inner tree by DaOstOmap.
    void set_index_mode(bool m) {
        if (m == index_mode_) return;
        index_mode_ = m;
        if (!ods_mode_) {
            max_height_ += m ? -1 : 1;
            if (max_height_ < 1) max_height_ = 1;
            if (split_depth_ > max_height_)
                split_depth_ = max_height_;
        }
    }
    bool index_mode() const { return index_mode_; }
    int max_leaf_keys() const {
        return index_mode_ ? order_ * (order_ - 1) : order_ - 1;
    }

    void set_ods_mode(int tree_height_bound) {
        ods_mode_ = true;
        max_height_ = tree_height_bound;
    }
    void set_root(int id, int leaf) { root_id_ = id; root_leaf_ = leaf; }
    std::pair<int,int> get_root() const { return {root_id_, root_leaf_}; }
    int last_op_count() const { return op_count_; }
    PathORAM& oram() { return oram_; }
    void set_next_block_id(int id) { next_block_id_ = id; }
    int next_block_id() const { return next_block_id_; }

private:
    struct LocalNode {
        int id;
        int leaf;
        BPlusNode node;
        int parent_id;
        int depth;
    };

    struct CachedSibling {
        int id = INVALID_KEY;
        int leaf = INVALID_LEAF;
        BPlusNode node;
        int parent_local_idx = -1;
        int child_idx_in_parent = -1;
        int depth = 0;
    };

    PathORAM& oram_for_depth(int depth);
    void move_to_local(int id, int leaf, int parent_id, int depth);
    void move_to_sibling_cache(int id, int leaf, int parent_local_idx, int child_idx, int depth);
    int traverse_with_siblings(int key);
    void flush_all_to_stash();
    void reassign_all_leaves();
    void do_dummy_ops(int count);
    void pad_to_budget();
    int split_upper_budget() const;

    static int find_child_index(const BPlusNode& node, int key);
    static int find_leaf_index(const BPlusNode& node, int key);

    int split_leaf(LocalNode& leaf_node);
    int split_internal(LocalNode& internal_node);

    int min_leaf_keys() const;
    void handle_delete_underflow();

    int build_tree(const std::vector<std::pair<int, Bytes>>& sorted,
                   std::unordered_map<int, Bytes>& oram_data);

    int order_ = 8;
    int max_height_ = 0;
    bool index_mode_ = false;
    int root_id_ = INVALID_KEY;
    int root_leaf_ = INVALID_LEAF;
    int next_block_id_ = 0;

    void finalize_bw();

    enum class StepPhase { TRAVERSE_TARGET, TRAVERSE_SIBLING, DECISION, PAD, DONE };
    struct StepState {
        StepPhase phase = StepPhase::DONE;
        int key = INVALID_KEY;
        const Bytes* update = nullptr;
        bool is_dummy = false;
        bool decision_enabled = false;
        int budget = 0;
        int ops = 0;

        int cur_id = INVALID_KEY;
        int cur_leaf = INVALID_LEAF;
        int depth = 0;
        bool leaf_reached = false;

        bool sibling_is_dummy = false;
        int sib_id = INVALID_KEY;
        int sib_leaf = INVALID_LEAF;
        int sib_parent_idx = -1;
        int sib_child_idx = -1;

        int next_id = INVALID_KEY;
        int next_leaf = INVALID_LEAF;

        int pad_remaining = 0;
        int partial_upper_pad = 0;
        int cur_round_leaf = INVALID_LEAF;
        int cur_round_depth = 0;
        bool round_read = false;
        Bytes result;
    };
    StepState ss_;

    struct PBState {
        bool active = false;
        StepPhase phase = StepPhase::DONE;
        int key = INVALID_KEY;
        int cur_id = INVALID_KEY;
        int cur_leaf = INVALID_LEAF;
        int depth = 0;
        int ops = 0;
        int budget = 0;
        int pad_remaining = 0;
        int partial_upper_pad = 0;
        int cur_round_leaf = INVALID_LEAF;
        int cur_round_depth = 0;
        bool round_read = false;
        Bytes result;
        bool node_in_local = false;
        bool traverse_done = false;
        bool leaf_reached = false;
        bool sibling_is_dummy = false;
        int next_id = INVALID_KEY;
        int next_leaf = INVALID_LEAF;
        int sib_id = INVALID_KEY;
        int sib_leaf = INVALID_LEAF;
        int sib_parent_idx = -1;
        int sib_child_idx = -1;
        bool is_insert = false;
        Bytes insert_value;
        bool decision_enabled = false;
    };
    PBState pb_;

    int split_depth_ = 0;
    StorageCreator storage_creator_;
    PathORAM upper_oram_;
    PathORAM oram_;
    std::vector<LocalNode> local_;
    std::vector<CachedSibling> sibling_cache_;
    bool ods_mode_ = false;
    int op_count_ = 0;
    int upper_op_count_ = 0;
    int lower_op_count_ = 0;
    BandwidthStats last_bw_;
    BandwidthStats total_bw_;
};

}  // namespace tiered_omap
