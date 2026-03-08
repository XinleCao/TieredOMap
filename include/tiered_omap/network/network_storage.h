#pragma once

#include "tiered_omap/network/storage_interface.h"
#include "tiered_omap/network/tcp_channel.h"
#include <memory>

namespace tiered_omap {

// Client-side StorageInterface that delegates to a remote StorageServer via TCP.
class NetworkStorage : public StorageInterface {
public:
    // Creates a new storage on the remote server.
    NetworkStorage(std::shared_ptr<TcpChannel> channel,
                   int num_data, int bucket_size);
    ~NetworkStorage() override;

    NetworkStorage(NetworkStorage&&) noexcept = default;
    NetworkStorage& operator=(NetworkStorage&&) noexcept = default;

    int level() const override { return level_; }
    int leaf_range() const override { return leaf_range_; }
    int bucket_size() const override { return bucket_size_; }

    void reset(int num_data, int bucket_size) override;
    void fill_data_to_leaf(const Block& block) override;

    std::unordered_set<int>
    bulk_load(const std::vector<Block>& blocks) override;

    std::unordered_map<int, std::vector<Block>>
    read_path(int leaf) const override;

    void write_path(
        int leaf,
        const std::unordered_map<int, std::vector<Block>>& buckets) override;

    std::unordered_map<int, std::vector<Block>>
    read_multiple_paths(const std::vector<int>& leaves) const override;

    void write_multiple_paths(
        const std::unordered_map<int, std::vector<Block>>& buckets) override;

    int store_id() const { return store_id_; }

private:
    std::shared_ptr<TcpChannel> channel_;
    int store_id_ = -1;
    int level_ = 0;
    int leaf_range_ = 0;
    int bucket_size_ = 0;
};

// Returns a StorageCreator that creates NetworkStorage via the given channel.
inline StorageCreator make_network_creator(std::shared_ptr<TcpChannel> channel) {
    return [ch = std::move(channel)](int n, int bs)
        -> std::unique_ptr<StorageInterface> {
        return std::make_unique<NetworkStorage>(ch, n, bs);
    };
}

}  // namespace tiered_omap
