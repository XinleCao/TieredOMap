#include "tiered_omap/oram/disk_binary_tree_storage.h"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <fstream>
#include <ios>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace tiered_omap {

namespace {

std::atomic<uint64_t> g_disk_store_counter{0};

void write_i32(std::ostream& out, int v) {
    out.write(reinterpret_cast<const char*>(&v), sizeof(v));
    if (!out) throw std::runtime_error("DiskBinaryTreeStorage write failed");
}

int read_i32(std::istream& in) {
    int v = 0;
    in.read(reinterpret_cast<char*>(&v), sizeof(v));
    if (!in) throw std::runtime_error("DiskBinaryTreeStorage read failed");
    return v;
}

void write_u64(std::ostream& out, uint64_t v) {
    out.write(reinterpret_cast<const char*>(&v), sizeof(v));
    if (!out) throw std::runtime_error("DiskBinaryTreeStorage write failed");
}

uint64_t read_u64(std::istream& in) {
    uint64_t v = 0;
    in.read(reinterpret_cast<char*>(&v), sizeof(v));
    if (!in) throw std::runtime_error("DiskBinaryTreeStorage read failed");
    return v;
}

class FdGuard {
public:
    explicit FdGuard(int fd) : fd_(fd) {}
    ~FdGuard() {
        if (fd_ >= 0)
            ::close(fd_);
    }
    FdGuard(const FdGuard&) = delete;
    FdGuard& operator=(const FdGuard&) = delete;
    int get() const { return fd_; }

private:
    int fd_;
};

void throw_errno(const std::string& prefix) {
    throw std::runtime_error(prefix + ": " + std::strerror(errno));
}

void write_all(int fd, const char* data, size_t len) {
    while (len > 0) {
        ssize_t n = ::write(fd, data, len);
        if (n < 0)
            throw_errno("DiskBinaryTreeStorage sparse copy write failed");
        data += n;
        len -= static_cast<size_t>(n);
    }
}

void copy_range_exact(int in_fd, int out_fd, off_t start, off_t end) {
    if (::lseek(in_fd, start, SEEK_SET) < 0)
        throw_errno("DiskBinaryTreeStorage sparse copy input seek failed");
    if (::lseek(out_fd, start, SEEK_SET) < 0)
        throw_errno("DiskBinaryTreeStorage sparse copy output seek failed");

    std::vector<char> buffer(1 << 20);
    off_t remaining = end - start;
    while (remaining > 0) {
        size_t want = static_cast<size_t>(
            std::min<off_t>(remaining, static_cast<off_t>(buffer.size())));
        ssize_t n = ::read(in_fd, buffer.data(), want);
        if (n < 0)
            throw_errno("DiskBinaryTreeStorage sparse copy read failed");
        if (n == 0)
            throw std::runtime_error(
                "DiskBinaryTreeStorage sparse copy unexpected EOF");
        write_all(out_fd, buffer.data(), static_cast<size_t>(n));
        remaining -= n;
    }
}

bool is_all_zero(const std::vector<char>& buffer, size_t len) {
    return std::all_of(buffer.begin(), buffer.begin() + len,
                       [](char c) { return c == 0; });
}

void copy_sparse_fallback(int in_fd, int out_fd, uint64_t size) {
    if (::lseek(in_fd, 0, SEEK_SET) < 0)
        throw_errno("DiskBinaryTreeStorage fallback input seek failed");

    std::vector<char> buffer(1 << 20);
    uint64_t offset = 0;
    while (offset < size) {
        size_t want = static_cast<size_t>(
            std::min<uint64_t>(size - offset, buffer.size()));
        ssize_t n = ::read(in_fd, buffer.data(), want);
        if (n < 0)
            throw_errno("DiskBinaryTreeStorage fallback read failed");
        if (n == 0)
            break;
        if (!is_all_zero(buffer, static_cast<size_t>(n))) {
            if (::lseek(out_fd, static_cast<off_t>(offset), SEEK_SET) < 0)
                throw_errno("DiskBinaryTreeStorage fallback output seek failed");
            write_all(out_fd, buffer.data(), static_cast<size_t>(n));
        }
        offset += static_cast<uint64_t>(n);
    }
}

void copy_sparse_file(const std::string& src, const std::string& dst) {
    uint64_t size = static_cast<uint64_t>(std::filesystem::file_size(src));
    FdGuard in_fd(::open(src.c_str(), O_RDONLY));
    if (in_fd.get() < 0)
        throw_errno("DiskBinaryTreeStorage sparse copy source open failed");
    FdGuard out_fd(::open(dst.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644));
    if (out_fd.get() < 0)
        throw_errno("DiskBinaryTreeStorage sparse copy destination open failed");

    bool copied_with_extents = false;
#ifdef SEEK_DATA
    bool extent_mode_ok = true;
    off_t pos = 0;
    while (static_cast<uint64_t>(pos) < size) {
        errno = 0;
        off_t data = ::lseek(in_fd.get(), pos, SEEK_DATA);
        if (data < 0) {
            if (errno == ENXIO) {
                copied_with_extents = true;
                break;
            }
            extent_mode_ok = false;
            break;
        }

        errno = 0;
        off_t hole = ::lseek(in_fd.get(), data, SEEK_HOLE);
        if (hole < 0) {
            if (errno == ENXIO)
                hole = static_cast<off_t>(size);
            else {
                extent_mode_ok = false;
                break;
            }
        }
        if (static_cast<uint64_t>(hole) > size)
            hole = static_cast<off_t>(size);
        copy_range_exact(in_fd.get(), out_fd.get(), data, hole);
        pos = hole;
        copied_with_extents = true;
    }
    if (!extent_mode_ok)
        copied_with_extents = false;
#endif

    if (!copied_with_extents)
        copy_sparse_fallback(in_fd.get(), out_fd.get(), size);

    if (::ftruncate(out_fd.get(), static_cast<off_t>(size)) < 0)
        throw_errno("DiskBinaryTreeStorage sparse copy truncate failed");
}

}  // namespace

