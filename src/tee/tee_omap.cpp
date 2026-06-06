#include "tiered_omap/tee/tee_omap.h"
#include <algorithm>
#include <stdexcept>

namespace tiered_omap {
namespace tee {

// ── Helpers ─────────────────────────────────────────────────────────────────

int TeeOmap::stored_value_size() const {
    return config_.maintenance.enabled
         ? config_.value_size + static_cast<int>(EPOCH_META_SIZE)
         : config_.value_size;
}

Bytes TeeOmap::wrap_value(const Bytes& val, const EpochMeta& m) const {
    if (!config_.maintenance.enabled) return val;
    return encode_with_epoch(pad_bytes(val, config_.value_size), m);
}

std::pair<Bytes, EpochMeta> TeeOmap::unwrap_value(const Bytes& stored) const {
    if (!config_.maintenance.enabled) return {stored, {}};
    return decode_epoch(stored);
}

EpochMeta TeeOmap::bump_epoch(const EpochMeta& old_meta) const {
    EpochMeta m = old_meta;
    int stale = o_less(m.ep, current_epoch_);
    int fresh_cnt = m.cnt + 1;
    m.cnt = o_select_i(stale, 1, fresh_cnt);
    m.ep  = o_select_i(stale, current_epoch_, m.ep);
    return m;
}

// ── Constructor ─────────────────────────────────────────────────────────────

TeeOmap::TeeOmap(const TeeOmapConfig& config) : config_(config) {
    int n = config_.hot_set_size;
    int N = config_.total_keys;

    hot_capacity_ = config_.maintenance.enabled
                  ? std::max(n * 2, n + 64)
                  : n;

    int sv = stored_value_size();
    hot_dir_ = std::make_unique<PackedDirectory>(hot_capacity_);
    hot_oram_ = std::make_unique<EnclaveOram>(
        hot_capacity_, sv, config_.bucket_size, 7, config_.oram_layout);

    int cold_cap = config_.maintenance.enabled
                 ? std::max(N - n, N)
                 : N;
    int split_depth = config_.use_split_oram
                    ? std::max(1, ceil_log2(std::max(n, 2)))
                    : 0;
    cold_omap_ = std::make_unique<TeeAvlOmap>(
        cold_cap, sv, config_.bucket_size, split_depth,
        config_.oram_layout);
}

// ── Init ────────────────────────────────────────────────────────────────────

void TeeOmap::init(const std::vector<std::pair<int, Bytes>>& all_data,
                   const std::vector<int>& hot_keys) {
    std::unordered_set<int> hk_set(hot_keys.begin(), hot_keys.end());

    std::vector<std::pair<int, Bytes>> hot_data, cold_data;
    for (auto& [k, v] : all_data) {
        Bytes stored = wrap_value(v);
        if (hk_set.count(k))
            hot_data.push_back({k, stored});
        else
            cold_data.push_back({k, stored});
    }

    int real_hot = static_cast<int>(hot_data.size());
    Bytes dummy_val(stored_value_size(), 0);
    for (int i = 0; i < hot_capacity_ - real_hot; ++i)
        hot_data.push_back({-(i + 1), dummy_val});

    auto hot_leaves = hot_oram_->init(hot_data);
    for (auto& [k, leaf] : hot_leaves) {
        if (k >= 0)
            hot_dir_->insert(k, leaf);
    }

    cold_omap_->init(cold_data);
}

// ── Doubly-oblivious access ─────────────────────────────────────────────────

TeeAccessResult TeeOmap::access(int key, const Bytes* new_value,
                                ResponseCallback early_cb,
                                ResponseCallback final_cb) {
    TeeAccessResult result;
    auto& mcfg = config_.maintenance;
    int sv = stored_value_size();

    // ── Phase 1: Hot tier ────────────────────────────────────────────────
    // Directory lookup is always needed to determine tier membership.
    int old_pos = hot_dir_->lookup(key);
    int hot_hit_i = 1 - o_equal(old_pos, INVALID_LEAF);

    int dir_pages = (hot_dir_->capacity() * 20 + EnclaveOram::PAGE_SIZE - 1)
                  / EnclaveOram::PAGE_SIZE;

    // In TM mode, cold queries can skip the hot ORAM entirely (tier
    // membership is public). In FO mode, always access both tiers.
    bool is_tm = (config_.mode == TeeSecurityMode::TierMembership);
    bool skip_hot_oram = is_tm && (hot_hit_i == 0);

    Bytes hot_val(config_.value_size, 0);
    EpochMeta hot_meta{};
    uint64_t hot_oram_pages = 0;

    if (!skip_hot_oram) {
        int new_hot_leaf = hot_oram_->random_leaf();
        int dummy_leaf = hot_oram_->random_leaf();
        int use_leaf = o_select_i(hot_hit_i, old_pos, dummy_leaf);

        hot_oram_->begin_page_tracking();
        hot_oram_->read_path_to_stash(use_leaf);
        Block blk = hot_oram_->extract_from_stash(
            o_select_i(hot_hit_i, key, -2));

        auto decoded = unwrap_value(blk.value);
        hot_val = decoded.first;
        hot_meta = decoded.second;

        EpochMeta new_hot_meta = bump_epoch(hot_meta);

        Bytes wb_val = pad_bytes(hot_val, config_.value_size);
        if (new_value) {
            Bytes nv_padded = pad_bytes(*new_value, config_.value_size);
            o_mov_bytes(hot_hit_i, wb_val, nv_padded);
        }
        Bytes wb_stored = wrap_value(wb_val, new_hot_meta);
        wb_stored = pad_bytes(wb_stored, sv);

        hot_oram_->add_to_stash(
            o_select_i(hot_hit_i, key, blk.key), new_hot_leaf, wb_stored);
        hot_oram_->evict_one_path(use_leaf);
        hot_oram_pages = hot_oram_->end_page_tracking();

        hot_dir_->update_pos(key, o_select_i(hot_hit_i, new_hot_leaf, INVALID_LEAF));
    }

    if (mcfg.enabled)
        hot_dir_->bump_freq(key, current_epoch_);

    result.hot_pages = static_cast<uint64_t>(dir_pages) * 2
                     + (skip_hot_oram ? 0 : hot_oram_pages);

    result.value.resize(config_.value_size, 0);
    Bytes padded_hot = pad_bytes(hot_val, config_.value_size);
    o_mov_bytes(hot_hit_i, result.value, padded_hot);
    result.found_in_hot = (hot_hit_i != 0);

    if (early_cb)
        early_cb(result.value, result.found_in_hot);

    // ── Phase 2: Cold tier ───────────────────────────────────────────────
    // In TM mode, tier membership is public. Hot queries skip the cold
    // tier entirely; cold queries skip the hot ORAM (directory lookup
    // above is always needed to determine membership).
    // In FO mode, both tiers are always accessed (dummy when not needed).
    int cold_real = 1 - hot_hit_i;
    bool skip_cold = is_tm && (hot_hit_i != 0);

    Bytes cold_stored(stored_value_size(), 0);
    EpochMeta cold_meta{};

    if (!skip_cold) {
        TeeAvlOmap::ValueTransform cold_transform = nullptr;
        if (mcfg.enabled) {
            cold_transform = [&](const Bytes& old_stored) -> Bytes {
                auto [cv, cm] = unwrap_value(old_stored);
                EpochMeta nm = bump_epoch(cm);
                Bytes wv = pad_bytes(cv, config_.value_size);
                if (new_value) {
                    Bytes nv_padded = pad_bytes(*new_value, config_.value_size);
                    o_mov_bytes(cold_real, wv, nv_padded);
                }
                return wrap_value(wv, nm);
            };
        }

        const Bytes* cold_update = (!mcfg.enabled && new_value) ? new_value : nullptr;
        cold_stored = cold_omap_->search_or_dummy(
            key, cold_real != 0, cold_update, cold_transform);

        auto decoded = unwrap_value(cold_stored);
        cold_meta = decoded.second;

        // Obliviously select result from cold tier.
        Bytes padded_cold = pad_bytes(decoded.first, config_.value_size);
        o_mov_bytes(cold_real, result.value, padded_cold);
    }

    result.cold_up_pages = skip_cold ? 0 : cold_omap_->last_stats().upper_pages;
    result.cold_low_pages = skip_cold ? 0 : cold_omap_->last_stats().lower_pages;
    result.total_pages = result.hot_pages + result.cold_up_pages
                       + result.cold_low_pages;

    // ── Final response ───────────────────────────────────────────────────
    if (final_cb)
        final_cb(result.value, !result.found_in_hot);

    // ── Frequency tracking & maintenance ─────────────────────────────────
    if (mcfg.enabled) {
        int cold_freq_ok = 1 - o_less(cold_meta.cnt, mcfg.promote_threshold);
        int is_promo_candidate = cold_real & cold_freq_ok;
        o_mov_i(is_promo_candidate, pending_promo_key_, key);
        Bytes pv = pad_bytes(
            skip_cold ? Bytes(config_.value_size, 0)
                      : unwrap_value(cold_stored).first,
            config_.value_size);
        o_mov_bytes(is_promo_candidate, pending_promo_val_, pv);

        ++access_counter_;
        current_epoch_ = access_counter_ / mcfg.observation_window;
        if (access_counter_ % mcfg.swap_interval == 0 && access_counter_ > 0) {
            do_maintenance_step();
        }
    }

    return result;
}

TeeBatchAccessResult TeeOmap::access_batch_tier_membership(
        const std::vector<int>& keys,
        BatchResponseCallback hot_release_cb,
        BatchResponseCallback final_cb) {
    if (config_.mode != TeeSecurityMode::TierMembership) {
        throw std::runtime_error(
            "access_batch_tier_membership requires TierMembership mode");
    }
    if (config_.maintenance.enabled) {
        throw std::runtime_error(
            "access_batch_tier_membership currently supports static TEE runs only");
    }

    TeeBatchAccessResult batch;
    batch.results.resize(keys.size());
    std::vector<size_t> cold_indices;
    cold_indices.reserve(keys.size());

    int sv = stored_value_size();
    int dir_pages = (hot_dir_->capacity() * 20 + EnclaveOram::PAGE_SIZE - 1)
                  / EnclaveOram::PAGE_SIZE;

    // Phase 1: every logical query first goes through the hot directory phase.
    // Hot hits also read/update the hot ORAM; cold queries continue later.
    for (size_t i = 0; i < keys.size(); ++i) {
        int key = keys[i];
        auto& r = batch.results[i];
        r.value.assign(config_.value_size, 0);

        int old_pos = hot_dir_->lookup(key);
        bool hot_hit = (old_pos != INVALID_LEAF);
        r.found_in_hot = hot_hit;
        r.hot_pages = static_cast<uint64_t>(dir_pages) * 2;

        if (hot_hit) {
            int new_hot_leaf = hot_oram_->random_leaf();

            hot_oram_->begin_page_tracking();
            hot_oram_->read_path_to_stash(old_pos);
            Block blk = hot_oram_->extract_from_stash(key);

            auto decoded = unwrap_value(blk.value);
            Bytes hot_val = pad_bytes(decoded.first, config_.value_size);
            EpochMeta new_hot_meta = bump_epoch(decoded.second);

            Bytes wb_stored = wrap_value(hot_val, new_hot_meta);
            wb_stored = pad_bytes(wb_stored, sv);

            hot_oram_->add_to_stash(key, new_hot_leaf, wb_stored);
            hot_oram_->evict_one_path(old_pos);
            r.hot_pages += hot_oram_->end_page_tracking();

            hot_dir_->update_pos(key, new_hot_leaf);

            r.value = hot_val;
            ++batch.hot_count;
        } else {
            cold_indices.push_back(i);
            ++batch.cold_count;
        }

        batch.hot_phase_pages += r.hot_pages;
    }

    if (hot_release_cb)
        hot_release_cb(batch.results);

    // Phase 2: only cold continuations enter the cold OMAP. This is the actual
    // batch-level TM execution shape used by the TEE BatchTM benchmark.
    for (size_t idx : cold_indices) {
        int key = keys[idx];
        auto& r = batch.results[idx];

        Bytes cold_stored = cold_omap_->search_or_dummy(
            key, true, nullptr, nullptr);
        auto decoded = unwrap_value(cold_stored);
        r.value = pad_bytes(decoded.first, config_.value_size);
        r.cold_up_pages = cold_omap_->last_stats().upper_pages;
        r.cold_low_pages = cold_omap_->last_stats().lower_pages;
        batch.cold_extra_pages += r.cold_up_pages + r.cold_low_pages;
    }

    for (auto& r : batch.results) {
        r.total_pages = r.hot_pages + r.cold_up_pages + r.cold_low_pages;
        batch.total_pages += r.total_pages;
    }

    if (final_cb)
        final_cb(batch.results);

    return batch;
}

// ── Maintenance: one demote + one promote per epoch boundary ─────────────

void TeeOmap::do_maintenance_step() {
    auto& mcfg = config_.maintenance;

    // ── Demotion: oblivious scan of hot directory ────────────────────────
    int demote_key = hot_dir_->find_demote_candidate(
        scan_ptr_, current_epoch_, mcfg.staleness_windows, mcfg.demote_threshold);
    scan_ptr_ = (scan_ptr_ + 1) % std::max(hot_dir_->capacity(), 1);

    // Always execute demote ops (real or dummy) for fixed access pattern.
    // demote_key is INVALID_KEY when no candidate found → demote() handles it.
    demote(demote_key);

    // ── Promotion ────────────────────────────────────────────────────────
    int promo_key = pending_promo_key_;
    Bytes promo_val = pending_promo_val_;

    // Check if candidate is still cold (not already hot).
    int already_hot = 1 - o_equal(hot_dir_->lookup(promo_key), INVALID_LEAF);
    int capacity_ok = (hot_dir_->size() < hot_capacity_) ? 1 : 0;
    int do_promo = (1 - o_equal(promo_key, INVALID_KEY))
                 & (1 - already_hot) & capacity_ok;
    int eff_key = o_select_i(do_promo, promo_key, INVALID_KEY);

    promote(eff_key, promo_val);

    // Clear pending.
    pending_promo_key_ = INVALID_KEY;
    pending_promo_val_.assign(config_.value_size, 0);
}

// ── Promote / Demote (fixed ORAM access pattern) ────────────────────────

void TeeOmap::promote(int key, const Bytes& value) {
    int is_real = 1 - o_equal(key, INVALID_KEY);

    // Cold OMAP remove: traverses the AVL tree (3*max_height steps).
    // When key=INVALID_KEY, traverses but finds nothing → no-op.
    cold_omap_->remove(key);

    // Hot ORAM: add the value (or dummy block).
    int leaf = hot_oram_->random_leaf();
    hot_oram_->read_path_to_stash(leaf);

    Bytes stored = wrap_value(value);
    stored = pad_bytes(stored, stored_value_size());
    hot_oram_->add_to_stash(
        o_select_i(is_real, key, INVALID_KEY), leaf, stored);
    hot_oram_->evict_one_path(leaf);

    // Hot directory: insert handles INVALID_KEY as dummy (full scan, no insert).
    hot_dir_->insert(key, leaf);
}

void TeeOmap::demote(int key) {
    int is_real = 1 - o_equal(key, INVALID_KEY);

    // Hot ORAM: read the value out (or dummy read).
    int old_leaf = hot_dir_->lookup(key);
    int use_old = o_select_i(is_real, old_leaf, hot_oram_->random_leaf());

    hot_oram_->read_path_to_stash(use_old);
    Block blk = hot_oram_->extract_from_stash(
        o_select_i(is_real, key, -2));

    // Real: don't add back (removing from hot). Dummy: add the block back.
    hot_oram_->add_to_stash(
        o_select_i(is_real, INVALID_KEY, blk.key),
        hot_oram_->random_leaf(), blk.value);
    hot_oram_->evict_one_path(use_old);

    // Hot directory: remove handles INVALID_KEY as dummy (full scan, no remove).
    hot_dir_->remove(key);

    // Cold OMAP: insert value (INVALID_KEY → dummy insert, same ORAM pattern).
    auto [val, meta] = unwrap_value(blk.value);
    Bytes stored = wrap_value(val, meta);
    stored = pad_bytes(stored, stored_value_size());
    cold_omap_->insert(o_select_i(is_real, key, INVALID_KEY), stored);
}

}  // namespace tee
}  // namespace tiered_omap
