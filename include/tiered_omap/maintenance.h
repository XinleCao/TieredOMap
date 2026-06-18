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

    // ── Staggered B1/B2/B3 triggers ──
    // B1 (scan), B2 (hot insert), B3 (cold insert) share the same period
    // but fire at different phase offsets so at most ONE triggers per access.
    bool should_scan() const {
        return can_maintain() && phase() == scan_phase();
    }
    bool should_hot_insert() const {
        return can_maintain() && phase() == hot_insert_phase();
    }
    bool should_cold_insert() const {
        return can_maintain() && phase() == cold_insert_phase();
    }

    bool should_maintain() const {
        return should_scan() || should_hot_insert() || should_cold_insert();
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

    void benchmark_set_total_accesses(int total) {
        total_accesses_ = std::max(0, total);
        obs_epoch_ = total_accesses_ / cfg_.observation_window;
    }

private:
    bool can_maintain() const {
        return total_accesses_ > 0 && obs_epoch_ > 0;
    }

    int phase() const {
        return total_accesses_ % cfg_.swap_interval;
    }

    // Phase offsets: scan=0, hot_insert=B/3, cold_insert=2B/3.
    // For B_swap >= 3 these are distinct; for B_swap < 3 they collapse
    // (degenerate case, not used in practice).
    int scan_phase() const { return 0; }
    int hot_insert_phase() const {
        return std::max(1, cfg_.swap_interval / 3);
    }
    int cold_insert_phase() const {
        return std::max(2, 2 * cfg_.swap_interval / 3);
    }

    MaintenanceConfig cfg_;
    std::vector<CacheEntry> cache_;
    SwapState swap_state_ = SwapState::Idle;
    int obs_epoch_ = 0;
    int total_accesses_ = 0;
    int scan_ptr_ = -1;
};

}  // namespace tiered_omap