DiskBinaryTreeStorage::DiskBinaryTreeStorage(int num_data, int bucket_size,
                                             std::string directory)
    : directory_(std::move(directory)) {
    std::filesystem::create_directories(directory_);
    file_path_ = make_file_path(directory_);
    reset(num_data, bucket_size);
}

DiskBinaryTreeStorage::~DiskBinaryTreeStorage() {
    if (remove_on_destroy_ && !file_path_.empty()) {
        std::error_code ec;
        std::filesystem::remove(file_path_, ec);
    }
}

std::string DiskBinaryTreeStorage::make_file_path(
    const std::string& directory) {
    std::filesystem::create_directories(directory);
    auto id = g_disk_store_counter.fetch_add(1);
    std::filesystem::path p(directory);
    p /= "runtime-store-" + std::to_string(::getpid()) + "-"
       + std::to_string(id) + ".bin";
    return p.string();
}

uint64_t DiskBinaryTreeStorage::slot_size() const {
    return 2 * sizeof(int) + sizeof(uint64_t) + slot_value_size_;
}

uint64_t DiskBinaryTreeStorage::node_size() const {
    return sizeof(int) + static_cast<uint64_t>(bucket_size_) * slot_size();
}

uint64_t DiskBinaryTreeStorage::node_offset(int node) const {
    return static_cast<uint64_t>(node) * node_size();
}

uint64_t DiskBinaryTreeStorage::slot_offset(int node, int slot) const {
    return node_offset(node) + sizeof(int)
         + static_cast<uint64_t>(slot) * slot_size();
}

