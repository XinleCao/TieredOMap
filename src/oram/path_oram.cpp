#include "tiered_omap/oram/path_oram.h"
#include <algorithm>
#include <stdexcept>
#include <unordered_set>

namespace tiered_omap {

PathORAM::PathORAM(int num_data, int bucket_size, int stash_scale)
    : num_data_(num_data),
      bucket_size_(bucket_size),
      storage_(num_data, bucket_size),
      aes_key_(generate_aes_key()) {
    int lvl = storage_.level();
    stash_max_size_ = stash_scale * std::max(lvl - 1, 1);
}

void PathORAM::init(const std::unordered_map<int, Bytes>& data) {
    block_size_bytes_ = 0;
    stash_.clear();

    // Assign random leaves for all keys not yet in pos_map.
    for (auto& [key, value] : data) {
        if (pos_map_.find(key) == pos_map_.end())
            pos_map_[key] = SecureRandom::rand_below(storage_.leaf_range());
        if (static_cast<int>(value.size()) > block_size_bytes_)
            block_size_bytes_ = static_cast<int>(value.size());
    }

    storage_ = BinaryTreeStorage(num_data_, bucket_size_);
    for (auto& [key, value] : data) {
        Bytes enc_val = value.empty() ? value : aes_encrypt(aes_key_, value);
        Block block{key, pos_map_.at(key), enc_val};
        storage_.fill_data_to_leaf(block);
    }

    // Collect blocks that didn't fit into the tree.
    std::unordered_set<int> placed_keys;
    for (int leaf = 0; leaf < storage_.leaf_range(); ++leaf) {
        auto path = storage_.read_path(leaf);
        for (auto& [node, bucket] : path)
            for (auto& b : bucket)
                if (!b.is_dummy())
                    placed_keys.insert(b.key);
    }
    for (auto& [key, value] : data) {
        if (placed_keys.find(key) == placed_keys.end())
            stash_.push_back({key, pos_map_.at(key), value});
    }
}

Bytes PathORAM::access(int key, const Bytes* new_value) {
    last_bw_.reset();

    int old_leaf = pos_map_.at(key);
    int new_leaf = random_leaf();
    pos_map_[key] = new_leaf;

    // Read path.
    read_path_to_stash(old_leaf);

    // Find the target block in stash and retrieve/update.
    Bytes result;
    Block* target = find_in_stash(key);
    if (!target)
        throw std::runtime_error("PathORAM::access: key " +
                                 std::to_string(key) + " not found");
    result = target->value;
    if (new_value)
        target->value = *new_value;
    target->leaf = new_leaf;

    // Evict and write back.
    evict_and_write_path(old_leaf);
    inject_round_delay();

    last_bw_.rounds = 1;
    int path_blocks = storage_.level() * bucket_size_;
    last_bw_.bytes_downloaded = path_blocks * (block_size_bytes_ + 8);
    last_bw_.bytes_uploaded = last_bw_.bytes_downloaded;
    total_bw_ += last_bw_;

    return result;
}

void PathORAM::dummy_access() {
    int leaf = random_leaf();
    read_path_to_stash(leaf);
    evict_and_write_path(leaf);
    inject_round_delay();

    last_bw_.reset();
    last_bw_.rounds = 1;
    int path_blocks = storage_.level() * bucket_size_;
    last_bw_.bytes_downloaded = path_blocks * (block_size_bytes_ + 8);
    last_bw_.bytes_uploaded = last_bw_.bytes_downloaded;
    total_bw_ += last_bw_;
}

void PathORAM::encrypt_bucket(std::vector<Block>& bucket) {
    for (auto& block : bucket) {
        if (!block.is_dummy() && !block.value.empty())
            block.value = aes_encrypt(aes_key_, block.value);
    }
}

void PathORAM::decrypt_bucket(std::vector<Block>& bucket) {
    for (auto& block : bucket) {
        if (!block.is_dummy() && !block.value.empty())
            block.value = aes_decrypt(aes_key_, block.value);
    }
}

void PathORAM::read_path_to_stash(int leaf) {
    auto path_data = storage_.read_path(leaf);

    std::unordered_set<int> stash_ids;
    for (auto& b : stash_)
        if (!b.is_dummy()) stash_ids.insert(b.key);

    for (auto& [node, bucket] : path_data) {
        decrypt_bucket(bucket);
        for (auto& block : bucket) {
            if (!block.is_dummy() && stash_ids.find(block.key) == stash_ids.end()) {
                stash_ids.insert(block.key);
                stash_.push_back(std::move(block));
            }
        }
    }
}

void PathORAM::evict_and_write_path(int leaf) {
    evict_stash({leaf});
}

void PathORAM::evict_and_write_paths(const std::vector<int>& leaves) {
    evict_stash(leaves);
}

void PathORAM::evict_stash(const std::vector<int>& leaves) {
    auto indices = BinaryTreeStorage::get_merged_path_indices(
        storage_.level(), leaves);

    std::unordered_map<int, std::vector<Block>> path;
    for (int idx : indices)
        path[idx] = {};

    std::vector<Block> remaining;
    for (auto& block : stash_) {
        bool inserted = BinaryTreeStorage::fill_block_to_path(
            block, path, leaves, storage_.level(), bucket_size_);
        if (!inserted)
            remaining.push_back(std::move(block));
    }
    stash_ = std::move(remaining);

    if (static_cast<int>(stash_.size()) > stash_max_size_) {
        throw std::runtime_error(
            "PathORAM: stash overflow (" + std::to_string(stash_.size()) +
            " > " + std::to_string(stash_max_size_) + ")");
    }

    for (auto& [node, bucket] : path)
        encrypt_bucket(bucket);

    storage_.write_multiple_paths(path);
}

Block* PathORAM::find_in_stash(int key) {
    for (auto& block : stash_)
        if (block.key == key)
            return &block;
    return nullptr;
}

Block PathORAM::extract_from_stash(int key) {
    for (auto it = stash_.begin(); it != stash_.end(); ++it) {
        if (it->key == key) {
            Block b = std::move(*it);
            stash_.erase(it);
            return b;
        }
    }
    throw std::runtime_error("PathORAM::extract_from_stash: key " +
                             std::to_string(key) + " not found in stash");
}

void PathORAM::add_to_stash(Block block) {
    stash_.push_back(std::move(block));
}

int PathORAM::get_leaf(int key) const {
    auto it = pos_map_.find(key);
    if (it == pos_map_.end()) return INVALID_LEAF;
    return it->second;
}

void PathORAM::set_leaf(int key, int leaf) {
    pos_map_[key] = leaf;
}

int PathORAM::random_leaf() const {
    return SecureRandom::rand_below(storage_.leaf_range());
}

}  // namespace tiered_omap
