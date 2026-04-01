#pragma once

#include "tiered_omap/common.h"
#include <utility>
#include <vector>

namespace tiered_omap {

using PathData = std::unordered_map<int, std::vector<Block>>;

struct StepReadReq {
    int store_id = -1;
    int leaf = INVALID_LEAF;
};

struct StepWriteReq {
    int store_id = -1;
    PathData data;
};

struct OramStepRound {
    std::vector<StepReadReq> reads;
};

class OmapInterface {
public:
    virtual ~OmapInterface() = default;

    virtual Bytes search(int key, const Bytes* update = nullptr) = 0;
    virtual void insert(int key, const Bytes& value) = 0;
    virtual void remove(int key) = 0;
    virtual void dummy_access() = 0;
    virtual void partial_dummy_access() { dummy_access(); }

    virtual void init(const std::vector<std::pair<int, Bytes>>& data) = 0;

    virtual void set_round_delay_us(int /*us*/) {}

    // Piggyback interface: perform two B+ tree operations within one round
    // budget (2*max_height). The main op is search(key); the extra op is on
    // extra_key.  extra_op: 's' = scan-read, 'd' = delete, 'i' = insert.
    // Returns the main search result.  Default falls back to separate calls.
    virtual Bytes search_piggyback(int key, const Bytes* update,
                                   int extra_key, char extra_op,
                                   const Bytes* extra_value,
                                   Bytes* extra_result) {
        (void)extra_key; (void)extra_op; (void)extra_value; (void)extra_result;
        return search(key, update);
    }

    // Replace this access's dummy with a real insert (for migration target).
    virtual void insert_replacing_dummy(int key, const Bytes& value) {
        insert(key, value);
    }

    virtual const BandwidthStats& last_stats() const = 0;
    virtual const BandwidthStats& total_stats() const = 0;
    virtual void reset_stats() = 0;

    // Step-by-step interface for interleaved (batched) access.
    // Each round: step_next_round → batch read → step_apply_reads →
    //             step_process → step_prepare_writes → batch write.
    virtual bool supports_interleaved() const { return false; }
    virtual void begin_step_search(int /*key*/, const Bytes* /*update*/ = nullptr) {}
    virtual void begin_step_dummy() {}
    virtual void begin_step_partial_dummy() { begin_step_dummy(); }
    virtual OramStepRound step_next_round() { return {}; }
    virtual void step_apply_reads(const std::vector<PathData>& /*results*/) {}
    virtual void step_process() {}
    virtual std::vector<StepWriteReq> step_prepare_writes() { return {}; }
    virtual bool step_done() const { return true; }
    virtual Bytes step_finish() { return {}; }
    virtual void step_abort() {}  // TM early termination: finalize BW for completed rounds

    struct ScanResult {
        int key = INVALID_KEY;
        Bytes value;
    };
    virtual void begin_step_scan() {}
    virtual ScanResult step_finish_scan() { return {}; }

    // ── Mid-access decision interface ──
    // After TRAVERSE completes, the step machine pauses in DECISION phase.
    // Caller reads data ORAM externally, then commits to remove or noop.
    // The remaining rounds execute delete+pad or pure pad (type-hidden).
    virtual void set_step_decision_enabled(bool /*enable*/) {}
    virtual bool step_needs_decision() const { return false; }
    virtual Bytes step_get_traverse_result() { return {}; }
    virtual void step_commit_remove() {}
    virtual void step_commit_noop() {}

    virtual void set_piggyback_decision_enabled(bool /*enable*/) {}
    // piggyback_needs_decision / commit already declared below

    // ── Piggyback interface (shared interaction rounds) ──
    // A second operation runs concurrently on the same OMAP, sharing rounds.
    // Each round, step_next_round() returns ORAM requests for both ops.
    virtual void begin_piggyback_search(int /*key*/) {}
    virtual void begin_piggyback_insert(int /*key*/, const Bytes& /*value*/) {}
    virtual void begin_piggyback_dummy() {}
    virtual Bytes finish_piggyback() { return {}; }
    virtual bool piggyback_needs_decision() const { return false; }
    virtual Bytes piggyback_get_traverse_result() { return {}; }
    virtual void piggyback_commit_remove() {}
    virtual void piggyback_commit_noop() {}
};

}  // namespace tiered_omap