void DiskBinaryTreeStorage::create_empty_file() {
    std::ofstream out(file_path_, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("DiskBinaryTreeStorage open failed");
}

void DiskBinaryTreeStorage::reset(int num_data, int bucket_size) {
    std::lock_guard<std::mutex> lock(mutex_);
    bucket_size_ = bucket_size;
    level_ = ceil_log2(num_data) + 1;
    leaf_range_ = 1 << (level_ - 1);
    total_nodes_ = (1 << level_) - 1;
    base_file_path_.clear();
    dirty_nodes_.clear();
    create_empty_file();
}

std::vector<Block> DiskBinaryTreeStorage::read_bucket(int node) const {
    if (!base_file_path_.empty() && dirty_nodes_.count(node) == 0)
        return read_bucket_from_file(base_file_path_, node);
    return read_bucket_from_file(file_path_, node);
}

std::vector<Block>
DiskBinaryTreeStorage::read_bucket_from_file(const std::string& path,
                                             int node) const {
    if (node < 0 || node >= total_nodes_)
        throw std::runtime_error("DiskBinaryTreeStorage::read_bucket: node out of range");

    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("DiskBinaryTreeStorage read open failed");
    in.seekg(static_cast<std::streamoff>(node_offset(node)));
    int count = 0;
    in.read(reinterpret_cast<char*>(&count), sizeof(count));
    if (!in) return {};
    if (count < 0 || count > bucket_size_)
        throw std::runtime_error("DiskBinaryTreeStorage invalid bucket count");

    std::vector<Block> bucket;
    bucket.reserve(count);
    for (int i = 0; i < count; ++i)
        bucket.push_back(read_block_record(in));
    return bucket;
}

void DiskBinaryTreeStorage::write_bucket(
    int node, const std::vector<Block>& bucket) {
    if (node < 0 || node >= total_nodes_)
        throw std::runtime_error("DiskBinaryTreeStorage::write_bucket: node out of range");
    if (static_cast<int>(bucket.size()) > bucket_size_)
        throw std::runtime_error("DiskBinaryTreeStorage::write_bucket: bucket too large");

    std::fstream io(file_path_, std::ios::binary | std::ios::in | std::ios::out);
    if (!io) throw std::runtime_error("DiskBinaryTreeStorage write open failed");
    io.seekp(static_cast<std::streamoff>(node_offset(node)));
    write_i32(io, static_cast<int>(bucket.size()));
    for (const auto& block : bucket)
        write_block_record(io, block);
    dirty_nodes_.insert(node);
}

void DiskBinaryTreeStorage::write_slot(
    int node, int slot, const Block& block, int new_count) {
    std::fstream io(file_path_, std::ios::binary | std::ios::in | std::ios::out);
    if (!io) throw std::runtime_error("DiskBinaryTreeStorage slot write open failed");
    write_slot_to(io, node, slot, block, new_count);
    dirty_nodes_.insert(node);
}

void DiskBinaryTreeStorage::write_slot_to(
    std::ostream& out, int node, int slot, const Block& block,
    int new_count) const {
    out.seekp(static_cast<std::streamoff>(node_offset(node)));
    write_i32(out, new_count);
    out.seekp(static_cast<std::streamoff>(slot_offset(node, slot)));
    write_block_record(out, block);
}

void DiskBinaryTreeStorage::write_block_record(
    std::ostream& out, const Block& block) const {
    if (block.value.size() > slot_value_size_)
        throw std::runtime_error("DiskBinaryTreeStorage block exceeds slot size");
    write_i32(out, block.key);
    write_i32(out, block.leaf);
    write_u64(out, static_cast<uint64_t>(block.value.size()));
    if (!block.value.empty()) {
        out.write(reinterpret_cast<const char*>(block.value.data()),
                  static_cast<std::streamsize>(block.value.size()));
        if (!out) throw std::runtime_error("DiskBinaryTreeStorage value write failed");
    }
    size_t padding = slot_value_size_ - block.value.size();
    static const char zeros[4096] = {};
    while (padding > 0) {
        size_t chunk = std::min(padding, sizeof(zeros));
        out.write(zeros, static_cast<std::streamsize>(chunk));
        if (!out) throw std::runtime_error("DiskBinaryTreeStorage padding write failed");
        padding -= chunk;
    }
}

Block DiskBinaryTreeStorage::read_block_record(std::istream& in) const {
    Block block;
    block.key = read_i32(in);
    block.leaf = read_i32(in);
    uint64_t value_size = read_u64(in);
    if (value_size > slot_value_size_)
        throw std::runtime_error("DiskBinaryTreeStorage invalid value size");
    block.value.resize(static_cast<size_t>(value_size));
    if (!block.value.empty()) {
        in.read(reinterpret_cast<char*>(block.value.data()),
                static_cast<std::streamsize>(block.value.size()));
        if (!in) throw std::runtime_error("DiskBinaryTreeStorage value read failed");
    }
    size_t padding = slot_value_size_ - block.value.size();
    in.seekg(static_cast<std::streamoff>(padding), std::ios::cur);
    if (!in) throw std::runtime_error("DiskBinaryTreeStorage padding skip failed");
    return block;
}

void DiskBinaryTreeStorage::fill_data_to_leaf(const Block& block) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (block.value.size() > slot_value_size_) {
        if (slot_value_size_ == 0)
            slot_value_size_ = block.value.size();
        else
            throw std::runtime_error("DiskBinaryTreeStorage fill exceeds slot size");
    }
    int node = leaf_to_node(block.leaf);
    std::vector<int> path;
    while (node >= 0) {
        path.push_back(node);
        if (node == 0) break;
        node = parent(node);
    }
    for (int n : path) {
        auto bucket = read_bucket(n);
        if (static_cast<int>(bucket.size()) < bucket_size_) {
            bucket.push_back(block);
            write_bucket(n, bucket);
            return;
        }
    }
}

