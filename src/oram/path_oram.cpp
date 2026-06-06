#include "tiered_omap/oram/path_oram.h"
#include "tiered_omap/network/network_storage.h"
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <unordered_set>

namespace tiered_omap {

PathORAM::PathORAM(int num_data, int bucket_size, int stash_scale,
                   StorageCreator storage_creator)
    : num_data_(num_data),
      bucket_size_(bucket_size),
      storage_creator_(std::move(storage_creator)),
      aes_key_(generate_aes_key()) {
    if (storage_creator_)
        storage_ = storage_creator_(num_data, bucket_size);
    else
        storage_ = std::make_unique<BinaryTreeStorage>(num_data, bucket_size);
    cached_level_ = storage_->level();
    cached_leaf_range_ = storage_->leaf_range();
    stash_max_size_ = stash_scale * std::max(cached_level_ - 1, 1);
}

void PathORAM::init(const std::unordered_map<int, Bytes>& data) {
    block_size_bytes_ = 0;
    stash_.clear();

    for (auto& [key, value] : data) {
        if (pos_map_.find(key) == pos_map_.end())
            pos_map_[key] = SecureRandom::rand_below(storage_->leaf_range());
        if (static_cast<int>(value.size()) > block_size_bytes_)
            block_size_bytes_ = static_cast<int>(value.size());
    }

    storage_->reset(num_data_, bucket_size_);

    std::vector<Block> blocks;
    blocks.reserve(data.size());
    for (auto& [key, value] : data) {
        Bytes enc_val = value.empty() ? value : aes_encrypt(aes_key_, value);
        blocks.push_back({key, pos_map_.at(key), enc_val});
    }

    auto placed_keys = storage_->bulk_load(blocks);

    for (auto& [key, value] : data) {
        if (placed_keys.find(key) == placed_keys.end())
            stash_.push_back({key, pos_map_.at(key), value});
    }
}

void PathORAM::init_sequential(int count, int value_size) {
    block_size_bytes_ = value_size;
    stash_.clear();
    pos_map_.clear();
    pos_map_.reserve(static_cast<size_t>(count));
    storage_->reset(num_data_, bucket_size_);

    auto make_plain = [value_size](int key) {
        Bytes value(static_cast<size_t>(value_size), 0);
        if (value_size > 0) {
            std::memcpy(value.data(), &key,
                        std::min(sizeof(int), static_cast<size_t>(value_size)));
        }
        return value;
    };

    storage_->bulk_load_generated(
        count,
        static_cast<size_t>(value_size + AES_IV_LEN),
        [&](int key) {
            int leaf = random_leaf();
            pos_map_[key] = leaf;
            Bytes value = make_plain(key);
            Bytes enc_val = value.empty() ? value : aes_encrypt(aes_key_, value);
            return Block{key, leaf, std::move(enc_val)};
        },
        [&](int key) {
            auto it = pos_map_.find(key);
            int leaf = (it == pos_map_.end()) ? random_leaf() : it->second;
            stash_.push_back(Block{key, leaf, make_plain(key)});
        });
}

Bytes PathORAM::access(int key, const Bytes* new_value) {
    last_bw_.reset();

    int old_leaf = pos_map_.at(key);
    int new_leaf = random_leaf();
    pos_map_[key] = new_leaf;

    read_path_to_stash(old_leaf);

    Bytes result;
    Block* target = find_in_stash(key);
    if (!target)
        throw std::runtime_error("PathORAM::access: key " +
                                 std::to_string(key) + " not found");
    result = target->value;
    if (new_value)
        target->value = *new_value;
    target->leaf = new_leaf;

    evict_and_write_path(old_leaf);
    inject_round_delay();

    last_bw_.rounds = 1;
    int path_blocks = storage_->level() * bucket_size_;
    last_bw_.bytes_downloaded = path_blocks * (block_size_bytes_ + 8);
    last_bw_.bytes_uploaded = last_bw_.bytes_downloaded;
    total_bw_ += last_bw_;

    return result;
}

Bytes PathORAM::access_update(
    int key, const std::function<Bytes(const Bytes&)>& update) {
    last_bw_.reset();

    int old_leaf = pos_map_.at(key);
    int new_leaf = random_leaf();
    pos_map_[key] = new_leaf;

    read_path_to_stash(old_leaf);

    Bytes result;
    Block* target = find_in_stash(key);
    if (!target)
        throw std::runtime_error("PathORAM::access_update: key " +
                                 std::to_string(key) + " not found");
    result = target->value;
    if (update)
        target->value = update(result);
    target->leaf = new_leaf;

    evict_and_write_path(old_leaf);
    inject_round_delay();

    last_bw_.rounds = 1;
    int path_blocks = storage_->level() * bucket_size_;
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
    int path_blocks = storage_->level() * bucket_size_;
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
    auto path_data = storage_->read_path(leaf);
    apply_fetched_path(std::move(path_data));
}

void PathORAM::apply_fetched_path(
    std::unordered_map<int, std::vector<Block>> data) {
    for (auto& [node, bucket] : data) {
        decrypt_bucket(bucket);
        for (auto& block : bucket) {
            if (block.is_dummy())
                continue;
            Block* existing = find_in_stash(block.key);
            if (!existing) {
                stash_.push_back(std::move(block));
                continue;
            }
            auto it = pos_map_.find(block.key);
            bool incoming_current =
                (it == pos_map_.end()) || (it->second == block.leaf);
            bool existing_current =
                (it == pos_map_.end()) || (it->second == existing->leaf);
            if (incoming_current && !existing_current)
                *existing = std::move(block);
        }
    }
}

std::unordered_map<int, std::vector<Block>>
PathORAM::prepare_eviction(int leaf) {
    std::vector<int> leaves = {leaf};
    auto indices = BinaryTreeStorage::get_merged_path_indices(
        storage_->level(), leaves);

    std::unordered_map<int, std::vector<Block>> path;
    for (int idx : indices)
        path[idx] = {};

    std::vector<Block> remaining;
    for (auto& block : stash_) {
        bool inserted = BinaryTreeStorage::fill_block_to_path(
            block, path, leaves, storage_->level(), bucket_size_);
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
    return path;
}

void PathORAM::read_multiple_paths_to_stash(const std::vector<int>& leaves) {
    auto path_data = storage_->read_multiple_paths(leaves);
    apply_fetched_path(std::move(path_data));
}

std::unordered_map<int, std::vector<Block>>
PathORAM::prepare_eviction_paths(const std::vector<int>& leaves) {
    auto indices = BinaryTreeStorage::get_merged_path_indices(
        storage_->level(), leaves);

    std::unordered_map<int, std::vector<Block>> path;
    for (int idx : indices)
        path[idx] = {};

    std::vector<Block> remaining;
    for (auto& block : stash_) {
        bool inserted = BinaryTreeStorage::fill_block_to_path(
            block, path, leaves, storage_->level(), bucket_size_);
        if (!inserted)
            remaining.push_back(std::move(block));
    }
    stash_ = std::move(remaining);

    if (static_cast<int>(stash_.size()) > stash_max_size_) {
        throw std::runtime_error(
            "PathORAM: stash overflow (" + std::to_string(stash_.size()) +
            " > " + std::to_string(stash_max_size_) + ")");
    }

    for (auto& [node, bucket] : path) {
        while (static_cast<int>(bucket.size()) < bucket_size_)
            bucket.push_back(Block{});
        encrypt_bucket(bucket);
    }
    return path;
}

int PathORAM::get_store_id() const {
    return storage_ ? storage_->store_id() : -1;
}

void PathORAM::evict_and_write_path(int leaf) {
    evict_stash({leaf});
}

void PathORAM::evict_and_write_paths(const std::vector<int>& leaves) {
    evict_stash(leaves);
}

void PathORAM::evict_stash(const std::vector<int>& leaves) {
    auto indices = BinaryTreeStorage::get_merged_path_indices(
        storage_->level(), leaves);

    std::unordered_map<int, std::vector<Block>> path;
    for (int idx : indices)
        path[idx] = {};

    std::vector<Block> remaining;
    for (auto& block : stash_) {
        bool inserted = BinaryTreeStorage::fill_block_to_path(
            block, path, leaves, storage_->level(), bucket_size_);
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

    storage_->write_multiple_paths(path);
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
    std::string diag = "PathORAM::extract_from_stash: key " +
                        std::to_string(key) + " not found. stash=[";
    for (auto& b : stash_)
        diag += std::to_string(b.key) + "(leaf=" +
                std::to_string(b.leaf) + " val_sz=" +
                std::to_string(b.value.size()) + ") ";
    diag += "]";
    auto pm_it = pos_map_.find(key);
    if (pm_it != pos_map_.end()) {
        diag += " pos_map_leaf=" + std::to_string(pm_it->second);
        auto path = storage_->read_path(pm_it->second);
        bool on_path = false;
        for (auto& [node, bucket] : path)
            for (auto& b : bucket)
                if (b.key == key) {
                    on_path = true;
                    diag += " tree_copy(leaf=" + std::to_string(b.leaf) +
                            " val_sz=" + std::to_string(b.value.size()) + ")";
                }
        diag += on_path ? " DEDUP_BUG" : " NOT_ON_PATH";
    } else {
        diag += " NOT_IN_POSMAP";
    }
    throw std::runtime_error(diag);
}

void PathORAM::add_to_stash(Block block) {
    for (auto it = stash_.begin(); it != stash_.end();) {
        if (it->key == block.key)
            it = stash_.erase(it);
        else
            ++it;
    }
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
    return SecureRandom::rand_below(storage_->leaf_range());
}

Bytes PathORAM::export_state(int store_id) const {
    Bytes buf;
    auto si = [&](int v) { size_t p=buf.size(); buf.resize(p+4); std::memcpy(buf.data()+p,&v,4); };
    auto sb = [&](const Bytes& b) { si(static_cast<int>(b.size())); buf.insert(buf.end(),b.begin(),b.end()); };
    auto sblk = [&](const Block& b) { si(b.key); si(b.leaf); sb(b.value); };
    si(store_id);
    si(level());
    si(leaf_range());
    si(num_data_); si(bucket_size_); si(stash_max_size_); si(block_size_bytes_);
    sb(aes_key_);
    si(static_cast<int>(pos_map_.size()));
    for (auto& [k,v] : pos_map_) { si(k); si(v); }
    si(static_cast<int>(stash_.size()));
    for (auto& b : stash_) sblk(b);
    return buf;
}

PathORAM PathORAM::from_state_network(const uint8_t*& p,
                                      std::shared_ptr<TcpChannel> channel) {
    auto di = [&]() -> int { int v; std::memcpy(&v,p,4); p+=4; return v; };
    auto db = [&]() -> Bytes { int n=di(); Bytes v(p,p+n); p+=n; return v; };
    auto dblk = [&]() -> Block { int k=di(); int l=di(); Bytes v=db(); return {k,l,std::move(v)}; };
    int store_id = di();
    int level = di();
    int leaf_range = di();
    PathORAM o;
    o.num_data_ = di(); o.bucket_size_ = di(); o.stash_max_size_ = di(); o.block_size_bytes_ = di();
    o.aes_key_ = db();
    int pm_sz = di();
    for (int i = 0; i < pm_sz; ++i) { int k=di(); int v=di(); o.pos_map_[k]=v; }
    int st_sz = di();
    for (int i = 0; i < st_sz; ++i) o.stash_.push_back(dblk());
    o.cached_level_ = level;
    o.cached_leaf_range_ = leaf_range;
    o.storage_ = NetworkStorage::from_existing(
        std::move(channel), store_id, level, leaf_range, o.bucket_size_);
    return o;
}

}  // namespace tiered_omap
