#pragma once

#include "tiered_omap/common.h"
#include <algorithm>
#include <vector>

namespace tiered_omap {

struct MaintenanceConfig {
    int observation_window = 65536;   // B_obs: epoch length for frequency estimation
    int swap_interval = 32;           // B_swap (= B1 = B2 = B3)
    int cache_size = 8;               // B: client-side cache capacity
    bool enabled = false;
    bool piggyback = false;
};

struct CacheEntry {
    int key = INVALID_KEY;
    int fp = 0;
};

enum class SwapState { Idle, HotPend, ColdPend };

class MaintenanceManager {
public:
    explicit MaintenanceManager(const MaintenanceConfig& cfg)
        : cfg_(cfg) {}

    // Paper Algorithm 2: UpdateMeta(k, cnt, ep, fp, e)
    EpochMeta update_meta(const EpochMeta& stored) const {
        EpochMeta m = stored;
        if (m.ep < obs_epoch_) {
            m.fp = (m.ep == obs_epoch_ - 1) ? m.cnt : 0;
            m.cnt = 1;
            m.ep = obs_epoch_;
        } else {
            ++m.cnt;
        }
        return m;
    }

    void tick() {
        ++total_accesses_;
        obs_epoch_ = total_accesses_ / cfg_.observation_window;
    }

    bool should_maintain() const {
        return total_accesses_ > 0 && obs_epoch_ > 0
               && (total_accesses_ % cfg_.swap_interval) == 0;
    }

    bool should_maintain_next() const {
        return ((total_accesses_ + 1) % cfg_.swap_interval) == 0;
    }

    // Cold promotion: x.fp > min(cache.fp)
    bool should_promote(int fp) const {
        if (cache_.empty()) return false;
        int min_fp = cache_[0].fp;
        for (size_t i = 1; i < cache_.size(); ++i)
            if (cache_[i].fp < min_fp) min_fp = cache_[i].fp;
        return fp > min_fp;
    }

    // Hot demotion: z.fp < max(cache.fp)
    bool should_demote_cache(int fp) const {
        if (cache_.empty()) return false;
        int max_fp = cache_[0].fp;
        for (size_t i = 1; i < cache_.size(); ++i)
            if (cache_[i].fp > max_fp) max_fp = cache_[i].fp;
        return fp < max_fp;
    }

    // Paper Algorithm 2: CacheAdmit(x, dir).
    // Adds (key, fp) to B, then evicts one entry:
    //   from_hot=true  → evict argmax(fp) (returns to OMAP_h)
    //   from_hot=false → evict argmin(fp) (returns to OMAP_c)
    CacheEntry cache_admit(int key, int fp, bool from_hot) {
        cache_.push_back({key, fp});
        size_t idx = 0;
        if (from_hot) {
            for (size_t i = 1; i < cache_.size(); ++i)
                if (cache_[i].fp > cache_[idx].fp) idx = i;
        } else {
            for (size_t i = 1; i < cache_.size(); ++i)
                if (cache_[i].fp < cache_[idx].fp) idx = i;
        }
        CacheEntry evicted = cache_[idx];
        cache_.erase(cache_.begin() + static_cast<long>(idx));
        return evicted;
    }

    SwapState swap_state() const { return swap_state_; }
    void set_swap_state(SwapState st) { swap_state_ = st; }

    int scan_ptr() const { return scan_ptr_; }
    void set_scan_ptr(int key) { scan_ptr_ = key; }
    void reset_scan_ptr() { scan_ptr_ = -1; }

    void init_cache(const std::vector<CacheEntry>& entries) { cache_ = entries; }

    bool is_in_cache(int key) const {
        for (auto& e : cache_) if (e.key == key) return true;
        return false;
    }

    void update_cache_fp(int key, int new_fp) {
        for (auto& e : cache_)
            if (e.key == key) { e.fp = new_fp; break; }
    }

    int obs_epoch() const { return obs_epoch_; }
    int total_access_count() const { return total_accesses_; }
    const MaintenanceConfig& config() const { return cfg_; }
    const std::vector<CacheEntry>& cache() const { return cache_; }

private:
    MaintenanceConfig cfg_;
    std::vector<CacheEntry> cache_;
    SwapState swap_state_ = SwapState::Idle;
    int obs_epoch_ = 0;
    int total_accesses_ = 0;
    int scan_ptr_ = 0;
};

}  // namespace tiered_omap