std::unordered_set<int>
DiskBinaryTreeStorage::bulk_load(const std::vector<Block>& blocks) {
    std::lock_guard<std::mutex> lock(mutex_);
    slot_value_size_ = 0;
    for (const auto& block : blocks)
        slot_value_size_ = std::max(slot_value_size_, block.value.size());
    create_empty_file();

    std::vector<uint8_t> counts(static_cast<size_t>(total_nodes_), 0);
    std::unordered_set<int> placed;
    placed.reserve(blocks.size());
    std::fstream io(file_path_, std::ios::binary | std::ios::in | std::ios::out);
    if (!io) throw std::runtime_error("DiskBinaryTreeStorage bulk open failed");

    std::vector<int> path;
    for (const auto& block : blocks) {
        path.clear();
        int node = leaf_to_node(block.leaf);
        while (node >= 0) {
            path.push_back(node);
            if (node == 0) break;
            node = parent(node);
        }
        for (int n : path) {
            uint8_t& count = counts[static_cast<size_t>(n)];
            if (count < bucket_size_) {
                write_slot_to(io, n, count, block, count + 1);
                ++count;
                if (!block.is_dummy())
                    placed.insert(block.key);
                break;
            }
        }
    }
    return placed;
}

void DiskBinaryTreeStorage::bulk_load_generated(
    int count, size_t max_value_size,
    const std::function<Block(int)>& make_block,
    const std::function<void(int)>& on_overflow) {
    std::lock_guard<std::mutex> lock(mutex_);
    slot_value_size_ = max_value_size;
    create_empty_file();

    std::vector<uint8_t> counts(static_cast<size_t>(total_nodes_), 0);
    std::fstream io(file_path_, std::ios::binary | std::ios::in | std::ios::out);
    if (!io) throw std::runtime_error("DiskBinaryTreeStorage bulk open failed");

    std::vector<int> path;
    for (int i = 0; i < count; ++i) {
        Block block = make_block(i);
        if (block.value.size() > slot_value_size_)
            throw std::runtime_error("DiskBinaryTreeStorage generated block exceeds slot size");

        path.clear();
        int node = leaf_to_node(block.leaf);
        while (node >= 0) {
            path.push_back(node);
            if (node == 0) break;
            node = parent(node);
        }

        bool placed = false;
        for (int n : path) {
            uint8_t& bucket_count = counts[static_cast<size_t>(n)];
            if (bucket_count < bucket_size_) {
                write_slot_to(io, n, bucket_count, block, bucket_count + 1);
                ++bucket_count;
                placed = true;
                break;
            }
        }
        if (!placed && on_overflow)
            on_overflow(i);
    }
}

