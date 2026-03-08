#include "tiered_omap/oram/da_oram.h"
#include <algorithm>
#include <functional>
#include <stdexcept>
#include <unordered_set>

namespace tiered_omap {

DAOram::DAOram(int num_data, int bucket_size, int stash_scale,
               int num_ic, int ic_max, StorageCreator storage_creator)
    : num_data_(num_data),
      bucket_size_(bucket_size),
      num_ic_(num_ic),
      ic_max_(ic_max),
      storage_creator_(std::move(storage_creator)) {
    if (storage_creator_)
        storage_ = storage_creator_(num_data, bucket_size);
    else
        storage_ = std::make_unique<BinaryTreeStorage>(num_data, bucket_size);
    int lvl = storage_->level();
    stash_max_size_ = stash_scale * std::max(lvl - 1, 1);

    int num_groups = (num_data + num_ic - 1) / num_ic;
    counters_.resize(num_groups, CounterBlock(num_ic));

    prf_seed_ = static_cast<uint64_t>(std::random_device{}());
}

int DAOram::prf_leaf(int data_key, uint64_t gc, uint8_t ic) const {
    std::hash<uint64_t> h;
    uint64_t v = prf_seed_;
    v ^= h(static_cast<uint64_t>(data_key)) + 0x9e3779b9 + (v << 6) + (v >> 2);
    v ^= h(gc) + 0x9e3779b9 + (v << 6) + (v >> 2);
    v ^= h(static_cast<uint64_t>(ic)) + 0x9e3779b9 + (v << 6) + (v >> 2);
    return static_cast<int>(v % static_cast<uint64_t>(storage_->leaf_range()));
}

// ─── Init ───────────────────────────────────────────────────────────────────

void DAOram::init(const std::unordered_map<int, Bytes>& data) {
    block_size_bytes_ = 0;
    stash_.clear();

    for (auto& [key, value] : data) {
        if (static_cast<int>(value.size()) > block_size_bytes_)
            block_size_bytes_ = static_cast<int>(value.size());
    }

    storage_->reset(num_data_, bucket_size_);

    std::vector<Block> blocks;
    blocks.reserve(data.size());
    for (auto& [key, value] : data) {
        int leaf = prf_leaf(key, 0, 0);
        blocks.push_back({key, leaf, value});
    }

    auto placed = storage_->bulk_load(blocks);

    for (auto& [key, value] : data) {
        if (placed.find(key) == placed.end()) {
            int leaf = prf_leaf(key, 0, 0);
            stash_.push_back({key, leaf, value});
        }
    }
}

// ─── Counter management ─────────────────────────────────────────────────────

std::pair<int, int> DAOram::update_counter(int data_key) {
    int group = data_key / num_ic_;
    int offset = data_key % num_ic_;
    auto& cb = counters_[group];

    uint64_t gc = cb.gc;
    uint8_t ic = cb.ic[offset];

    uint64_t next_gc;
    uint8_t next_ic;

    if (cb.backup[offset] == 1) {
        next_ic = 0;
        next_gc = gc;
        gc = gc - 1;
        cb.backup[offset] = 0;
    } else if (ic + 1 >= ic_max_) {
        next_ic = 0;
        next_gc = gc + 1;
        cb.gc = next_gc;
        for (int i = 0; i < num_ic_; ++i) {
            if (i != offset) cb.backup[i] = 1;
        }
    } else {
        next_ic = ic + 1;
        next_gc = gc;
    }

    cb.ic[offset] = next_ic;

    int cur_leaf = prf_leaf(data_key, gc, ic);
    int new_leaf = prf_leaf(data_key, next_gc, next_ic);

    return {cur_leaf, new_leaf};
}

std::tuple<int, int, int> DAOram::perform_reset(int group_key) {
    auto& cb = counters_[group_key];

    for (int i = 0; i < num_ic_; ++i) {
        if (cb.backup[i] == 1) {
            int data_key = group_key * num_ic_ + i;
            if (data_key >= num_data_) continue;

            uint64_t gc = cb.gc;
            uint8_t ic = cb.ic[i];

            uint64_t old_gc = gc - 1;
            int cur_leaf = prf_leaf(data_key, old_gc, ic);
            int new_leaf = prf_leaf(data_key, gc, 0);

            cb.ic[i] = 0;
            cb.backup[i] = 0;

            return {data_key, cur_leaf, new_leaf};
        }
    }

    return {-1, SecureRandom::rand_below(storage_->leaf_range()), -1};
}

// ─── Access ─────────────────────────────────────────────────────────────────

Bytes DAOram::access(int key, const Bytes* new_value) {
    last_bw_.reset();

    auto [cur_leaf, new_leaf] = update_counter(key);

    int group = key / num_ic_;
    auto [r_key, r_cur_leaf, r_new_leaf] = perform_reset(group);

    {
        auto merged = storage_->read_multiple_paths({cur_leaf, r_cur_leaf});
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
    target->leaf = new_leaf;

    if (r_key >= 0) {
        Block* r_block = find_in_stash(r_key);
        if (r_block) r_block->leaf = r_new_leaf;
    }

    evict_and_write_paths({cur_leaf, r_cur_leaf});

    last_bw_.rounds = 1;
    int path_blocks = storage_->level() * bucket_size_;
    last_bw_.bytes_downloaded = 2 * path_blocks * (block_size_bytes_ + 8);
    last_bw_.bytes_uploaded = last_bw_.bytes_downloaded;
    total_bw_ += last_bw_;

    return result;
}

void DAOram::dummy_access() {
    int leaf1 = SecureRandom::rand_below(storage_->leaf_range());
    int leaf2 = SecureRandom::rand_below(storage_->leaf_range());

    {
        auto merged = storage_->read_multiple_paths({leaf1, leaf2});
        for (auto& [node, bucket] : merged)
            for (auto& block : bucket)
                if (!block.is_dummy()) stash_.push_back(block);
    }
    evict_and_write_paths({leaf1, leaf2});

    last_bw_.reset();
    last_bw_.rounds = 1;
    int path_blocks = storage_->level() * bucket_size_;
    last_bw_.bytes_downloaded = 2 * path_blocks * (block_size_bytes_ + 8);
    last_bw_.bytes_uploaded = last_bw_.bytes_downloaded;
    total_bw_ += last_bw_;
}

// ─── Split access ───────────────────────────────────────────────────────────

Bytes DAOram::access_without_eviction(int key) {
    last_bw_.reset();

    auto [cur_leaf, new_leaf] = update_counter(key);
    int group = key / num_ic_;
    auto [r_key, r_cur_leaf, r_new_leaf] = perform_reset(group);

    {
        auto merged = storage_->read_multiple_paths({cur_leaf, r_cur_leaf});
        for (auto& [node, bucket] : merged)
            for (auto& block : bucket)
                if (!block.is_dummy()) stash_.push_back(block);
    }

    Block* target = find_in_stash(key);
    if (!target)
        throw std::runtime_error("DAOram::access_without_eviction: key " +
                                 std::to_string(key) + " not found");
    Bytes result = target->value;
    target->leaf = new_leaf;

    if (r_key >= 0) {
        Block* r_block = find_in_stash(r_key);
        if (r_block) r_block->leaf = r_new_leaf;
    }

    pending_leaves_ = {cur_leaf, r_cur_leaf};

    last_bw_.rounds = 1;
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

// ─── Low-level ORAM operations ──────────────────────────────────────────────

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
    if (key < 0 || key >= num_data_) return INVALID_LEAF;
    int group = key / num_ic_;
    int offset = key % num_ic_;
    return prf_leaf(key, counters_[group].gc, counters_[group].ic[offset]);
}

void DAOram::set_leaf(int key, int /*leaf*/) {
    (void)key;
}

int DAOram::random_leaf() const {
    return SecureRandom::rand_below(storage_->leaf_range());
}

}  // namespace tiered_omap
