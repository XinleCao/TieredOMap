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

    virtual const BandwidthStats& last_stats() const = 0;
    virtual const BandwidthStats& total_stats() const = 0;
    virtual void reset_stats() = 0;
};

}  // namespace tiered_omap