std::unordered_map<int, std::vector<Block>>
DiskBinaryTreeStorage::read_path(int leaf) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::unordered_map<int, std::vector<Block>> result;
    int node = leaf_to_node(leaf);
    std::vector<int> path;
    while (node >= 0) {
        path.push_back(node);
        if (node == 0) break;
        node = parent(node);
    }
    std::reverse(path.begin(), path.end());
    for (int n : path)
        result[n] = read_bucket(n);
    return result;
}

void DiskBinaryTreeStorage::write_path(
    int /*leaf*/, const std::unordered_map<int, std::vector<Block>>& buckets) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [node, bucket] : buckets)
        write_bucket(node, bucket);
}

std::unordered_map<int, std::vector<Block>>
DiskBinaryTreeStorage::read_multiple_paths(const std::vector<int>& leaves) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto indices = BinaryTreeStorage::get_merged_path_indices(level_, leaves);
    std::unordered_map<int, std::vector<Block>> result;
    for (int n : indices)
        result[n] = read_bucket(n);
    return result;
}

void DiskBinaryTreeStorage::write_multiple_paths(
    const std::unordered_map<int, std::vector<Block>>& buckets) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [node, bucket] : buckets)
        write_bucket(node, bucket);
}

void DiskBinaryTreeStorage::write_state(std::ostream& out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    write_i32(out, level_);
    write_i32(out, leaf_range_);
    write_i32(out, bucket_size_);
    write_i32(out, total_nodes_);
    write_u64(out, static_cast<uint64_t>(total_nodes_));
    for (int node = 0; node < total_nodes_; ++node) {
        auto bucket = read_bucket(node);
        write_u64(out, static_cast<uint64_t>(bucket.size()));
        for (const auto& block : bucket) {
            write_i32(out, block.key);
            write_i32(out, block.leaf);
            write_u64(out, static_cast<uint64_t>(block.value.size()));
            if (!block.value.empty()) {
                out.write(reinterpret_cast<const char*>(block.value.data()),
                          static_cast<std::streamsize>(block.value.size()));
                if (!out) throw std::runtime_error("DiskBinaryTreeStorage state write failed");
            }
        }
    }
}

std::unique_ptr<DiskBinaryTreeStorage>
DiskBinaryTreeStorage::read_state(std::istream& in,
                                  const std::string& directory) {
    auto start = in.tellg();
    int level = read_i32(in);
    int leaf_range = read_i32(in);
    int bucket_size = read_i32(in);
    int total_nodes = read_i32(in);
    uint64_t node_count = read_u64(in);
    if (node_count > static_cast<uint64_t>(total_nodes))
        throw std::runtime_error("DiskBinaryTreeStorage state node count invalid");

    size_t max_value = 0;
    for (uint64_t node = 0; node < node_count; ++node) {
        uint64_t bucket_count = read_u64(in);
        if (bucket_count > static_cast<uint64_t>(bucket_size))
            throw std::runtime_error("DiskBinaryTreeStorage state bucket invalid");
        for (uint64_t i = 0; i < bucket_count; ++i) {
            (void)read_i32(in);
            (void)read_i32(in);
            uint64_t value_size = read_u64(in);
            max_value = std::max(max_value, static_cast<size_t>(value_size));
            in.seekg(static_cast<std::streamoff>(value_size), std::ios::cur);
            if (!in) throw std::runtime_error("DiskBinaryTreeStorage state scan failed");
        }
    }

    in.clear();
    in.seekg(start);
    auto store = std::unique_ptr<DiskBinaryTreeStorage>(new DiskBinaryTreeStorage());
    store->directory_ = directory;
    std::filesystem::create_directories(store->directory_);
    store->file_path_ = make_file_path(store->directory_);
    store->remove_on_destroy_ = true;
    store->level_ = read_i32(in);
    store->leaf_range_ = read_i32(in);
    store->bucket_size_ = read_i32(in);
    store->total_nodes_ = read_i32(in);
    uint64_t nodes = read_u64(in);
    store->slot_value_size_ = max_value;
    store->base_file_path_.clear();
    store->dirty_nodes_.clear();
    store->create_empty_file();

    for (uint64_t node = 0; node < nodes; ++node) {
        uint64_t bucket_count = read_u64(in);
        std::vector<Block> bucket;
        bucket.reserve(static_cast<size_t>(bucket_count));
        for (uint64_t i = 0; i < bucket_count; ++i) {
            Block block;
            block.key = read_i32(in);
            block.leaf = read_i32(in);
            uint64_t value_size = read_u64(in);
            block.value.resize(static_cast<size_t>(value_size));
            if (!block.value.empty()) {
                in.read(reinterpret_cast<char*>(block.value.data()),
                        static_cast<std::streamsize>(block.value.size()));
                if (!in) throw std::runtime_error("DiskBinaryTreeStorage state read failed");
            }
            bucket.push_back(std::move(block));
        }
        if (!bucket.empty())
            store->write_bucket(static_cast<int>(node), bucket);
    }
    return store;
}

