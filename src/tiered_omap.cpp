#include "tiered_omap/tiered_omap.h"
#include "tiered_omap/omap/avl_omap.h"
#include "tiered_omap/omap/bplus_omap.h"
#include "tiered_omap/omap/da_ost_omap.h"
#include "tiered_omap/network/tcp_channel.h"
#include "tiered_omap/network/network_storage.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <thread>

namespace tiered_omap {

static constexpr int INVALID_BLOCK = -1;

static int ref_to_blk(const Bytes& ref) {
    return ref.empty() ? INVALID_BLOCK : bytes_to_int(ref);
}

TieredOMap::TieredOMap(const TieredOMapConfig& config)
    : config_(config) {
    if (config_.maintenance.enabled)
        maint_ = std::make_unique<MaintenanceManager>(config_.maintenance);
}

void TieredOMap::enable_maintenance(const MaintenanceConfig& mc) {
    config_.maintenance = mc;
    config_.maintenance.enabled = true;
    maint_ = std::make_unique<MaintenanceManager>(config_.maintenance);

    int B = mc.cache_size;
    int extract_count = std::min(B, static_cast<int>(hot_key_list_.size()));
    std::vector<CacheEntry> cache_entries;
    std::vector<int> to_extract(hot_key_list_.begin(),
                                 hot_key_list_.begin() + extract_count);
    for (int k : to_extract) {
        Bytes ref = hot_omap_->search(k);
        hot_omap_->remove(k);
        phys_hot_keys_.erase(k);
        cache_keys_.insert(k);
        cache_data_refs_[k] = ref;
        cache_entries.push_back({k, 0});
    }
    hot_key_list_.erase(hot_key_list_.begin(),
                        hot_key_list_.begin() + extract_count);
    maint_->init_cache(cache_entries);
}

void TieredOMap::init(
    const std::vector<std::pair<int, Bytes>>& all_data,
    const std::vector<int>& hot_keys) {
    hot_keys_ = {hot_keys.begin(), hot_keys.end()};
    phys_hot_keys_ = hot_keys_;
    hot_key_list_ = hot_keys;
    std::sort(hot_key_list_.begin(), hot_key_list_.end());

    bool use_epoch = config_.maintenance.enabled;
    int bs = config_.bucket_size;
    auto& sc = config_.storage_creator;

    int N = std::max(static_cast<int>(all_data.size()), 1);
    int data_cap = use_epoch ? std::max(N * 2, N + 64) : N;
    data_oram_ = PathORAM(data_cap, bs, 7, sc);

    std::unordered_map<int, Bytes> data_oram_blocks;
    next_data_block_id_ = 0;

    std::vector<std::pair<int, Bytes>> hot_refs, cold_refs;
    for (auto& [k, v] : all_data) {
        Bytes stored = v;
        if (use_epoch)
            stored = encode_with_epoch(v, {0, 0});

        int blk_id = next_data_block_id_++;
        data_oram_blocks[blk_id] = stored;

        Bytes ref = int_to_bytes(blk_id);
        if (phys_hot_keys_.count(k))
            hot_refs.emplace_back(k, ref);
        else
            cold_refs.emplace_back(k, ref);
    }
    data_oram_.init(data_oram_blocks);

    int n = static_cast<int>(hot_refs.size());
    int N_cold = static_cast<int>(cold_refs.size());

    int hot_cap = std::max(n, 1);
    int cold_cap = use_epoch ? std::max(N_cold, config_.total_keys) : std::max(N_cold, 1);
    hot_capacity_ = hot_cap;

    auto make_omap = [&](OmapBackend be, int cap, bool is_cold) -> std::unique_ptr<OmapInterface> {
        switch (be) {
        case OmapBackend::BPlus: {
            if (is_cold && config_.use_split_oram && n > 0) {
                int ord = config_.bplus_order;
                int split_depth = std::max(1,
                    static_cast<int>(std::floor(
                        std::log(std::max(n, 2)) / std::log(std::max(ord, 2)))));
                int upper_cap = static_cast<int>(
                    (std::pow(ord, split_depth) - 1) / std::max(ord - 1, 1));
                upper_cap = std::max(upper_cap, n);
                auto bp = std::make_unique<BPlusOmap>(
                    cap, config_.bplus_order, bs,
                    split_depth, upper_cap, sc);
                bp->set_index_mode(true);
                return bp;
            }
            auto bp = std::make_unique<BPlusOmap>(cap, config_.bplus_order, bs, sc);
            bp->set_index_mode(true);
            return bp;
        }
        case OmapBackend::DaAvl:
            return std::make_unique<DaOstOmap>(cap, OdsTreeType::AVL, 0, bs, 8, sc);
        case OmapBackend::DaBplus:
            return std::make_unique<DaOstOmap>(
                cap, OdsTreeType::BPlus, 0, bs, config_.bplus_order, sc);
        default: {
            if (is_cold && config_.use_split_oram && n > 0) {
                int split_depth = ceil_log2(std::max(n, 2));
                return std::make_unique<AVLOmap>(
                    cap, bs, split_depth, std::max(n, 1), sc);
            }
            return std::make_unique<AVLOmap>(cap, bs, sc);
        }
        }
    };

    OmapBackend hot_be = config_.effective_hot_backend();
    hot_omap_ = make_omap(hot_be, hot_cap, false);
    cold_omap_ = make_omap(config_.backend, cold_cap, true);
    hot_omap_->init(hot_refs);
    cold_omap_->init(cold_refs);
    if (round_delay_us_ > 0)
        set_round_delay_us(round_delay_us_);

    if (maint_) {
        int B = config_.maintenance.cache_size;
        int extract_count = std::min(B, static_cast<int>(hot_key_list_.size()));
        std::vector<CacheEntry> cache_entries;
        std::vector<int> to_extract(hot_key_list_.begin(),
                                     hot_key_list_.begin() + extract_count);
        for (int k : to_extract) {
            Bytes ref = hot_omap_->search(k);
            hot_omap_->remove(k);
            phys_hot_keys_.erase(k);
            cache_keys_.insert(k);
            cache_data_refs_[k] = ref;
            cache_entries.push_back({k, 0});
        }
        hot_key_list_.erase(hot_key_list_.begin(),
                            hot_key_list_.begin() + extract_count);
        maint_->init_cache(cache_entries);
    }
}

DynamicDebugState TieredOMap::dynamic_debug_state() const {
    DynamicDebugState st;
    st.hot_keys = hot_keys_;
    st.phys_hot_keys = phys_hot_keys_;
    st.cache_keys = cache_keys_;
    st.hot_key_list = hot_key_list_;
    st.pending_insert_key = pending_insert_key_;
    st.pending_has_ref = !pending_insert_ref_.empty();
    if (maint_) {
        st.cache_entries = maint_->cache();
        st.swap_state = maint_->swap_state();
        st.total_accesses = maint_->total_access_count();
        st.obs_epoch = maint_->obs_epoch();
    }
    return st;
}

Bytes TieredOMap::data_epoch_access(
    int blk, const Bytes* new_value, EpochMeta* out_meta) {
    if (blk < 0) {
        data_oram_.dummy_access();
        if (out_meta) *out_meta = {};
        return {};
    }

    Bytes plain;
    EpochMeta updated;
    data_oram_.access_update(blk, [&](const Bytes& raw) {
        auto [val, meta] = decode_epoch(raw);
        updated = maint_->update_meta(meta);
        plain = new_value ? *new_value : val;
        return encode_with_epoch(plain, updated);
    });
    if (out_meta) *out_meta = updated;
    return plain;
}

// ─── Main access ────────────────────────────────────────────────────────────

AccessResult TieredOMap::access(int key, const Bytes* new_value) {
    AccessResult result;
    bool is_hot_logical = hot_keys_.count(key) > 0;
    bool is_hot_physical = phys_hot_keys_.count(key) > 0;
    bool use_epoch = maint_ != nullptr;

    result.found_in_hot = is_hot_logical;

    if (use_epoch) {
        // ── Epoch-aware access with dynamic maintenance ──
        maint_->tick();
        bool scan_piggybacked = false;
        bool hot_ins_piggybacked = false;
        bool cold_ins_piggybacked = false;
        bool hot_dummy_pb = false;
        bool cold_dummy_pb = false;
        bool query_stats_ready = false;

        bool is_pending = (key == pending_insert_key_);
        bool is_in_cache = !is_pending && cache_keys_.count(key) > 0;
        bool can_interleave =
            channel_ &&
            hot_omap_->supports_interleaved() &&
            cold_omap_->supports_interleaved();

        if (debug_access_) {
            fprintf(stderr, "[DBG-PATH] key=%d is_pending=%d is_in_cache=%d "
                    "can_interleave=%d(ch=%d h=%d c=%d) is_hot_log=%d is_hot_phys=%d\n",
                    key, (int)is_pending, (int)is_in_cache,
                    (int)can_interleave, (channel_ != nullptr),
                    (int)hot_omap_->supports_interleaved(),
                    (int)cold_omap_->supports_interleaved(),
                    (int)is_hot_logical, (int)is_hot_physical);
        }
        if (is_pending) {
            // Pending entry: between cache eviction and its fixed-rate
            // insertion into the destination OMAP. The logical tier is tracked
            // by hot_keys_: HotPend entries remain hot, ColdPend entries do not.
            result.found_in_hot = is_hot_logical;
            int blk = ref_to_blk(pending_insert_ref_);
            EpochMeta meta;
            result.value = data_epoch_access(blk, new_value, &meta);
            result.last_access_fp = meta.fp;
            if (config_.mode == SecurityMode::FullOblivious) {
                hot_omap_->dummy_access();
                cold_omap_->dummy_access();
            } else if (is_hot_logical) {
                hot_omap_->dummy_access();
                cold_omap_->partial_dummy_access();
            } else {
                hot_omap_->partial_dummy_access();
                cold_omap_->dummy_access();
            }
        } else if (can_interleave && !is_in_cache) {
            bool use_partial = (config_.mode == SecurityMode::TierMembership);
            bool do_pb = config_.maintenance.piggyback;

            int scan_key = INVALID_KEY;
            int hot_ins_key = INVALID_KEY;
            int cold_ins_key = INVALID_KEY;

            if (do_pb) {
                bool do_scan = maint_->should_scan()
                               && !is_da_hot()
                               && maint_->swap_state() == SwapState::Idle
                               && is_hot_logical;
                scan_key = do_scan ? next_scan_key_excluding(key)
                                   : INVALID_KEY;
                if (scan_key != INVALID_KEY) scan_piggybacked = true;

                bool can_pb_hot = is_hot_physical || !use_partial;
                bool can_pb_cold = !is_hot_physical || !use_partial;

                if (maint_->should_hot_insert()) {
                    if (maint_->swap_state() == SwapState::HotPend
                        && pending_insert_key_ != INVALID_KEY)
                        hot_ins_key = pending_insert_key_;
                    else if (can_pb_hot)
                        hot_dummy_pb = true;
                }

                if (maint_->should_cold_insert()) {
                    if (maint_->swap_state() == SwapState::ColdPend
                        && pending_insert_key_ != INVALID_KEY)
                        cold_ins_key = pending_insert_key_;
                    else if (can_pb_cold)
                        cold_dummy_pb = true;
                }
            }

            auto ir = interleaved_access(key, new_value, use_partial, true,
                                         scan_key, hot_ins_key, cold_ins_key,
                                         hot_dummy_pb, cold_dummy_pb);
            result.value = ir.value;
            result.found_in_hot = ir.found_in_hot;
            result.hot_bw = ir.hot_bw;
            result.cold_bw = ir.cold_bw;
            result.total_bw = ir.total_bw;
            result.rounds_to_answer = ir.rounds_to_answer;
            result.last_access_fp = ir.last_access_fp;
            query_stats_ready = true;
            if (debug_access_) {
                fprintf(stderr, "[DBG] key=%d pb=%d scan=%d hins=%d cins=%d hdpb=%d cdpb=%d "
                        "ir_rnd=%d ir_rta=%d ir_bw=%.0f\n",
                        key, (int)do_pb, scan_key, hot_ins_key, cold_ins_key,
                        (int)hot_dummy_pb, (int)cold_dummy_pb,
                        (int)ir.total_bw.rounds, ir.rounds_to_answer,
                        ir.total_bw.total_bytes() / 1024.0);
            }

            // Process piggybacked scan result (DECISION handled inside loop)
            if (scan_key != INVALID_KEY && ir.scan_decision_handled
                && ir.scan_demoted) {
                CacheEntry evicted = maint_->cache_admit(
                    scan_key, ir.scan_fp, true);
                pending_insert_key_ = evicted.key;
                pending_insert_ref_ = cache_data_refs_[evicted.key];
                phys_hot_keys_.erase(scan_key);
                {
                    auto eit = std::lower_bound(
                        hot_key_list_.begin(), hot_key_list_.end(), scan_key);
                    if (eit != hot_key_list_.end() && *eit == scan_key)
                        hot_key_list_.erase(eit);
                }
                cache_keys_.insert(scan_key);
                cache_data_refs_[scan_key] = ir.scan_ref;
                cache_keys_.erase(evicted.key);
                cache_data_refs_.erase(evicted.key);
                maint_->set_swap_state(SwapState::HotPend);
            }

            // Handle piggybacked B2 (hot insert) result
            if (ir.hot_insert_done) {
                phys_hot_keys_.insert(hot_ins_key);
                {
                    auto it = std::lower_bound(
                        hot_key_list_.begin(), hot_key_list_.end(), hot_ins_key);
                    hot_key_list_.insert(it, hot_ins_key);
                }
                pending_insert_key_ = INVALID_KEY;
                pending_insert_ref_.clear();
                maint_->set_swap_state(SwapState::Idle);
                hot_ins_piggybacked = true;
            }

            // Handle piggybacked B3 (cold insert) result
            if (ir.cold_insert_done) {
                pending_insert_key_ = INVALID_KEY;
                pending_insert_ref_.clear();
                maint_->set_swap_state(SwapState::Idle);
                cold_ins_piggybacked = true;
            }

            // Reactive cold promotion
            if (!is_hot_logical && ir.cold_decision_handled) {
                // DECISION phase handled the type-hidden remove/noop
                if (ir.cold_promotion_done) {
                    CacheEntry evicted = maint_->cache_admit(
                        key, ir.last_access_fp, false);
                    pending_insert_key_ = evicted.key;
                    pending_insert_ref_ = cache_data_refs_[evicted.key];
                    cache_keys_.erase(evicted.key);
                    cache_data_refs_.erase(evicted.key);
                    hot_keys_.erase(evicted.key);
                    cache_keys_.insert(key);
                    cache_data_refs_[key] = ir.cold_ref;
                    hot_keys_.insert(key);
                    maint_->set_swap_state(SwapState::ColdPend);
                }
            } else if (!is_hot_logical && ir.last_access_fp > 0
                && maint_->swap_state() == SwapState::Idle
                && maint_->should_promote(ir.last_access_fp)) {
                cold_omap_->remove(key);
                CacheEntry evicted = maint_->cache_admit(
                    key, ir.last_access_fp, false);
                pending_insert_key_ = evicted.key;
                pending_insert_ref_ = cache_data_refs_[evicted.key];
                cache_keys_.erase(evicted.key);
                cache_data_refs_.erase(evicted.key);
                hot_keys_.erase(evicted.key);
                cache_keys_.insert(key);
                cache_data_refs_[key] = ir.cold_ref;
                hot_keys_.insert(key);
                maint_->set_swap_state(SwapState::ColdPend);
            } else if (!is_hot_logical && !ir.cold_decision_handled) {
                cold_omap_->dummy_access();
            }
        } else if (is_in_cache) {
            // Cache hit: direct data access, dummy both OMAPs
            Bytes ref = cache_data_refs_[key];
            int blk = ref_to_blk(ref);
            EpochMeta meta;
            result.value = data_epoch_access(blk, new_value, &meta);
            result.last_access_fp = meta.fp;
            maint_->update_cache_fp(key, meta.fp);
            hot_omap_->dummy_access();
            if (config_.mode == SecurityMode::FullOblivious)
                cold_omap_->dummy_access();
            else
                cold_omap_->partial_dummy_access();
        } else {
            // Sequential epoch access (local mode, no channel)
            OmapInterface* target = is_hot_physical ? hot_omap_.get()
                                                    : cold_omap_.get();
            OmapInterface* other  = is_hot_physical ? cold_omap_.get()
                                                    : hot_omap_.get();
            Bytes ref = target->search(key);
            int blk = ref_to_blk(ref);
            EpochMeta meta;
            result.value = data_epoch_access(blk, new_value, &meta);
            result.last_access_fp = meta.fp;

            if (config_.mode == SecurityMode::FullOblivious)
                other->dummy_access();
            else
                other->partial_dummy_access();

            // Reactive cold promotion: cold access with high fp
            if (!is_hot_logical && !ref.empty()
                && maint_->swap_state() == SwapState::Idle
                && maint_->should_promote(meta.fp)) {
                cold_omap_->remove(key);
                CacheEntry evicted = maint_->cache_admit(key, meta.fp, false);
                pending_insert_key_ = evicted.key;
                pending_insert_ref_ = cache_data_refs_[evicted.key];
                cache_keys_.erase(evicted.key);
                cache_data_refs_.erase(evicted.key);
                hot_keys_.erase(evicted.key);
                cache_keys_.insert(key);
                cache_data_refs_[key] = ref;
                hot_keys_.insert(key);
                maint_->set_swap_state(SwapState::ColdPend);
            } else if (!is_hot_logical && !ref.empty()) {
                // Dummy to match promotion's remove shape
                cold_omap_->dummy_access();
            }
        }

        // DA piggyback scan (reads metadata of next hot key during access)
        if (is_da_hot() && config_.maintenance.piggyback) {
            auto* da_hot = static_cast<DaOstOmap*>(hot_omap_.get());
            auto sr = da_hot->piggyback_scan_step();
            if (sr.key != INVALID_KEY && !sr.value.empty()) {
                int scan_blk = ref_to_blk(sr.value);
                Bytes scan_raw = data_access(scan_blk);
                auto [sv, sm] = decode_epoch(scan_raw);
                if (maint_->should_demote_cache(sm.fp))
                    da_pending_demotions_.push_back({sr.key, sm.fp});
            }
        }

        if (!query_stats_ready) {
            // Non-interleaved epoch paths above execute their OMAP/data ORAM
            // operations sequentially, so total interaction rounds are additive.
            result.hot_bw = hot_omap_->last_stats();
            result.cold_bw = cold_omap_->last_stats();
            auto data_bw = data_oram_.last_stats();
            result.total_bw.bytes_downloaded =
                result.hot_bw.bytes_downloaded + result.cold_bw.bytes_downloaded +
                data_bw.bytes_downloaded;
            result.total_bw.bytes_uploaded =
                result.hot_bw.bytes_uploaded + result.cold_bw.bytes_uploaded +
                data_bw.bytes_uploaded;
            result.total_bw.rounds =
                result.hot_bw.rounds + result.cold_bw.rounds + data_bw.rounds;

            if (is_in_cache || is_pending) {
                result.rounds_to_answer = result.found_in_hot
                    ? static_cast<int>(data_bw.rounds)
                    : static_cast<int>(result.total_bw.rounds);
            } else if (is_hot_logical) {
                result.rounds_to_answer =
                    static_cast<int>(result.hot_bw.rounds + data_bw.rounds);
            } else {
                result.rounds_to_answer = static_cast<int>(result.total_bw.rounds);
            }
        }

        // ── Staggered maintenance (B1/B2/B3 at different phase offsets) ──
        auto h_before = hot_omap_->total_stats();
        auto c_before = cold_omap_->total_stats();
        auto d_before = data_oram_.total_stats();
        bool did_scan = false, did_hins = false, did_cins = false;
        if (!scan_piggybacked && maint_->should_scan()) { do_scan_step(); did_scan = true; }
        if (!hot_ins_piggybacked && !hot_dummy_pb && maint_->should_hot_insert())
            { do_hot_insert_step(); did_hins = true; }
        if (!cold_ins_piggybacked && !cold_dummy_pb && maint_->should_cold_insert())
            { do_cold_insert_step(); did_cins = true; }
        {
            auto h_after = hot_omap_->total_stats();
            auto c_after = cold_omap_->total_stats();
            auto d_after = data_oram_.total_stats();
            auto delta_rnd =
                (h_after.rounds - h_before.rounds)
                + (c_after.rounds - c_before.rounds)
                + (d_after.rounds - d_before.rounds);
            if (debug_access_ && delta_rnd > 0) {
                fprintf(stderr, "[DBG] standalone maint: scan=%d hins=%d cins=%d "
                        "h_rnd=%lld c_rnd=%lld d_rnd=%lld total_delta=%lld\n",
                        (int)did_scan, (int)did_hins, (int)did_cins,
                        (long long)(h_after.rounds - h_before.rounds),
                        (long long)(c_after.rounds - c_before.rounds),
                        (long long)(d_after.rounds - d_before.rounds),
                        (long long)delta_rnd);
            }
            result.total_bw.bytes_downloaded +=
                (h_after.bytes_downloaded - h_before.bytes_downloaded)
                + (c_after.bytes_downloaded - c_before.bytes_downloaded)
                + (d_after.bytes_downloaded - d_before.bytes_downloaded);
            result.total_bw.bytes_uploaded +=
                (h_after.bytes_uploaded - h_before.bytes_uploaded)
                + (c_after.bytes_uploaded - c_before.bytes_uploaded)
                + (d_after.bytes_uploaded - d_before.bytes_uploaded);
            result.total_bw.rounds +=
                (h_after.rounds - h_before.rounds)
                + (c_after.rounds - c_before.rounds)
                + (d_after.rounds - d_before.rounds);
        }

        return result;
    }

    // ── Non-epoch paths (no maintenance) ──
    bool can_interleave =
        channel_ &&
        (config_.mode == SecurityMode::FullOblivious ||
         config_.mode == SecurityMode::TierMembership) &&
        hot_omap_->supports_interleaved() &&
        cold_omap_->supports_interleaved();

    if (can_interleave) {
        bool use_partial = (config_.mode == SecurityMode::TierMembership);
        auto ir = interleaved_access(key, new_value, use_partial, false,
                                     INVALID_KEY, INVALID_KEY, INVALID_KEY,
                                     false, force_cold_dummy_pb_);
        result.value = ir.value;
        result.found_in_hot = ir.found_in_hot;
        result.hot_bw = ir.hot_bw;
        result.cold_bw = ir.cold_bw;
        result.total_bw = ir.total_bw;
        result.rounds_to_answer = ir.rounds_to_answer;
        return result;
    }

    // Both OMAPs search (sequential in non-interleaved mode)
    Bytes hot_ref = hot_omap_->search(key);
    result.hot_bw = hot_omap_->last_stats();
    bool found_hot = !hot_ref.empty();

    if (found_hot) {
        result.found_in_hot = true;
        int blk = ref_to_blk(hot_ref);
        result.value = data_access(blk, new_value);
        if (config_.mode == SecurityMode::FullOblivious) {
            cold_omap_->search(key);
            data_oram_.dummy_access();
        } else {
            cold_omap_->partial_dummy_access();
        }
    } else {
        Bytes cold_ref = cold_omap_->search(key);
        int blk = ref_to_blk(cold_ref);
        if (config_.mode == SecurityMode::FullOblivious)
            data_oram_.dummy_access();
        result.value = data_access(blk, new_value);
    }
    result.cold_bw = cold_omap_->last_stats();

    int dpbw = data_oram_.path_bandwidth_bytes();
    int nops = (config_.mode == SecurityMode::FullOblivious) ? 2 : 1;
    result.total_bw.bytes_downloaded =
        result.hot_bw.bytes_downloaded + result.cold_bw.bytes_downloaded +
        static_cast<size_t>(dpbw) * nops;
    result.total_bw.bytes_uploaded =
        result.hot_bw.bytes_uploaded + result.cold_bw.bytes_uploaded +
        static_cast<size_t>(dpbw) * nops;
    result.total_bw.rounds =
        result.hot_bw.rounds + result.cold_bw.rounds + nops;

    if (found_hot)
        result.rounds_to_answer = static_cast<int>(result.hot_bw.rounds) + 1;
    else
        result.rounds_to_answer = static_cast<int>(result.total_bw.rounds);

    return result;
}

// ─── Helpers ─────────────────────────────────────────────────────────────────

int TieredOMap::next_scan_key() {
    if (hot_key_list_.empty()) return INVALID_KEY;
    auto it = std::upper_bound(
        hot_key_list_.begin(), hot_key_list_.end(), maint_->scan_ptr());
    if (it == hot_key_list_.end()) {
        maint_->reset_scan_ptr();
        it = hot_key_list_.begin();
    }
    int sk = *it;
    maint_->set_scan_ptr(sk);
    return sk;
}

int TieredOMap::next_scan_key_excluding(int avoid_key) {
    if (hot_key_list_.empty()) return INVALID_KEY;
    if (hot_key_list_.size() == 1 && hot_key_list_.front() == avoid_key)
        return INVALID_KEY;

    auto it = std::upper_bound(
        hot_key_list_.begin(), hot_key_list_.end(), maint_->scan_ptr());
    for (size_t seen = 0; seen < hot_key_list_.size(); ++seen) {
        if (it == hot_key_list_.end()) {
            maint_->reset_scan_ptr();
            it = hot_key_list_.begin();
        }
        int sk = *it;
        ++it;
        if (sk == avoid_key) continue;
        maint_->set_scan_ptr(sk);
        return sk;
    }
    return INVALID_KEY;
}

// ─── Staggered maintenance steps ────────────────────────────────────────────

// B1: scan hot OMAP for demotion candidate (or dummy)
void TieredOMap::do_scan_step() {
    if (maint_->swap_state() != SwapState::Idle) {
        hot_omap_->dummy_access();
        data_oram_.dummy_access();
        return;
    }

    bool da = is_da_hot() && config_.maintenance.piggyback;

    if (da) {
        // DA path: dequeue from piggyback scan results
        int dk = INVALID_KEY;
        int dk_fp = 0;
        if (!da_pending_demotions_.empty()) {
            auto [k, fp] = da_pending_demotions_.front();
            da_pending_demotions_.pop_front();
            dk = k;
            dk_fp = fp;
        }
        if (dk != INVALID_KEY && phys_hot_keys_.count(dk)) {
            Bytes ref = hot_omap_->search(dk);
            hot_omap_->remove(dk);
            CacheEntry evicted = maint_->cache_admit(dk, dk_fp, true);
            pending_insert_key_ = evicted.key;
            pending_insert_ref_ = cache_data_refs_[evicted.key];
            phys_hot_keys_.erase(dk);
            {
                auto eit = std::lower_bound(
                    hot_key_list_.begin(), hot_key_list_.end(), dk);
                if (eit != hot_key_list_.end() && *eit == dk)
                    hot_key_list_.erase(eit);
            }
            cache_keys_.insert(dk);
            cache_data_refs_[dk] = ref;
            cache_keys_.erase(evicted.key);
            cache_data_refs_.erase(evicted.key);
            maint_->set_swap_state(SwapState::HotPend);
        } else {
            hot_omap_->dummy_access();
        }
        return;
    }

    // Non-DA path: sequential scan of hot_key_list_
    if (hot_key_list_.empty()) {
        hot_omap_->dummy_access();
        data_oram_.dummy_access();
        return;
    }

    auto it = std::upper_bound(
        hot_key_list_.begin(), hot_key_list_.end(), maint_->scan_ptr());
    if (it == hot_key_list_.end()) {
        maint_->reset_scan_ptr();
        it = hot_key_list_.begin();
    }
    int scan_key = *it;
    maint_->set_scan_ptr(scan_key);

    Bytes ref = hot_omap_->search(scan_key);
    int blk = ref_to_blk(ref);
    Bytes raw = data_access(blk);
    auto [val, meta] = decode_epoch(raw);

    if (maint_->should_demote_cache(meta.fp)) {
        hot_omap_->remove(scan_key);
        CacheEntry evicted = maint_->cache_admit(scan_key, meta.fp, true);
        pending_insert_key_ = evicted.key;
        pending_insert_ref_ = cache_data_refs_[evicted.key];
        phys_hot_keys_.erase(scan_key);
        {
            auto eit = std::lower_bound(
                hot_key_list_.begin(), hot_key_list_.end(), scan_key);
            if (eit != hot_key_list_.end() && *eit == scan_key)
                hot_key_list_.erase(eit);
        }
        cache_keys_.insert(scan_key);
        cache_data_refs_[scan_key] = ref;
        cache_keys_.erase(evicted.key);
        cache_data_refs_.erase(evicted.key);
        maint_->set_swap_state(SwapState::HotPend);
    }
}

// B2: insert pending entry into hot OMAP (or dummy)
void TieredOMap::do_hot_insert_step() {
    if (maint_->swap_state() == SwapState::HotPend) {
        hot_omap_->insert(pending_insert_key_, pending_insert_ref_);
        phys_hot_keys_.insert(pending_insert_key_);
        {
            auto it = std::lower_bound(
                hot_key_list_.begin(), hot_key_list_.end(), pending_insert_key_);
            hot_key_list_.insert(it, pending_insert_key_);
        }
        pending_insert_key_ = INVALID_KEY;
        pending_insert_ref_.clear();
        maint_->set_swap_state(SwapState::Idle);
    } else {
        hot_omap_->dummy_access();
    }
}

// B3: insert pending entry into cold OMAP (or dummy)
void TieredOMap::do_cold_insert_step() {
    if (maint_->swap_state() == SwapState::ColdPend) {
        cold_omap_->insert(pending_insert_key_, pending_insert_ref_);
        pending_insert_key_ = INVALID_KEY;
        pending_insert_ref_.clear();
        maint_->set_swap_state(SwapState::Idle);
    } else {
        cold_omap_->dummy_access();
    }
}

// ─── Promote / Demote (explicit, non-oblivious) ────────────────────────────

void TieredOMap::promote(int key) {
    if (hot_keys_.count(key)) return;

    Bytes ref = cold_omap_->search(key);
    cold_omap_->remove(key);
    hot_omap_->insert(key, ref);
    hot_keys_.insert(key);
    phys_hot_keys_.insert(key);
    auto it = std::lower_bound(hot_key_list_.begin(), hot_key_list_.end(), key);
    hot_key_list_.insert(it, key);
}

void TieredOMap::demote(int key) {
    if (!hot_keys_.count(key)) return;

    Bytes ref = hot_omap_->search(key);
    hot_omap_->remove(key);
    cold_omap_->insert(key, ref);
    hot_keys_.erase(key);
    phys_hot_keys_.erase(key);
    auto it = std::lower_bound(hot_key_list_.begin(), hot_key_list_.end(), key);
    if (it != hot_key_list_.end() && *it == key)
        hot_key_list_.erase(it);
}

// ─── Interleaved helpers ────────────────────────────────────────────────────

void TieredOMap::inject_round_delay() const {
    if (round_delay_us_ > 0)
        std::this_thread::sleep_for(std::chrono::microseconds(round_delay_us_));
}

void TieredOMap::run_interleaved_loop(OmapInterface* a, OmapInterface* b) {
    while (!a->step_done() || !b->step_done()) {
        bool a_active = !a->step_done();
        bool b_active = !b->step_done();

        OramStepRound ar = a_active ? a->step_next_round() : OramStepRound{};
        OramStepRound br = b_active ? b->step_next_round() : OramStepRound{};

        std::vector<BatchReadReq> reads;
        for (auto& req : ar.reads)
            reads.push_back({req.store_id, req.leaf});
        size_t a_count = ar.reads.size();
        for (auto& req : br.reads)
            reads.push_back({req.store_id, req.leaf});

        auto results = batch_read_paths(*channel_, reads);

        if (a_active && a_count > 0) {
            std::vector<PathData> a_res(results.begin(),
                                        results.begin() + a_count);
            a->step_apply_reads(a_res);
        }
        if (b_active && br.reads.size() > 0) {
            std::vector<PathData> b_res(results.begin() + a_count,
                                        results.end());
            b->step_apply_reads(b_res);
        }

        if (a_active) a->step_process();
        if (b_active) b->step_process();

        std::vector<BatchWriteReq> writes;
        if (a_active) {
            for (auto& w : a->step_prepare_writes())
                writes.push_back({w.store_id, std::move(w.data)});
        }
        if (b_active) {
            for (auto& w : b->step_prepare_writes())
                writes.push_back({w.store_id, std::move(w.data)});
        }

        batch_write_paths(*channel_, writes);
        inject_round_delay();
    }
}

// ─── Interleaved (batched) access with data ORAM piggyback ──────────────────

AccessResult TieredOMap::interleaved_access(int key, const Bytes* new_value,
                                            bool use_partial_dummy,
                                            bool epoch_mode,
                                            int scan_key,
                                            int hot_insert_key,
                                            int cold_insert_key,
                                            bool hot_dummy_pb,
                                            bool cold_dummy_pb) {
    AccessResult result;
    bool fo = !use_partial_dummy;

    if (use_partial_dummy && !epoch_mode) {
        bool is_hot = hot_keys_.count(key) > 0;
        OmapInterface* target = is_hot ? hot_omap_.get() : cold_omap_.get();
        OmapInterface* other  = is_hot ? cold_omap_.get() : hot_omap_.get();

        target->begin_step_search(key, nullptr);
        other->begin_step_partial_dummy();
        run_interleaved_loop(target, other);

        Bytes ref = target->step_finish();
        other->step_finish();

        result.found_in_hot = is_hot;
        int blk = ref_to_blk(ref);
        result.value = data_access(blk, new_value);

        result.hot_bw = hot_omap_->last_stats();
        result.cold_bw = cold_omap_->last_stats();
        auto data_bw = data_oram_.last_stats();
        result.total_bw.bytes_downloaded =
            result.hot_bw.bytes_downloaded + result.cold_bw.bytes_downloaded +
            data_bw.bytes_downloaded;
        result.total_bw.bytes_uploaded =
            result.hot_bw.bytes_uploaded + result.cold_bw.bytes_uploaded +
            data_bw.bytes_uploaded;
        result.total_bw.rounds =
            std::max(result.hot_bw.rounds, result.cold_bw.rounds) +
            data_bw.rounds;
        result.rounds_to_answer = static_cast<int>(result.total_bw.rounds);
        if (!is_hot) result.cold_ref = ref;
        return result;
    }

    // ── Both OMAPs search in parallel (no searcher/dummy split) ──
    hot_omap_->begin_step_search(key, nullptr);
    cold_omap_->begin_step_search(key, nullptr);

    // ── Piggybacking setup (epoch mode maintenance) ──
    bool scan_on_piggyback = false;
    if (scan_key != INVALID_KEY) {
        hot_omap_->begin_piggyback_search(scan_key);
        hot_omap_->set_piggyback_decision_enabled(true);
        scan_on_piggyback = true;
    }
    if (hot_insert_key != INVALID_KEY)
        hot_omap_->begin_piggyback_insert(hot_insert_key, pending_insert_ref_);
    if (cold_insert_key != INVALID_KEY)
        cold_omap_->begin_piggyback_insert(cold_insert_key, pending_insert_ref_);
    if (hot_dummy_pb && hot_insert_key == INVALID_KEY)
        hot_omap_->begin_piggyback_dummy();
    if (cold_dummy_pb && cold_insert_key == INVALID_KEY)
        cold_omap_->begin_piggyback_dummy();

    if (epoch_mode)
        cold_omap_->set_step_decision_enabled(true);

    // ── Data ORAM state machines ──
    enum DStep { IDLE, NEED_READ, NEED_WRITE, DONE };

    // Data ORAM at hot_done (real if key in hot, dummy if key in cold for FO)
    DStep hot_data_st = IDLE;
    int  hot_data_blk = INVALID_BLOCK;
    int  hot_data_leaf = -1, hot_data_new_leaf = -1;

    // Data ORAM at cold_done (real if key in cold, dummy if key in hot for FO)
    DStep cold_data_st = IDLE;
    int  cold_data_blk = INVALID_BLOCK;
    int  cold_data_leaf = -1, cold_data_new_leaf = -1;

    // Scan data ORAM (epoch piggyback)
    DStep scan_data_st = scan_on_piggyback ? IDLE : DONE;
    int  scan_data_blk = INVALID_BLOCK;
    int  scan_data_leaf = -1, scan_data_new_leaf = -1;
    bool scan_decision_triggered = false;

    bool hot_done_flag = false, cold_done_flag = false;
    bool found_in_hot = false;
    Bytes hot_ref, cold_ref;
    int  total_rounds = 0;
    int  answer_round = 0;
    int  computed_fp  = 0;
    bool decision_triggered = false;
    bool decision_pending   = false;
    bool maintenance_transition_reserved = false;

    auto setup_data_oram = [&](DStep& st, int& blk, int& leaf, int& new_leaf,
                               const Bytes& ref) {
        blk = ref_to_blk(ref);
        if (blk != INVALID_BLOCK) {
            leaf     = data_oram_.get_leaf(blk);
            new_leaf = data_oram_.random_leaf();
            data_oram_.set_leaf(blk, new_leaf);
        } else {
            leaf     = data_oram_.random_leaf();
            new_leaf = -1;
        }
        st = NEED_READ;
    };

    while (true) {
        bool h_act = !hot_done_flag  && !hot_omap_->step_done();
        bool c_act = !cold_done_flag && !cold_omap_->step_done();
        if (!h_act && !c_act
            && hot_data_st  == DONE && cold_data_st == DONE
            && scan_data_st == DONE)
            break;

        ++total_rounds;

        // ── Gather read requests ──
        OramStepRound hr{}, cr{};
        if (h_act) hr = hot_omap_->step_next_round();
        if (c_act) cr = cold_omap_->step_next_round();

        std::vector<BatchReadReq> reads;
        for (auto& r : hr.reads) reads.push_back({r.store_id, r.leaf});
        size_t h_cnt = hr.reads.size();
        for (auto& r : cr.reads) reads.push_back({r.store_id, r.leaf});
        size_t c_cnt = cr.reads.size();

        size_t hd_cnt = 0, cd_cnt = 0, sd_cnt = 0;
        if (hot_data_st  == NEED_READ) { reads.push_back({data_oram_.get_store_id(), hot_data_leaf});  hd_cnt = 1; }
        if (cold_data_st == NEED_READ) { reads.push_back({data_oram_.get_store_id(), cold_data_leaf}); cd_cnt = 1; }
        if (scan_data_st == NEED_READ) { reads.push_back({data_oram_.get_store_id(), scan_data_leaf}); sd_cnt = 1; }

        auto fetched = batch_read_paths(*channel_, reads);

        // ── Apply reads ──
        size_t off = 0;
        if (h_act && h_cnt > 0) {
            hot_omap_->step_apply_reads({fetched.begin() + off,
                                         fetched.begin() + off + h_cnt});
        }
        off += h_cnt;
        if (c_act && c_cnt > 0) {
            cold_omap_->step_apply_reads({fetched.begin() + off,
                                          fetched.begin() + off + c_cnt});
        }
        off += c_cnt;
        if (hd_cnt) { data_oram_.apply_fetched_path(std::move(fetched[off])); ++off; }
        if (cd_cnt) { data_oram_.apply_fetched_path(std::move(fetched[off])); ++off; }
        if (sd_cnt) { data_oram_.apply_fetched_path(std::move(fetched[off])); ++off; }

        // ── Process OMAP steps ──
        if (h_act) hot_omap_->step_process();
        if (c_act) cold_omap_->step_process();

        // ── Hot data ORAM result processing ──
        if (hd_cnt) {
            if (hot_data_blk != INVALID_BLOCK) {
                Block b = data_oram_.extract_from_stash(hot_data_blk);
                Bytes sv;
                if (epoch_mode && maint_) {
                    auto [val, meta] = decode_epoch(b.value);
                    meta = maint_->update_meta(meta);
                    computed_fp = meta.fp;
                    result.value = new_value ? *new_value : val;
                    sv = encode_with_epoch(result.value, meta);
                    if (cache_keys_.count(key))
                        maint_->update_cache_fp(key, meta.fp);
                } else {
                    result.value = new_value ? *new_value : b.value;
                    sv = new_value ? *new_value : b.value;
                }
                data_oram_.add_to_stash({hot_data_blk, hot_data_new_leaf, std::move(sv)});
                answer_round = total_rounds;
            }
            hot_data_st = NEED_WRITE;
        }

        // ── Cold data ORAM result processing ──
        if (cd_cnt) {
            if (cold_data_blk != INVALID_BLOCK) {
                Block b = data_oram_.extract_from_stash(cold_data_blk);
                Bytes sv;
                if (epoch_mode && maint_) {
                    auto [val, meta] = decode_epoch(b.value);
                    meta = maint_->update_meta(meta);
                    computed_fp = meta.fp;
                    result.value = new_value ? *new_value : val;
                    sv = encode_with_epoch(result.value, meta);
                    if (cache_keys_.count(key))
                        maint_->update_cache_fp(key, meta.fp);
                } else {
                    result.value = new_value ? *new_value : b.value;
                    sv = new_value ? *new_value : b.value;
                }
                data_oram_.add_to_stash({cold_data_blk, cold_data_new_leaf, std::move(sv)});
                answer_round = total_rounds;
            }
            cold_data_st = NEED_WRITE;
            if (decision_pending) {
                bool should_prom = computed_fp > 0
                    && maint_ && maint_->swap_state() == SwapState::Idle
                    && !maintenance_transition_reserved
                    && maint_->should_promote(computed_fp);
                if (should_prom) {
                    cold_omap_->step_commit_remove();
                    result.cold_promotion_done = true;
                    maintenance_transition_reserved = true;
                } else {
                    cold_omap_->step_commit_noop();
                }
                result.cold_decision_handled = true;
                decision_pending = false;
            }
        }

        // ── Scan data ORAM result processing ──
        if (sd_cnt) {
            if (scan_data_blk != INVALID_BLOCK) {
                Block sb = data_oram_.extract_from_stash(scan_data_blk);
                if (epoch_mode && maint_) {
                    auto [sv, sm] = decode_epoch(sb.value);
                    bool should_demote =
                        maint_->swap_state() == SwapState::Idle
                        && !maintenance_transition_reserved
                        && maint_->should_demote_cache(sm.fp);
                    if (should_demote) {
                        hot_omap_->piggyback_commit_remove();
                        result.scan_demoted = true;
                        result.scan_fp = sm.fp;
                        maintenance_transition_reserved = true;
                    } else {
                        hot_omap_->piggyback_commit_noop();
                    }
                    result.scan_decision_handled = true;
                } else {
                    hot_omap_->piggyback_commit_noop();
                }
                data_oram_.add_to_stash({scan_data_blk, scan_data_new_leaf, sb.value});
            }
            scan_data_st = NEED_WRITE;
        }

        // ── DECISION trigger: cold OMAP paused after traverse (epoch) ──
        if (!decision_triggered && epoch_mode
            && !cold_done_flag && cold_omap_->step_needs_decision()) {
            decision_triggered = true;
            Bytes trav_ref = cold_omap_->step_get_traverse_result();
            setup_data_oram(cold_data_st, cold_data_blk, cold_data_leaf,
                            cold_data_new_leaf, trav_ref);
            decision_pending = true;
        }

        // ── Scan DECISION trigger ──
        if (!scan_decision_triggered && scan_on_piggyback
            && hot_omap_->piggyback_needs_decision()) {
            scan_decision_triggered = true;
            Bytes scan_trav_ref = hot_omap_->piggyback_get_traverse_result();
            setup_data_oram(scan_data_st, scan_data_blk, scan_data_leaf,
                            scan_data_new_leaf, scan_trav_ref);
        }

        // ── Hot OMAP completion ──
        if (!hot_done_flag && hot_omap_->step_done()) {
            hot_done_flag = true;
            hot_ref = hot_omap_->step_finish();
            found_in_hot = !hot_ref.empty();

            if (found_in_hot) {
                setup_data_oram(hot_data_st, hot_data_blk, hot_data_leaf,
                                hot_data_new_leaf, hot_ref);
                if (!fo) {
                    // TM: abort cold, skip cold data ORAM
                    cold_omap_->step_abort();
                    cold_done_flag = true;
                    cold_data_st = DONE;
                }
            } else {
                if (fo) {
                    // FO: dummy data ORAM at hot_done to hide tier membership
                    Bytes dummy_ref;
                    setup_data_oram(hot_data_st, hot_data_blk, hot_data_leaf,
                                    hot_data_new_leaf, dummy_ref);
                } else {
                    // TM: no data ORAM at hot_done (reveals key is cold)
                    hot_data_st = DONE;
                }
            }
        }

        // ── Cold OMAP completion ──
        if (!cold_done_flag && cold_omap_->step_done()) {
            cold_done_flag = true;
            if (!decision_triggered)
                cold_ref = cold_omap_->step_finish();
            else
                cold_omap_->step_finish();

            if (cold_data_st == IDLE) {
                if (!found_in_hot && !cold_ref.empty()) {
                    setup_data_oram(cold_data_st, cold_data_blk, cold_data_leaf,
                                    cold_data_new_leaf, cold_ref);
                } else if (fo) {
                    // FO: dummy data ORAM at cold_done (key was in hot)
                    Bytes dummy_ref;
                    setup_data_oram(cold_data_st, cold_data_blk, cold_data_leaf,
                                    cold_data_new_leaf, dummy_ref);
                } else {
                    cold_data_st = DONE;
                }
            }
        }

        // ── Prepare writes ──
        std::vector<BatchWriteReq> writes;
        if (h_act) {
            for (auto& w : hot_omap_->step_prepare_writes())
                writes.push_back({w.store_id, std::move(w.data)});
        }
        if (c_act) {
            for (auto& w : cold_omap_->step_prepare_writes())
                writes.push_back({w.store_id, std::move(w.data)});
        }
        if (hot_data_st == NEED_WRITE) {
            writes.push_back({data_oram_.get_store_id(),
                              data_oram_.prepare_eviction(hot_data_leaf)});
            hot_data_st = DONE;
        }
        if (cold_data_st == NEED_WRITE) {
            writes.push_back({data_oram_.get_store_id(),
                              data_oram_.prepare_eviction(cold_data_leaf)});
            cold_data_st = DONE;
        }
        if (scan_data_st == NEED_WRITE) {
            writes.push_back({data_oram_.get_store_id(),
                              data_oram_.prepare_eviction(scan_data_leaf)});
            scan_data_st = DONE;
        }
        batch_write_paths(*channel_, writes);
        inject_round_delay();
    }

    // ── Cleanup ──
    if (!hot_done_flag)
        hot_omap_->step_finish();
    if (!cold_done_flag)
        cold_omap_->step_finish();

    result.found_in_hot = found_in_hot;
    if (!found_in_hot) result.cold_ref = cold_ref;

    if (scan_on_piggyback)
        result.scan_ref = hot_omap_->finish_piggyback();
    if (hot_insert_key != INVALID_KEY) {
        hot_omap_->finish_piggyback();
        result.hot_insert_done = true;
    } else if (hot_dummy_pb) {
        hot_omap_->finish_piggyback();
    }
    if (cold_insert_key != INVALID_KEY) {
        cold_omap_->finish_piggyback();
        result.cold_insert_done = true;
    } else if (cold_dummy_pb) {
        cold_omap_->finish_piggyback();
    }

    result.last_access_fp = computed_fp;
    result.hot_bw  = hot_omap_->last_stats();
    result.cold_bw = cold_omap_->last_stats();

    int dpbw = data_oram_.path_bandwidth_bytes();
    int nops = fo ? 2 : 1;
    if (scan_on_piggyback) nops++;
    result.total_bw.bytes_downloaded =
        result.hot_bw.bytes_downloaded + result.cold_bw.bytes_downloaded +
        static_cast<size_t>(dpbw) * nops;
    result.total_bw.bytes_uploaded =
        result.hot_bw.bytes_uploaded + result.cold_bw.bytes_uploaded +
        static_cast<size_t>(dpbw) * nops;
    result.total_bw.rounds = total_rounds;

    result.rounds_to_answer = answer_round > 0 ? answer_round
        : static_cast<int>(result.total_bw.rounds);

    return result;
}

// ─── Interleaved DA piggyback access ────────────────────────────────────────

AccessResult TieredOMap::interleaved_da_piggyback(int key, const Bytes* new_value) {
    AccessResult result;
    bool is_hot_physical = phys_hot_keys_.count(key) > 0;
    bool is_hot_logical = hot_keys_.count(key) > 0;
    result.found_in_hot = is_hot_logical;

    OmapInterface* target = is_hot_physical ? hot_omap_.get() : cold_omap_.get();
    OmapInterface* other  = is_hot_physical ? cold_omap_.get() : hot_omap_.get();

    target->begin_step_search(key, nullptr);
    other->begin_step_partial_dummy();
    run_interleaved_loop(target, other);
    Bytes ref = target->step_finish();
    other->step_finish();

    int blk = ref_to_blk(ref);
    Bytes raw = data_access(blk);
    data_oram_.dummy_access();

    auto [val, meta] = decode_epoch(raw);
    meta = maint_->update_meta(meta);
    Bytes wb = encode_with_epoch(new_value ? *new_value : val, meta);
    result.value = new_value ? *new_value : val;
    result.last_access_fp = meta.fp;

    data_access(blk, &wb);
    data_oram_.dummy_access();

    auto* da_hot = static_cast<DaOstOmap*>(hot_omap_.get());

    da_hot->begin_step_scan();
    cold_omap_->begin_step_partial_dummy();
    run_interleaved_loop(da_hot, cold_omap_.get());
    cold_omap_->step_finish();
    auto sr = da_hot->step_finish_scan();

    if (sr.key != INVALID_KEY && !sr.value.empty()) {
        int scan_blk = ref_to_blk(sr.value);
        Bytes scan_raw = data_access(scan_blk);
        auto [sv, sm] = decode_epoch(scan_raw);
        if (maint_->should_demote_cache(sm.fp))
            da_pending_demotions_.push_back({sr.key, sm.fp});
    }

    result.hot_bw = hot_omap_->last_stats();
    result.cold_bw = cold_omap_->last_stats();
    auto data_bw = data_oram_.last_stats();

    result.total_bw.bytes_downloaded =
        result.hot_bw.bytes_downloaded + result.cold_bw.bytes_downloaded +
        data_bw.bytes_downloaded;
    result.total_bw.bytes_uploaded =
        result.hot_bw.bytes_uploaded + result.cold_bw.bytes_uploaded +
        data_bw.bytes_uploaded;
    result.total_bw.rounds =
        std::max(result.hot_bw.rounds, result.cold_bw.rounds) +
        data_bw.rounds;

    if (is_hot_logical)
        result.rounds_to_answer = static_cast<int>(result.hot_bw.rounds) + 1;
    else
        result.rounds_to_answer = static_cast<int>(result.total_bw.rounds);

    return result;
}

// ─── State export / import ─────────────────────────────────────────────────

static Bytes export_omap_by_backend(OmapInterface* omap, OmapBackend be) {
    switch (be) {
    case OmapBackend::AVL:
        return static_cast<AVLOmap*>(omap)->export_state();
    case OmapBackend::BPlus:
        return static_cast<BPlusOmap*>(omap)->export_state();
    case OmapBackend::DaAvl:
    case OmapBackend::DaBplus:
        return static_cast<DaOstOmap*>(omap)->export_state();
    }
    return {};
}

static std::unique_ptr<OmapInterface> restore_omap_by_backend(
    OmapBackend be, const uint8_t*& p, std::shared_ptr<TcpChannel> channel) {
    switch (be) {
    case OmapBackend::AVL:
        return AVLOmap::from_state(p, channel);
    case OmapBackend::BPlus:
        return BPlusOmap::from_state(p, channel);
    case OmapBackend::DaAvl:
    case OmapBackend::DaBplus:
        return DaOstOmap::from_state(p, channel);
    }
    return nullptr;
}

Bytes TieredOMap::export_state() const {
    Bytes buf;
    auto si = [&](int v){ size_t p=buf.size(); buf.resize(p+4); std::memcpy(buf.data()+p,&v,4); };

    si(config_.total_keys); si(config_.hot_set_size);
    si(static_cast<int>(config_.mode)); si(config_.use_split_oram ? 1 : 0);
    si(config_.bucket_size); si(static_cast<int>(config_.backend));
    si(config_.bplus_order); si(hot_capacity_);
    si(next_data_block_id_);
    si(config_.use_hot_backend ? 1 : 0);
    si(static_cast<int>(config_.hot_backend));

    si(static_cast<int>(hot_keys_.size()));
    for (int k : hot_keys_) si(k);
    si(static_cast<int>(phys_hot_keys_.size()));
    for (int k : phys_hot_keys_) si(k);
    si(static_cast<int>(hot_key_list_.size()));
    for (int k : hot_key_list_) si(k);

    auto data_blob = data_oram_.export_state(-1);
    si(static_cast<int>(data_blob.size()));
    buf.insert(buf.end(), data_blob.begin(), data_blob.end());

    auto hot_blob = export_omap_by_backend(hot_omap_.get(), config_.effective_hot_backend());
    si(static_cast<int>(hot_blob.size()));
    buf.insert(buf.end(), hot_blob.begin(), hot_blob.end());

    auto cold_blob = export_omap_by_backend(cold_omap_.get(), config_.backend);
    si(static_cast<int>(cold_blob.size()));
    buf.insert(buf.end(), cold_blob.begin(), cold_blob.end());

    return buf;
}

std::unique_ptr<TieredOMap> TieredOMap::from_state(
    const uint8_t*& p, std::shared_ptr<TcpChannel> channel) {
    auto di = [&]() -> int { int v; std::memcpy(&v,p,4); p+=4; return v; };

    TieredOMapConfig cfg;
    cfg.total_keys = di(); cfg.hot_set_size = di();
    cfg.mode = static_cast<SecurityMode>(di());
    cfg.use_split_oram = (di() != 0);
    cfg.bucket_size = di(); cfg.backend = static_cast<OmapBackend>(di());
    cfg.bplus_order = di();
    int hot_capacity = di();
    int next_data_blk = di();
    cfg.use_hot_backend = (di() != 0);
    cfg.hot_backend = static_cast<OmapBackend>(di());

    int hk_sz = di();
    std::unordered_set<int> hot_keys;
    for (int i = 0; i < hk_sz; ++i) hot_keys.insert(di());
    int phk_sz = di();
    std::unordered_set<int> phys_hot_keys;
    for (int i = 0; i < phk_sz; ++i) phys_hot_keys.insert(di());
    int hkl_sz = di();
    std::vector<int> hot_key_list(hkl_sz);
    for (int i = 0; i < hkl_sz; ++i) hot_key_list[i] = di();

    int data_len = di(); (void)data_len;
    auto data_oram = PathORAM::from_state_network(p, channel);

    int hot_len = di(); (void)hot_len;
    auto hot_omap = restore_omap_by_backend(cfg.effective_hot_backend(), p, channel);

    int cold_len = di(); (void)cold_len;
    auto cold_omap = restore_omap_by_backend(cfg.backend, p, channel);

    auto tm = std::make_unique<TieredOMap>(cfg);
    tm->channel_ = channel;
    tm->data_oram_ = std::move(data_oram);
    tm->next_data_block_id_ = next_data_blk;
    tm->hot_omap_ = std::move(hot_omap);
    tm->cold_omap_ = std::move(cold_omap);
    tm->hot_keys_ = std::move(hot_keys);
    tm->phys_hot_keys_ = std::move(phys_hot_keys);
    tm->hot_key_list_ = std::move(hot_key_list);
    tm->hot_capacity_ = hot_capacity;
    return tm;
}

}  // namespace tiered_omap
