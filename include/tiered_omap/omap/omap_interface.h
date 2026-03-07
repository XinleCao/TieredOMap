#pragma once

#include "tiered_omap/common.h"
#include <utility>
#include <vector>

namespace tiered_omap {

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
};

}  // namespace tiered_omap