void DiskBinaryTreeStorage::save_file_snapshot(
    const std::string& meta_path, const std::string& data_path) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::filesystem::create_directories(
        std::filesystem::path(meta_path).parent_path());
    {
        std::ofstream meta(meta_path, std::ios::binary | std::ios::trunc);
        if (!meta)
            throw std::runtime_error("DiskBinaryTreeStorage snapshot meta open failed");
        write_i32(meta, level_);
        write_i32(meta, leaf_range_);
        write_i32(meta, bucket_size_);
        write_i32(meta, total_nodes_);
        write_u64(meta, static_cast<uint64_t>(slot_value_size_));
        write_u64(meta, static_cast<uint64_t>(
            std::filesystem::file_size(file_path_)));
    }
    copy_sparse_file(file_path_, data_path);
}

std::unique_ptr<DiskBinaryTreeStorage>
DiskBinaryTreeStorage::load_file_snapshot(
    const std::string& meta_path, const std::string& data_path,
    const std::string& runtime_directory) {
    std::ifstream meta(meta_path, std::ios::binary);
    if (!meta)
        throw std::runtime_error("DiskBinaryTreeStorage snapshot meta read failed");

    auto store = std::unique_ptr<DiskBinaryTreeStorage>(new DiskBinaryTreeStorage());
    store->directory_ = runtime_directory;
    std::filesystem::create_directories(store->directory_);
    store->file_path_ = make_file_path(store->directory_);
    store->remove_on_destroy_ = true;
    store->level_ = read_i32(meta);
    store->leaf_range_ = read_i32(meta);
    store->bucket_size_ = read_i32(meta);
    store->total_nodes_ = read_i32(meta);
    store->slot_value_size_ = static_cast<size_t>(read_u64(meta));
    uint64_t expected_size = read_u64(meta);

    store->base_file_path_ = data_path;
    store->dirty_nodes_.clear();
    store->create_empty_file();
    uint64_t actual_size = static_cast<uint64_t>(
        std::filesystem::file_size(store->base_file_path_));
    if (actual_size != expected_size)
        throw std::runtime_error("DiskBinaryTreeStorage snapshot size mismatch");
    return store;
}

std::unique_ptr<DiskBinaryTreeStorage>
DiskBinaryTreeStorage::from_binary_tree(const BinaryTreeStorage& src,
                                        const std::string& directory) {
    std::filesystem::create_directories(directory);
    std::filesystem::path tmp(directory);
    tmp /= "convert-" + std::to_string(::getpid()) + "-"
         + std::to_string(g_disk_store_counter.fetch_add(1)) + ".bin";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        src.write_state(out);
    }
    std::ifstream in(tmp, std::ios::binary);
    auto disk = read_state(in, directory);
    std::error_code ec;
    std::filesystem::remove(tmp, ec);
    return disk;
}

}  // namespace tiered_omap
