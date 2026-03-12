#include "tiered_omap/network/network_storage.h"

namespace tiered_omap {

NetworkStorage::NetworkStorage(std::shared_ptr<TcpChannel> channel,
                               int num_data, int bucket_size)
    : channel_(std::move(channel)) {
    Bytes req;
    ser_int(req, num_data);
    ser_int(req, bucket_size);
    auto resp = channel_->request(MsgType::CREATE, req);
    const uint8_t* p = resp.data();
    store_id_ = deser_int(p);
    level_ = deser_int(p);
    leaf_range_ = deser_int(p);
    bucket_size_ = bucket_size;
}

std::unique_ptr<NetworkStorage> NetworkStorage::from_existing(
    std::shared_ptr<TcpChannel> channel, int store_id,
    int level, int leaf_range, int bucket_size) {
    auto ns = std::unique_ptr<NetworkStorage>(new NetworkStorage());
    ns->channel_ = std::move(channel);
    ns->store_id_ = store_id;
    ns->level_ = level;
    ns->leaf_range_ = leaf_range;
    ns->bucket_size_ = bucket_size;
    return ns;
}

NetworkStorage::~NetworkStorage() {
    if (channel_ && channel_->is_open() && store_id_ >= 0) {
        try {
            Bytes req;
            ser_int(req, store_id_);
            channel_->request(MsgType::DESTROY, req);
        } catch (...) {}
    }
}

void NetworkStorage::reset(int num_data, int bucket_size) {
    Bytes req;
    ser_int(req, store_id_);
    ser_int(req, num_data);
    ser_int(req, bucket_size);
    auto resp = channel_->request(MsgType::RESET, req);
    const uint8_t* p = resp.data();
    level_ = deser_int(p);
    leaf_range_ = deser_int(p);
    bucket_size_ = bucket_size;
}

void NetworkStorage::fill_data_to_leaf(const Block& block) {
    Bytes req;
    ser_int(req, store_id_);
    ser_block(req, block);
    channel_->request(MsgType::FILL_DATA, req);
}

std::unordered_set<int>
NetworkStorage::bulk_load(const std::vector<Block>& blocks) {
    Bytes req;
    ser_int(req, store_id_);
    ser_int(req, static_cast<int>(blocks.size()));
    for (auto& b : blocks) ser_block(req, b);

    auto resp = channel_->request(MsgType::BULK_LOAD, req);
    const uint8_t* p = resp.data();
    int count = deser_int(p);
    std::unordered_set<int> placed;
    placed.reserve(count);
    for (int i = 0; i < count; ++i)
        placed.insert(deser_int(p));
    return placed;
}

std::unordered_map<int, std::vector<Block>>
NetworkStorage::read_path(int leaf) const {
    Bytes req;
    ser_int(req, store_id_);
    ser_int(req, leaf);
    auto resp = channel_->request(MsgType::READ_PATH, req);
    const uint8_t* p = resp.data();
    return deser_path(p);
}

void NetworkStorage::write_path(
    int leaf,
    const std::unordered_map<int, std::vector<Block>>& buckets) {
    Bytes req;
    ser_int(req, store_id_);
    ser_int(req, leaf);
    ser_path(req, buckets);
    channel_->request(MsgType::WRITE_PATH, req);
}

std::unordered_map<int, std::vector<Block>>
NetworkStorage::read_multiple_paths(const std::vector<int>& leaves) const {
    Bytes req;
    ser_int(req, store_id_);
    ser_int(req, static_cast<int>(leaves.size()));
    for (int lf : leaves) ser_int(req, lf);
    auto resp = channel_->request(MsgType::READ_MULTI, req);
    const uint8_t* p = resp.data();
    return deser_path(p);
}

void NetworkStorage::write_multiple_paths(
    const std::unordered_map<int, std::vector<Block>>& buckets) {
    Bytes req;
    ser_int(req, store_id_);
    ser_path(req, buckets);
    channel_->request(MsgType::WRITE_MULTI, req);
}

// ─── Batch helpers ──────────────────────────────────────────────────────────

std::vector<PathData> batch_read_paths(
    TcpChannel& ch, const std::vector<BatchReadReq>& reqs) {
    Bytes payload;
    ser_int(payload, static_cast<int>(reqs.size()));
    for (auto& r : reqs) {
        ser_int(payload, r.store_id);
        ser_int(payload, r.leaf);
    }
    auto resp = ch.request(MsgType::BATCH_READ, payload);
    const uint8_t* p = resp.data();
    int n = deser_int(p);
    std::vector<PathData> results;
    results.reserve(n);
    for (int i = 0; i < n; ++i)
        results.push_back(deser_path(p));
    return results;
}

void batch_write_paths(
    TcpChannel& ch, const std::vector<BatchWriteReq>& reqs) {
    Bytes payload;
    ser_int(payload, static_cast<int>(reqs.size()));
    for (auto& r : reqs) {
        ser_int(payload, r.store_id);
        ser_path(payload, r.data);
    }
    ch.request(MsgType::BATCH_WRITE, payload);
}

}  // namespace tiered_omap
