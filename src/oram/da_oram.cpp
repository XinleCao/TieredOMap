#include "tiered_omap/oram/da_oram.h"
#include "tiered_omap/network/network_storage.h"
#include <algorithm>
#include <cmath>
#include <functional>
#include <stdexcept>
#include <unordered_set>

namespace tiered_omap {

// ─── CounterBlock serialization ─────────────────────────────────────────────

Bytes CounterBlock::encode() const {
    Bytes b(8 + ic.size() + backup.size());
    std::memcpy(b.data(), &gc, 8);
    if (!ic.empty())
        std::memcpy(b.data() + 8, ic.data(), ic.size());
    if (!backup.empty())
        std::memcpy(b.data() + 8 + ic.size(), backup.data(), backup.size());
    return b;
}

CounterBlock CounterBlock::decode(const Bytes& data, int num_ic) {
    CounterBlock cb(num_ic);
    size_t needed = 8 + 2 * static_cast<size_t>(num_ic);
    if (data.size() >= needed) {
        std::memcpy(&cb.gc, data.data(), 8);
        std::memcpy(cb.ic.data(), data.data() + 8, num_ic);
        std::memcpy(cb.backup.data(), data.data() + 8 + num_ic, num_ic);
    }
    return cb;
}

// ─── Constructor ────────────────────────────────────────────────────────────

DAOram::DAOram(int num_data, int bucket_size, int stash_scale,
               int num_ic, int ic_max, int on_chip_mem,
               StorageCreator storage_creator)
    : num_data_(num_data),
      bucket_size_(bucket_size),
      num_ic_(num_ic),
      ic_max_(ic_max),
      on_chip_mem_(on_chip_mem),
      storage_creator_(std::move(storage_creator)) {

    if (storage_creator_)
        storage_ = storage_creator_(num_data, bucket_size);
    else
        storage_ = std::make_unique<BinaryTreeStorage>(num_data, bucket_size);
    cached_level_ = storage_->level();
    cached_leaf_range_ = storage_->leaf_range();
    stash_max_size_ = stash_scale * std::max(cached_level_ - 1, 1);

    // Compute recursive pos_map level sizes (bottom to top).
    int cur_size = num_data;
    while (true) {
        int groups = (cur_size + num_ic_ - 1) / num_ic_;
        if (groups <= on_chip_mem_) break;
        level_sizes_.push_back(groups);
        cur_size = groups;
    }
    num_pos_levels_ = static_cast<int>(level_sizes_.size());

    // Create pos_map PathORAMs.
    for (int i = 0; i < num_pos_levels_; ++i) {
        if (storage_creator_)
            pos_maps_.emplace_back(level_sizes_[i], bucket_size, 7,
                                   storage_creator_);
        else
            pos_maps_.emplace_back(level_sizes_[i], bucket_size, 7);
    }

    // Allocate on-chip counter blocks.
    int on_chip_managed = (num_pos_levels_ > 0) ? level_sizes_.back() : num_data;
    int on_chip_groups = (on_chip_managed + num_ic_ - 1) / num_ic_;
    on_chip_.resize(on_chip_groups, CounterBlock(num_ic_));

    prf_seed_ = static_cast<uint64_t>(std::random_device{}());
}

// ─── PRF ────────────────────────────────────────────────────────────────────

int DAOram::prf_leaf(int key, uint64_t gc, uint8_t ic, int lr) const {
    if (lr <= 0) return 0;
    std::hash<uint64_t> h;
    uint64_t v = prf_seed_;
    v ^= h(static_cast<uint64_t>(key)) + 0x9e3779b9 + (v << 6) + (v >> 2);
    v ^= h(gc) + 0x9e3779b9 + (v << 6) + (v >> 2);
    v ^= h(static_cast<uint64_t>(ic)) + 0x9e3779b9 + (v << 6) + (v >> 2);
    return static_cast<int>(v % static_cast<uint64_t>(lr));
}

int DAOram::prf_leaf_data(int key, uint64_t gc, uint8_t ic) const {
    return prf_leaf(key, gc, ic, leaf_range());
}

// ─── Counter manipulation ───────────────────────────────────────────────────

std::pair<int, int> DAOram::update_counter_in(
    CounterBlock& cb, int offset, int managed_key, int lr) {
    uint64_t gc = cb.gc;
    uint8_t cur_ic = cb.ic[offset];
    uint64_t next_gc;
    uint8_t next_ic;

    if (cb.backup[offset] == 1) {
        next_ic = 0;
        next_gc = gc;
        gc = gc - 1;
        cb.backup[offset] = 0;
    } else if (cur_ic + 1 >= ic_max_) {
        next_ic = 0;
        next_gc = gc + 1;
        cb.gc = next_gc;
        for (int i = 0; i < num_ic_; ++i) {
            if (i != offset) cb.backup[i] = 1;
        }
    } else {
        next_ic = cur_ic + 1;
        next_gc = gc;
    }

    cb.ic[offset] = next_ic;

    int cur_leaf = prf_leaf(managed_key, gc, cur_ic, lr);
    int new_leaf = prf_leaf(managed_key, next_gc, next_ic, lr);
    return {cur_leaf, new_leaf};
}

std::tuple<int, int, int> DAOram::perform_reset_in(
    CounterBlock& cb, int base_key, int num_managed, int lr) {
    for (int i = 0; i < num_ic_; ++i) {
        if (cb.backup[i] == 1) {
            int managed_key = base_key + i;
            if (managed_key >= num_managed) continue;

            uint64_t gc = cb.gc;
            uint8_t ic_val = cb.ic[i];
            uint64_t old_gc = gc - 1;
            int cur_leaf = prf_leaf(managed_key, old_gc, ic_val, lr);
            int new_leaf = prf_leaf(managed_key, gc, 0, lr);

            cb.ic[i] = 0;
            cb.backup[i] = 0;
            return {managed_key, cur_leaf, new_leaf};
        }
    }
    return {-1, SecureRandom::rand_below(std::max(lr, 1)), -1};
}

// ─── Init ───────────────────────────────────────────────────────────────────

void DAOram::init(const std::unordered_map<int, Bytes>& data) {
    block_size_bytes_ = 0;
    stash_.clear();

    for (auto& [key, value] : data) {
        if (static_cast<int>(value.size()) > block_size_bytes_)
            block_size_bytes_ = static_cast<int>(value.size());
    }

    // Reset on-chip counter blocks.
    int on_chip_managed = (num_pos_levels_ > 0) ? level_sizes_.back() : num_data_;
    int on_chip_groups = (on_chip_managed + num_ic_ - 1) / num_ic_;
    on_chip_.assign(on_chip_groups, CounterBlock(num_ic_));

    // Initialize pos_map ORAMs with zero-valued counter blocks.
    for (int level = num_pos_levels_ - 1; level >= 0; --level) {
        PathORAM& pm = pos_maps_[level];
        int num_blocks = level_sizes_[level];

        // Compute initial leaf via PRF(key, gc=0, ic=0).
        for (int k = 0; k < num_blocks; ++k) {
            int init_leaf = prf_leaf(k, 0, 0, pm.leaf_range());
            pm.set_leaf(k, init_leaf);
        }

        CounterBlock zero_cb(num_ic_);
        Bytes zero_bytes = zero_cb.encode();

        std::unordered_map<int, Bytes> pm_data;
        for (int k = 0; k < num_blocks; ++k)
            pm_data[k] = zero_bytes;
        pm.init(pm_data);
    }

    // Initialize data ORAM.
    storage_->reset(num_data_, bucket_size_);

    std::vector<Block> blocks;
    blocks.reserve(data.size());
    for (auto& [key, value] : data) {
        int leaf = prf_leaf_data(key, 0, 0);
        blocks.push_back({key, leaf, value});
    }

    auto placed = storage_->bulk_load(blocks);
    for (auto& [key, value] : data) {
        if (placed.find(key) == placed.end()) {
            int leaf = prf_leaf_data(key, 0, 0);
            stash_.push_back({key, leaf, value});
        }
    }
}

// ─── Pos-map traversal (local) ──────────────────────────────────────────────

DAOram::PosMapResult DAOram::traverse_pos_maps(int data_key) {
    // Compute keys at each pos_map level (bottom to top).
    std::vector<int> pm_keys;
    int cur = data_key;
    for (int i = 0; i < num_pos_levels_; ++i) {
        cur = cur / num_ic_;
        pm_keys.push_back(cur);
    }

    // Determine on-chip group and managed level info.
    int managed_key, on_chip_offset, on_chip_group;
    int managed_lr, managed_total;

    if (num_pos_levels_ > 0) {
        managed_key = pm_keys.back();
        on_chip_group = managed_key / num_ic_;
        on_chip_offset = managed_key % num_ic_;
        managed_lr = pos_maps_.back().leaf_range();
        managed_total = level_sizes_.back();
    } else {
        managed_key = data_key;
        on_chip_group = data_key / num_ic_;
        on_chip_offset = data_key % num_ic_;
        managed_lr = leaf_range();
        managed_total = num_data_;
    }

    auto& oc_cb = on_chip_[on_chip_group];
    auto [cur_leaf, new_leaf] = update_counter_in(
        oc_cb, on_chip_offset, managed_key, managed_lr);
    int base = on_chip_group * num_ic_;
    auto [r_key, r_cur, r_new] = perform_reset_in(
        oc_cb, base, managed_total, managed_lr);

    // Traverse pos_map levels top-down.
    for (int level = num_pos_levels_ - 1; level >= 0; --level) {
        PathORAM& pm = pos_maps_[level];
        pm.read_multiple_paths_to_stash({cur_leaf, r_cur});

        int target_key = pm_keys[level];
        Block* target = pm.find_in_stash(target_key);
        if (!target)
            throw std::runtime_error(
                "DAOram::traverse_pos_maps: block " +
                std::to_string(target_key) + " not found at level " +
                std::to_string(level));

        CounterBlock tcb = CounterBlock::decode(target->value, num_ic_);

        int next_key, next_off, next_lr, next_total;
        if (level == 0) {
            next_key = data_key;
            next_off = data_key % num_ic_;
            next_lr = leaf_range();
            next_total = num_data_;
        } else {
            next_key = pm_keys[level - 1];
            next_off = pm_keys[level - 1] % num_ic_;
            next_lr = pos_maps_[level - 1].leaf_range();
            next_total = level_sizes_[level - 1];
        }

        auto [next_cur, next_new] = update_counter_in(
            tcb, next_off, next_key, next_lr);
        int tbase = target_key * num_ic_;
        auto [r2_key, r2_cur, r2_new] = perform_reset_in(
            tcb, tbase, next_total, next_lr);

        target->value = tcb.encode();
        target->leaf = new_leaf;

        if (r_key >= 0) {
            Block* rb = pm.find_in_stash(r_key);
            if (rb) rb->leaf = r_new;
        }

        pm.evict_and_write_paths({cur_leaf, r_cur});

        cur_leaf = next_cur;
        new_leaf = next_new;
        r_key = r2_key;
        r_cur = r2_cur;
        r_new = r2_new;
    }

    return {cur_leaf, r_cur, new_leaf, r_key, r_new};
}

DAOram::PosMapResult DAOram::traverse_pos_maps_dummy() {
    for (int level = num_pos_levels_ - 1; level >= 0; --level) {
        PathORAM& pm = pos_maps_[level];
        int l1 = pm.random_leaf();
        int l2 = pm.random_leaf();
        pm.read_multiple_paths_to_stash({l1, l2});
        pm.evict_and_write_paths({l1, l2});
    }
    int dl1 = SecureRandom::rand_below(std::max(leaf_range(), 1));
    int dl2 = SecureRandom::rand_below(std::max(leaf_range(), 1));
    return {dl1, dl2, -1, -1, -1};
}

// ─── Access ─────────────────────────────────────────────────────────────────

Bytes DAOram::access(int key, const Bytes* new_value) {
    last_bw_.reset();

    auto pm = traverse_pos_maps(key);

    {
        auto merged = storage_->read_multiple_paths({pm.leaf1, pm.leaf2});
        for (auto& [node, bucket] : merged)
            for (auto& block : bucket)
                if (!block.is_dummy()) stash_.push_back(block);
    }

    Block* target = find_in_stash(key);
    if (!target)
        throw std::runtime_error("DAOram::access: key " +
                                 std::to_string(key) + " not found");
    Bytes result = target->value;
    if (new_value) target->value = *new_value;
    target->leaf = pm.new_leaf;

    if (pm.r_key >= 0) {
        Block* r_block = find_in_stash(pm.r_key);
        if (r_block) r_block->leaf = pm.r_new_leaf;
    }

    evict_and_write_paths({pm.leaf1, pm.leaf2});

    last_bw_.rounds = num_pos_levels_ + 1;
    int path_blocks = storage_->level() * bucket_size_;
    last_bw_.bytes_downloaded = 2 * path_blocks * (block_size_bytes_ + 8);
    last_bw_.bytes_uploaded = last_bw_.bytes_downloaded;
    for (int i = 0; i < num_pos_levels_; ++i) {
        int pm_path = pos_maps_[i].level() * bucket_size_;
        int pm_bs = 8 + 2 * num_ic_;
        last_bw_.bytes_downloaded += 2 * pm_path * (pm_bs + 8);
        last_bw_.bytes_uploaded += 2 * pm_path * (pm_bs + 8);
    }
    total_bw_ += last_bw_;

    return result;
}

void DAOram::dummy_access() {
    last_bw_.reset();

    auto pm = traverse_pos_maps_dummy();

    {
        auto merged = storage_->read_multiple_paths({pm.leaf1, pm.leaf2});
        for (auto& [node, bucket] : merged)
            for (auto& block : bucket)
                if (!block.is_dummy()) stash_.push_back(block);
    }
    evict_and_write_paths({pm.leaf1, pm.leaf2});

    last_bw_.rounds = num_pos_levels_ + 1;
    int path_blocks = storage_->level() * bucket_size_;
    last_bw_.bytes_downloaded = 2 * path_blocks * (block_size_bytes_ + 8);
    last_bw_.bytes_uploaded = last_bw_.bytes_downloaded;
    for (int i = 0; i < num_pos_levels_; ++i) {
        int pm_path = pos_maps_[i].level() * bucket_size_;
        int pm_bs = 8 + 2 * num_ic_;
        last_bw_.bytes_downloaded += 2 * pm_path * (pm_bs + 8);
        last_bw_.bytes_uploaded += 2 * pm_path * (pm_bs + 8);
    }
    total_bw_ += last_bw_;
}

// ─── Legacy interleaved interface (local pos_map traversal) ─────────────────

DAOram::PreparedAccess DAOram::prepare_interleaved_access(int key) {
    last_bw_.reset();
    auto pm = traverse_pos_maps(key);
    return {pm.leaf1, pm.leaf2, key, pm.new_leaf, pm.r_key, pm.r_new_leaf};
}

DAOram::PreparedAccess DAOram::prepare_interleaved_dummy() {
    last_bw_.reset();
    auto pm = traverse_pos_maps_dummy();
    return {pm.leaf1, pm.leaf2, INVALID_KEY, -1, -1, -1};
}

void DAOram::apply_fetched_paths(
    const std::unordered_map<int, std::vector<Block>>& merged) {
    for (auto& [node, bucket] : merged)
        for (auto& block : bucket)
            if (!block.is_dummy()) stash_.push_back(block);
}

Bytes DAOram::complete_interleaved_access(const PreparedAccess& prep,
                                          const Bytes* new_value) {
    Block* target = find_in_stash(prep.target_key);
    if (!target)
        throw std::runtime_error("DAOram::complete_interleaved_access: key " +
                                 std::to_string(prep.target_key) + " not found");
    Bytes result = target->value;
    if (new_value) target->value = *new_value;
    target->leaf = prep.new_leaf;

    if (prep.r_key >= 0) {
        Block* r_block = find_in_stash(prep.r_key);
        if (r_block) r_block->leaf = prep.r_new_leaf;
    }
    return result;
}

void DAOram::complete_interleaved_dummy() {}

std::unordered_map<int, std::vector<Block>>
DAOram::prepare_eviction_data(const std::vector<int>& leaves) {
    int lvl = level();
    auto indices = BinaryTreeStorage::get_merged_path_indices(lvl, leaves);

    std::unordered_map<int, std::vector<Block>> path;
    for (int idx : indices) path[idx] = {};

    std::vector<Block> remaining;
    for (auto& block : stash_) {
        bool inserted = BinaryTreeStorage::fill_block_to_path(
            block, path, leaves, lvl, bucket_size_);
        if (!inserted) remaining.push_back(std::move(block));
    }
    stash_ = std::move(remaining);

    if (static_cast<int>(stash_.size()) > stash_max_size_) {
        throw std::runtime_error(
            "DAOram: stash overflow (" + std::to_string(stash_.size()) +
            " > " + std::to_string(stash_max_size_) + ")");
    }

    for (auto& [idx, bucket] : path) {
        while (static_cast<int>(bucket.size()) < bucket_size_)
            bucket.push_back(Block{});
    }

    last_bw_.rounds = num_pos_levels_ + 1;
    int path_blocks = lvl * bucket_size_;
    last_bw_.bytes_downloaded = 2 * path_blocks * (block_size_bytes_ + 8);
    last_bw_.bytes_uploaded = last_bw_.bytes_downloaded;
    total_bw_ += last_bw_;

    return path;
}

// ─── Multi-round step interface ─────────────────────────────────────────────

void DAOram::begin_step_access(int key) {
    last_bw_.reset();
    step_ = StepState{};
    step_.active = true;
    step_.data_key = key;
    step_.round = 0;

    // Compute all pos_map keys (bottom to top).
    std::vector<int> pm_keys;
    int cur = key;
    for (int i = 0; i < num_pos_levels_; ++i) {
        cur = cur / num_ic_;
        pm_keys.push_back(cur);
    }

    // Process on-chip level.
    int managed_key, on_chip_offset, on_chip_group;
    int managed_lr, managed_total;

    if (num_pos_levels_ > 0) {
        managed_key = pm_keys.back();
        on_chip_group = managed_key / num_ic_;
        on_chip_offset = managed_key % num_ic_;
        managed_lr = pos_maps_.back().leaf_range();
        managed_total = level_sizes_.back();
    } else {
        managed_key = key;
        on_chip_group = key / num_ic_;
        on_chip_offset = key % num_ic_;
        managed_lr = leaf_range();
        managed_total = num_data_;
    }

    auto& oc_cb = on_chip_[on_chip_group];
    auto [cur_leaf, new_leaf] = update_counter_in(
        oc_cb, on_chip_offset, managed_key, managed_lr);
    int base = on_chip_group * num_ic_;
    auto [r_key, r_cur, r_new] = perform_reset_in(
        oc_cb, base, managed_total, managed_lr);

    step_.cur_leaf1 = cur_leaf;
    step_.cur_leaf2 = r_cur;
    step_.cur_new_leaf = new_leaf;
    step_.cur_r_key = r_key;
    step_.cur_r_new_leaf = r_new;

    if (num_pos_levels_ == 0)
        step_.done = false;
}

void DAOram::begin_step_dummy() {
    last_bw_.reset();
    step_ = StepState{};
    step_.active = true;
    step_.is_dummy = true;
    step_.round = 0;

    if (num_pos_levels_ > 0) {
        int lr = pos_maps_.back().leaf_range();
        step_.cur_leaf1 = SecureRandom::rand_below(std::max(lr, 1));
        step_.cur_leaf2 = SecureRandom::rand_below(std::max(lr, 1));
    } else {
        step_.cur_leaf1 = SecureRandom::rand_below(std::max(leaf_range(), 1));
        step_.cur_leaf2 = SecureRandom::rand_below(std::max(leaf_range(), 1));
    }
}

std::vector<StepReadReq> DAOram::step_get_reads() const {
    if (step_.done || !step_.active) return {};

    int level = num_pos_levels_ - 1 - step_.round;
    int sid;
    if (level >= 0)
        sid = pos_maps_[level].get_store_id();
    else
        sid = get_store_id();

    std::vector<StepReadReq> reads = {{sid, step_.cur_leaf1}, {sid, step_.cur_leaf2}};

    if (pb_step_.active) {
        reads.push_back({sid, pb_step_.cur_leaf1});
        reads.push_back({sid, pb_step_.cur_leaf2});
    }

    return reads;
}

void DAOram::step_apply_reads(const std::vector<PathData>& results) {
    int level = num_pos_levels_ - 1 - step_.round;

    std::unordered_map<int, std::vector<Block>> merged;
    for (auto& pd : results)
        for (auto& [nid, bucket] : pd)
            if (merged.find(nid) == merged.end())
                merged[nid] = bucket;

    if (level >= 0) {
        pos_maps_[level].apply_fetched_path(std::move(merged));
    } else {
        std::unordered_set<int> seen;
        for (auto& b : stash_) seen.insert(b.key);
        for (auto& [node, bucket] : merged)
            for (auto& block : bucket)
                if (!block.is_dummy() && seen.find(block.key) == seen.end()) {
                    stash_.push_back(block);
                    seen.insert(block.key);
                }
    }
}

void DAOram::step_process() {
    int level = num_pos_levels_ - 1 - step_.round;

    step_.write_level = level;
    step_.write_leaf1 = step_.cur_leaf1;
    step_.write_leaf2 = step_.cur_leaf2;
    if (pb_step_.active) {
        pb_step_.write_leaf1 = pb_step_.cur_leaf1;
        pb_step_.write_leaf2 = pb_step_.cur_leaf2;
    }

    // ── Main operation ──
    if (level >= 0 && !step_.is_dummy) {
        PathORAM& pm = pos_maps_[level];

        std::vector<int> pm_keys;
        int cur = step_.data_key;
        for (int i = 0; i < num_pos_levels_; ++i) {
            cur = cur / num_ic_;
            pm_keys.push_back(cur);
        }

        int target_key = pm_keys[level];
        Block* target = pm.find_in_stash(target_key);
        if (!target)
            throw std::runtime_error(
                "DAOram::step_process: block not found at level " +
                std::to_string(level));

        CounterBlock tcb = CounterBlock::decode(target->value, num_ic_);

        int next_key, next_off, next_lr, next_total;
        if (level == 0) {
            next_key = step_.data_key;
            next_off = step_.data_key % num_ic_;
            next_lr = leaf_range();
            next_total = num_data_;
        } else {
            next_key = pm_keys[level - 1];
            next_off = pm_keys[level - 1] % num_ic_;
            next_lr = pos_maps_[level - 1].leaf_range();
            next_total = level_sizes_[level - 1];
        }

        auto [next_cur, next_new] = update_counter_in(
            tcb, next_off, next_key, next_lr);
        int tbase = target_key * num_ic_;
        auto [r2_key, r2_cur, r2_new] = perform_reset_in(
            tcb, tbase, next_total, next_lr);

        target->value = tcb.encode();
        target->leaf = step_.cur_new_leaf;

        if (step_.cur_r_key >= 0) {
            Block* rb = pm.find_in_stash(step_.cur_r_key);
            if (rb) rb->leaf = step_.cur_r_new_leaf;
        }

        step_.cur_leaf1 = next_cur;
        step_.cur_leaf2 = r2_cur;
        step_.cur_new_leaf = next_new;
        step_.cur_r_key = r2_key;
        step_.cur_r_new_leaf = r2_new;

    } else if (level >= 0 && step_.is_dummy) {
        int next_lr;
        if (level > 0)
            next_lr = pos_maps_[level - 1].leaf_range();
        else
            next_lr = leaf_range();
        step_.cur_leaf1 = SecureRandom::rand_below(std::max(next_lr, 1));
        step_.cur_leaf2 = SecureRandom::rand_below(std::max(next_lr, 1));

    } else if (level < 0 && !step_.is_dummy) {
        Block* target = find_in_stash(step_.data_key);
        if (!target)
            throw std::runtime_error(
                "DAOram::step_process: data key " +
                std::to_string(step_.data_key) + " not found");
        step_.result = target->value;
        target->leaf = step_.cur_new_leaf;

        if (step_.cur_r_key >= 0) {
            Block* rb = find_in_stash(step_.cur_r_key);
            if (rb) rb->leaf = step_.cur_r_new_leaf;
        }
    }

    // ── Piggyback operation ──
    if (pb_step_.active) {
        if (level >= 0 && !pb_step_.is_dummy) {
            PathORAM& pm = pos_maps_[level];

            std::vector<int> pb_pm_keys;
            int cur = pb_step_.data_key;
            for (int i = 0; i < num_pos_levels_; ++i) {
                cur = cur / num_ic_;
                pb_pm_keys.push_back(cur);
            }

            int pb_target_key = pb_pm_keys[level];
            Block* pb_target = pm.find_in_stash(pb_target_key);
            if (!pb_target)
                throw std::runtime_error(
                    "DAOram::step_process(pb): block not found at level " +
                    std::to_string(level));

            CounterBlock pb_tcb = CounterBlock::decode(pb_target->value, num_ic_);

            int pb_next_key, pb_next_off, pb_next_lr, pb_next_total;
            if (level == 0) {
                pb_next_key = pb_step_.data_key;
                pb_next_off = pb_step_.data_key % num_ic_;
                pb_next_lr = leaf_range();
                pb_next_total = num_data_;
            } else {
                pb_next_key = pb_pm_keys[level - 1];
                pb_next_off = pb_pm_keys[level - 1] % num_ic_;
                pb_next_lr = pos_maps_[level - 1].leaf_range();
                pb_next_total = level_sizes_[level - 1];
            }

            auto [pb_next_cur, pb_next_new] = update_counter_in(
                pb_tcb, pb_next_off, pb_next_key, pb_next_lr);
            int pb_tbase = pb_target_key * num_ic_;
            auto [pb_r2_key, pb_r2_cur, pb_r2_new] = perform_reset_in(
                pb_tcb, pb_tbase, pb_next_total, pb_next_lr);

            pb_target->value = pb_tcb.encode();
            pb_target->leaf = pb_step_.cur_new_leaf;

            if (pb_step_.cur_r_key >= 0) {
                Block* rb = pm.find_in_stash(pb_step_.cur_r_key);
                if (rb) rb->leaf = pb_step_.cur_r_new_leaf;
            }

            pb_step_.cur_leaf1 = pb_next_cur;
            pb_step_.cur_leaf2 = pb_r2_cur;
            pb_step_.cur_new_leaf = pb_next_new;
            pb_step_.cur_r_key = pb_r2_key;
            pb_step_.cur_r_new_leaf = pb_r2_new;

        } else if (level >= 0 && pb_step_.is_dummy) {
            int next_lr;
            if (level > 0)
                next_lr = pos_maps_[level - 1].leaf_range();
            else
                next_lr = leaf_range();
            pb_step_.cur_leaf1 = SecureRandom::rand_below(std::max(next_lr, 1));
            pb_step_.cur_leaf2 = SecureRandom::rand_below(std::max(next_lr, 1));

        } else if (level < 0 && !pb_step_.is_dummy) {
            Block* pb_target = find_in_stash(pb_step_.data_key);
            if (!pb_target)
                throw std::runtime_error(
                    "DAOram::step_process(pb): data key " +
                    std::to_string(pb_step_.data_key) + " not found");
            pb_step_.result = pb_target->value;
            pb_target->leaf = pb_step_.cur_new_leaf;

            if (pb_step_.cur_r_key >= 0) {
                Block* rb = find_in_stash(pb_step_.cur_r_key);
                if (rb) rb->leaf = pb_step_.cur_r_new_leaf;
            }
        }
    }

    step_.round++;
    if (step_.round > num_pos_levels_)
        step_.done = true;
}

std::vector<StepWriteReq> DAOram::step_get_writes() {
    int level = step_.write_level;

    std::vector<int> leaves = {step_.write_leaf1, step_.write_leaf2};
    if (pb_step_.active) {
        leaves.push_back(pb_step_.write_leaf1);
        leaves.push_back(pb_step_.write_leaf2);
    }

    if (level >= 0) {
        auto data = pos_maps_[level].prepare_eviction_paths(leaves);
        return {{pos_maps_[level].get_store_id(), std::move(data)}};
    } else {
        auto data = prepare_eviction_data(leaves);
        return {{get_store_id(), std::move(data)}};
    }
}

bool DAOram::step_done() const {
    return step_.done;
}

Bytes DAOram::step_finish(const Bytes* new_value) {
    if (new_value && !step_.is_dummy) {
        Block* target = find_in_stash(step_.data_key);
        if (target) target->value = *new_value;
    }
    step_.active = false;
    return step_.result;
}

void DAOram::step_finish_dummy() {
    step_.active = false;
}

// ─── DAOram piggyback access ─────────────────────────────────────────────────

void DAOram::begin_piggyback_access(int key) {
    pb_step_ = PBStepState{};
    pb_step_.active = true;
    pb_step_.data_key = key;

    int managed_key, on_chip_offset, on_chip_group;
    int managed_lr, managed_total;

    std::vector<int> pm_keys;
    int cur = key;
    for (int i = 0; i < num_pos_levels_; ++i) {
        cur = cur / num_ic_;
        pm_keys.push_back(cur);
    }

    if (num_pos_levels_ > 0) {
        managed_key = pm_keys.back();
        on_chip_group = managed_key / num_ic_;
        on_chip_offset = managed_key % num_ic_;
        managed_lr = pos_maps_.back().leaf_range();
        managed_total = level_sizes_.back();
    } else {
        managed_key = key;
        on_chip_group = key / num_ic_;
        on_chip_offset = key % num_ic_;
        managed_lr = leaf_range();
        managed_total = num_data_;
    }

    auto& oc_cb = on_chip_[on_chip_group];
    auto [cur_leaf, new_leaf] = update_counter_in(
        oc_cb, on_chip_offset, managed_key, managed_lr);
    int base = on_chip_group * num_ic_;
    auto [r_key, r_cur, r_new] = perform_reset_in(
        oc_cb, base, managed_total, managed_lr);

    pb_step_.cur_leaf1 = cur_leaf;
    pb_step_.cur_leaf2 = r_cur;
    pb_step_.cur_new_leaf = new_leaf;
    pb_step_.cur_r_key = r_key;
    pb_step_.cur_r_new_leaf = r_new;
}

void DAOram::begin_piggyback_dummy() {
    pb_step_ = PBStepState{};
    pb_step_.active = true;
    pb_step_.is_dummy = true;

    if (num_pos_levels_ > 0) {
        int lr = pos_maps_.back().leaf_range();
        pb_step_.cur_leaf1 = SecureRandom::rand_below(std::max(lr, 1));
        pb_step_.cur_leaf2 = SecureRandom::rand_below(std::max(lr, 1));
    } else {
        pb_step_.cur_leaf1 = SecureRandom::rand_below(std::max(leaf_range(), 1));
        pb_step_.cur_leaf2 = SecureRandom::rand_below(std::max(leaf_range(), 1));
    }
}

Bytes DAOram::piggyback_finish(const Bytes* new_value) {
    if (new_value && !pb_step_.is_dummy) {
        Block* target = find_in_stash(pb_step_.data_key);
        if (target) target->value = *new_value;
    }
    Bytes result = std::move(pb_step_.result);
    pb_step_.active = false;
    return result;
}

void DAOram::piggyback_finish_dummy() {
    pb_step_.active = false;
}

// ─── Store IDs ──────────────────────────────────────────────────────────────

int DAOram::get_store_id() const {
    return storage_ ? storage_->store_id() : -1;
}

int DAOram::get_pos_map_store_id(int lvl) const {
    if (lvl < 0 || lvl >= num_pos_levels_) return -1;
    return pos_maps_[lvl].get_store_id();
}

// ─── Split access (data ORAM level only, after local pos_map traversal) ─────

Bytes DAOram::access_without_eviction(int key) {
    last_bw_.reset();
    auto pm = traverse_pos_maps(key);

    {
        auto merged = storage_->read_multiple_paths({pm.leaf1, pm.leaf2});
        for (auto& [node, bucket] : merged)
            for (auto& block : bucket)
                if (!block.is_dummy()) stash_.push_back(block);
    }

    Block* target = find_in_stash(key);
    if (!target)
        throw std::runtime_error("DAOram::access_without_eviction: key " +
                                 std::to_string(key) + " not found");
    Bytes result = target->value;
    target->leaf = pm.new_leaf;

    if (pm.r_key >= 0) {
        Block* r_block = find_in_stash(pm.r_key);
        if (r_block) r_block->leaf = pm.r_new_leaf;
    }

    pending_leaves_ = {pm.leaf1, pm.leaf2};

    last_bw_.rounds = num_pos_levels_ + 1;
    int path_blocks = storage_->level() * bucket_size_;
    last_bw_.bytes_downloaded = 2 * path_blocks * (block_size_bytes_ + 8);
    last_bw_.bytes_uploaded = 0;
    total_bw_ += last_bw_;

    return result;
}

void DAOram::complete_eviction(int key, const Bytes& new_value) {
    last_bw_.reset();

    Block* target = find_in_stash(key);
    if (target) target->value = new_value;

    evict_and_write_paths(pending_leaves_);

    last_bw_.rounds = 1;
    int path_blocks = storage_->level() * bucket_size_;
    last_bw_.bytes_downloaded = 0;
    last_bw_.bytes_uploaded = 2 * path_blocks * (block_size_bytes_ + 8);
    total_bw_ += last_bw_;

    pending_leaves_.clear();
}

// ─── Low-level data ORAM operations ─────────────────────────────────────────

void DAOram::read_path_to_stash(int leaf) {
    auto path_data = storage_->read_path(leaf);
    for (auto& [node, bucket] : path_data)
        for (auto& block : bucket)
            if (!block.is_dummy()) stash_.push_back(std::move(block));
}

void DAOram::evict_and_write_path(int leaf) {
    evict_stash({leaf});
}

void DAOram::evict_and_write_paths(const std::vector<int>& leaves) {
    evict_stash(leaves);
}

void DAOram::evict_stash(const std::vector<int>& leaves) {
    auto indices = BinaryTreeStorage::get_merged_path_indices(
        storage_->level(), leaves);

    std::unordered_map<int, std::vector<Block>> path;
    for (int idx : indices) path[idx] = {};

    std::vector<Block> remaining;
    for (auto& block : stash_) {
        bool inserted = BinaryTreeStorage::fill_block_to_path(
            block, path, leaves, storage_->level(), bucket_size_);
        if (!inserted) remaining.push_back(std::move(block));
    }
    stash_ = std::move(remaining);

    if (static_cast<int>(stash_.size()) > stash_max_size_) {
        throw std::runtime_error(
            "DAOram: stash overflow (" + std::to_string(stash_.size()) +
            " > " + std::to_string(stash_max_size_) + ")");
    }

    storage_->write_multiple_paths(path);
}

Block* DAOram::find_in_stash(int key) {
    for (auto& block : stash_)
        if (block.key == key) return &block;
    return nullptr;
}

Block DAOram::extract_from_stash(int key) {
    for (auto it = stash_.begin(); it != stash_.end(); ++it) {
        if (it->key == key) {
            Block b = std::move(*it);
            stash_.erase(it);
            return b;
        }
    }
    throw std::runtime_error("DAOram::extract_from_stash: key " +
                             std::to_string(key) + " not found");
}

void DAOram::add_to_stash(Block block) {
    stash_.push_back(std::move(block));
}

int DAOram::get_leaf(int key) const {
    (void)key;
    return INVALID_LEAF;
}

void DAOram::set_leaf(int key, int /*leaf*/) {
    (void)key;
}

int DAOram::random_leaf() const {
    return SecureRandom::rand_below(std::max(leaf_range(), 1));
}

// ─── State export / import ─────────────────────────────────────────────────

Bytes DAOram::export_state(int store_id) const {
    Bytes buf;
    auto si = [&](int v) {
        size_t p = buf.size(); buf.resize(p + 4);
        std::memcpy(buf.data() + p, &v, 4);
    };
    auto sb = [&](const Bytes& b) {
        si(static_cast<int>(b.size()));
        buf.insert(buf.end(), b.begin(), b.end());
    };
    auto sblk = [&](const Block& b) { si(b.key); si(b.leaf); sb(b.value); };

    si(store_id);
    si(level());
    si(leaf_range());
    si(num_data_); si(bucket_size_); si(stash_max_size_);
    si(num_ic_); si(ic_max_); si(on_chip_mem_); si(block_size_bytes_);
    { uint64_t s = prf_seed_; buf.insert(buf.end(), (uint8_t*)&s, (uint8_t*)&s + 8); }

    si(num_pos_levels_);
    for (int ls : level_sizes_) si(ls);

    // On-chip counter blocks.
    si(static_cast<int>(on_chip_.size()));
    for (auto& cb : on_chip_) sb(cb.encode());

    // Pos-map PathORAM states.
    for (int i = 0; i < num_pos_levels_; ++i) {
        auto pm_blob = pos_maps_[i].export_state(-1);
        sb(pm_blob);
    }

    // Data ORAM stash.
    si(static_cast<int>(stash_.size()));
    for (auto& b : stash_) sblk(b);
    si(static_cast<int>(pending_leaves_.size()));
    for (int l : pending_leaves_) si(l);
    return buf;
}

DAOram DAOram::from_state_network(const uint8_t*& p,
                                  std::shared_ptr<TcpChannel> channel) {
    auto di = [&]() -> int { int v; std::memcpy(&v, p, 4); p += 4; return v; };
    auto db = [&]() -> Bytes { int n = di(); Bytes v(p, p + n); p += n; return v; };
    auto dblk = [&]() -> Block {
        int k = di(); int l = di(); Bytes v = db(); return {k, l, std::move(v)};
    };

    int sid = di();
    int lvl = di();
    int lr = di();

    DAOram o;
    o.num_data_ = di(); o.bucket_size_ = di(); o.stash_max_size_ = di();
    o.num_ic_ = di(); o.ic_max_ = di(); o.on_chip_mem_ = di();
    o.block_size_bytes_ = di();
    std::memcpy(&o.prf_seed_, p, 8); p += 8;

    o.num_pos_levels_ = di();
    o.level_sizes_.resize(o.num_pos_levels_);
    for (int i = 0; i < o.num_pos_levels_; ++i)
        o.level_sizes_[i] = di();

    int oc_sz = di();
    o.on_chip_.resize(oc_sz);
    for (int i = 0; i < oc_sz; ++i) {
        Bytes cb_bytes = db();
        o.on_chip_[i] = CounterBlock::decode(cb_bytes, o.num_ic_);
    }

    for (int i = 0; i < o.num_pos_levels_; ++i) {
        Bytes pm_blob = db();
        const uint8_t* pm_p = pm_blob.data();
        o.pos_maps_.push_back(PathORAM::from_state_network(pm_p, channel));
    }

    int ns = di();
    for (int i = 0; i < ns; ++i) o.stash_.push_back(dblk());
    int np = di();
    for (int i = 0; i < np; ++i) o.pending_leaves_.push_back(di());

    o.cached_level_ = lvl;
    o.cached_leaf_range_ = lr;
    o.storage_ = NetworkStorage::from_existing(
        std::move(channel), sid, lvl, lr, o.bucket_size_);
    return o;
}

}  // namespace tiered_omap
