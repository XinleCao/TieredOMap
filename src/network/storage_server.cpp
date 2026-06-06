#include "tiered_omap/network/storage_server.h"
#include "tiered_omap/bench_setup.h"
#include "tiered_omap/oram/disk_binary_tree_storage.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <thread>
#include <unistd.h>

namespace tiered_omap {

namespace {

constexpr int kSetupCacheMagic = 0x534d4f54; // "TOMS" little-endian
constexpr int kSetupCacheVersion = 2;

void write_i32(std::ostream& out, int v) {
    out.write(reinterpret_cast<const char*>(&v), sizeof(v));
    if (!out) throw std::runtime_error("setup cache write failed");
}

int read_i32(std::istream& in) {
    int v = 0;
    in.read(reinterpret_cast<char*>(&v), sizeof(v));
    if (!in) throw std::runtime_error("setup cache read failed");
    return v;
}

void write_u64(std::ostream& out, uint64_t v) {
    out.write(reinterpret_cast<const char*>(&v), sizeof(v));
    if (!out) throw std::runtime_error("setup cache write failed");
}

uint64_t read_u64(std::istream& in) {
    uint64_t v = 0;
    in.read(reinterpret_cast<char*>(&v), sizeof(v));
    if (!in) throw std::runtime_error("setup cache read failed");
    return v;
}

}  // namespace

int StorageServer::ClientState::register_store(
    std::unique_ptr<StorageInterface> store) {
    int id = next_id++;
    if (next_store_on_disk && !disk_store_dir.empty()) {
        if (auto* mem = dynamic_cast<BinaryTreeStorage*>(store.get())) {
            stores[id] = DiskBinaryTreeStorage::from_binary_tree(
                *mem, disk_store_dir);
        } else {
            stores[id] = std::move(store);
        }
    } else {
        stores[id] = std::move(store);
    }
    next_store_on_disk = false;
    return id;
}

StorageServer::StorageServer(int port, std::string setup_cache_dir,
                             std::string disk_data_dir)
    : port_(port),
      setup_cache_dir_(std::move(setup_cache_dir)),
      disk_data_dir_(std::move(disk_data_dir)) {
    server_fd_ = TcpChannel::listen(port);
    if (!setup_cache_dir_.empty()) {
        std::filesystem::create_directories(setup_cache_dir_);
        std::cerr << "[StorageServer] setup cache dir: "
                  << setup_cache_dir_ << "\n";
    }
    if (!disk_data_dir_.empty()) {
        std::filesystem::create_directories(disk_data_dir_);
        std::cerr << "[StorageServer] disk data dir: "
                  << disk_data_dir_ << "\n";
    }
}

StorageServer::~StorageServer() {
    stop();
    if (server_fd_ >= 0) ::close(server_fd_);
}

void StorageServer::stop() { running_ = false; }

void StorageServer::run() {
    running_ = true;
    std::cerr << "[StorageServer] listening on port " << port_ << "\n";
    while (running_) {
        try {
            auto client = TcpChannel::accept(server_fd_);
            std::cerr << "[StorageServer] client connected\n";
            std::thread([this, ch = std::move(client)]() mutable {
                handle_client(std::move(ch));
            }).detach();
        } catch (const std::exception& e) {
            if (running_)
                std::cerr << "[StorageServer] accept error: " << e.what() << "\n";
        }
    }
}

void StorageServer::handle_client(TcpChannel channel) {
    ClientState state;
    state.disk_store_dir = disk_data_dir_;
    if (!setup_cache_dir_.empty()) {
        std::filesystem::path p(setup_cache_dir_);
        p /= "data_oram";
        state.data_oram_cache_dir = p.string();
    }
    try {
        while (true) {
            MsgType type;
            Bytes payload;
            if (!channel.recv_msg(type, payload)) break;
            dispatch(state, type, payload, channel);
        }
    } catch (const std::exception& e) {
        std::cerr << "[StorageServer] client error: " << e.what() << "\n";
    }
    std::cerr << "[StorageServer] client disconnected ("
              << state.stores.size() << " stores freed)\n";
}

std::string StorageServer::setup_cache_path(const Bytes& payload) const {
    if (setup_cache_dir_.empty())
        return {};
    std::ostringstream key;
    key << "setup-v" << kSetupCacheVersion << "-";
    key << std::hex << std::setfill('0');
    for (uint8_t byte : payload)
        key << std::setw(2) << static_cast<int>(byte);
    std::filesystem::path p(setup_cache_dir_);
    p /= key.str() + ".bin";
    return p.string();
}

bool StorageServer::load_setup_cache(const Bytes& payload, ClientState& state,
                                     Bytes& response) {
    std::string path = setup_cache_path(payload);
    if (path.empty() || !std::filesystem::exists(path))
        return false;

    std::lock_guard<std::mutex> lock(setup_cache_mutex_);
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;

    try {
        int magic = read_i32(in);
        int version = read_i32(in);
        if (magic != kSetupCacheMagic || version != kSetupCacheVersion)
            return false;

        uint64_t response_size = read_u64(in);
        if (response_size >
            static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
            throw std::runtime_error("setup cache response too large");
        }
        response.resize(static_cast<size_t>(response_size));
        if (!response.empty()) {
            in.read(reinterpret_cast<char*>(response.data()),
                    static_cast<std::streamsize>(response.size()));
            if (!in) throw std::runtime_error("setup cache response read failed");
        }

        ClientState loaded;
        loaded.next_id = read_i32(in);
        uint64_t store_count = read_u64(in);
        if (store_count > 1024)
            throw std::runtime_error("setup cache store count too large");
        loaded.disk_store_dir = disk_data_dir_;
        for (uint64_t i = 0; i < store_count; ++i) {
            int id = read_i32(in);
            int store_type = read_i32(in);
            if (store_type == 1 && !disk_data_dir_.empty()) {
                loaded.stores[id] = DiskBinaryTreeStorage::read_state(
                    in, disk_data_dir_);
            } else {
                loaded.stores[id] = BinaryTreeStorage::read_state(in);
            }
        }

        state.stores = std::move(loaded.stores);
        state.next_id = loaded.next_id;
        std::cerr << "[StorageServer] SETUP_CACHE hit path=" << path
                  << " stores=" << state.stores.size() << "\n";
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[StorageServer] SETUP_CACHE load failed: "
                  << e.what() << " path=" << path << "\n";
        state.stores.clear();
        state.next_id = 0;
        response.clear();
        return false;
    }
}

void StorageServer::save_setup_cache(const Bytes& payload,
                                     const ClientState& state,
                                     const Bytes& response) {
    std::string path = setup_cache_path(payload);
    if (path.empty())
        return;

    for (const auto& [_, store] : state.stores) {
        if (dynamic_cast<DiskBinaryTreeStorage*>(store.get())) {
            std::cerr << "[StorageServer] SETUP_CACHE skipped full config "
                      << "cache because disk data store is cached separately\n";
            return;
        }
    }

    std::lock_guard<std::mutex> lock(setup_cache_mutex_);
    if (std::filesystem::exists(path))
        return;

    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    std::string tmp = path + ".tmp." + std::to_string(::getpid());
    try {
        std::ofstream out(tmp, std::ios::binary);
        if (!out)
            throw std::runtime_error("open failed");
        write_i32(out, kSetupCacheMagic);
        write_i32(out, kSetupCacheVersion);
        write_u64(out, static_cast<uint64_t>(response.size()));
        if (!response.empty()) {
            out.write(reinterpret_cast<const char*>(response.data()),
                      static_cast<std::streamsize>(response.size()));
            if (!out) throw std::runtime_error("response write failed");
        }
        write_i32(out, state.next_id);
        write_u64(out, static_cast<uint64_t>(state.stores.size()));

        std::vector<int> ids;
        ids.reserve(state.stores.size());
        for (const auto& [id, _] : state.stores)
            ids.push_back(id);
        std::sort(ids.begin(), ids.end());

        for (int id : ids) {
            auto it = state.stores.find(id);
            if (it == state.stores.end() || !it->second)
                throw std::runtime_error("missing store during cache save");
            write_i32(out, id);
            if (auto* disk = dynamic_cast<DiskBinaryTreeStorage*>(it->second.get())) {
                write_i32(out, 1);
                disk->write_state(out);
            } else if (auto* mem = dynamic_cast<BinaryTreeStorage*>(it->second.get())) {
                write_i32(out, 0);
                mem->write_state(out);
            } else {
                throw std::runtime_error("unsupported store type during cache save");
            }
        }
        out.close();
        if (!out)
            throw std::runtime_error("close failed");
        std::filesystem::rename(tmp, path);
        std::cerr << "[StorageServer] SETUP_CACHE saved path=" << path
                  << " stores=" << state.stores.size() << "\n";
    } catch (const std::exception& e) {
        std::cerr << "[StorageServer] SETUP_CACHE save failed: "
                  << e.what() << " path=" << path << "\n";
        std::error_code ec;
        std::filesystem::remove(tmp, ec);
    }
}

void StorageServer::dispatch(ClientState& state, MsgType type,
                             const Bytes& payload, TcpChannel& channel) {
    const uint8_t* p = payload.data();

    auto send_ok = [&](const Bytes& data = {}) {
        channel.send_msg(MsgType::OK, data);
    };
    auto send_err = [&](const std::string& msg) {
        channel.send_msg(MsgType::ERROR,
            Bytes(msg.begin(), msg.end()));
    };
    auto get_store = [&](int id) -> StorageInterface* {
        auto it = state.stores.find(id);
        return (it != state.stores.end()) ? it->second.get() : nullptr;
    };

    switch (type) {
    case MsgType::CREATE: {
        int num_data = deser_int(p);
        int bucket_size = deser_int(p);
        int id = state.next_id++;
        auto store = std::make_unique<BinaryTreeStorage>(num_data, bucket_size);
        Bytes resp;
        ser_int(resp, id);
        ser_int(resp, store->level());
        ser_int(resp, store->leaf_range());
        state.stores[id] = std::move(store);
        send_ok(resp);
        break;
    }
    case MsgType::RESET: {
        int id = deser_int(p);
        int num_data = deser_int(p);
        int bucket_size = deser_int(p);
        auto* store = get_store(id);
        if (!store) { send_err("bad store id"); break; }
        store->reset(num_data, bucket_size);
        Bytes resp;
        ser_int(resp, store->level());
        ser_int(resp, store->leaf_range());
        send_ok(resp);
        break;
    }
    case MsgType::FILL_DATA: {
        int id = deser_int(p);
        Block b = deser_block(p);
        auto* store = get_store(id);
        if (!store) { send_err("bad store id"); break; }
        store->fill_data_to_leaf(b);
        send_ok();
        break;
    }
    case MsgType::BULK_LOAD: {
        int id = deser_int(p);
        int count = deser_int(p);
        std::vector<Block> blocks;
        blocks.reserve(count);
        for (int i = 0; i < count; ++i)
            blocks.push_back(deser_block(p));
        auto* store = get_store(id);
        if (!store) { send_err("bad store id"); break; }
        auto placed = store->bulk_load(blocks);
        Bytes resp;
        ser_int(resp, static_cast<int>(placed.size()));
        for (int k : placed) ser_int(resp, k);
        send_ok(resp);
        break;
    }
    case MsgType::READ_PATH: {
        int id = deser_int(p);
        int leaf = deser_int(p);
        auto* store = get_store(id);
        if (!store) { send_err("bad store id"); break; }
        auto path = store->read_path(leaf);
        Bytes resp;
        ser_path(resp, path);
        send_ok(resp);
        break;
    }
    case MsgType::WRITE_PATH: {
        int id = deser_int(p);
        int leaf = deser_int(p);
        auto path = deser_path(p);
        auto* store = get_store(id);
        if (!store) { send_err("bad store id"); break; }
        store->write_path(leaf, path);
        send_ok();
        break;
    }
    case MsgType::READ_MULTI: {
        int id = deser_int(p);
        int nl = deser_int(p);
        std::vector<int> leaves(nl);
        for (int i = 0; i < nl; ++i) leaves[i] = deser_int(p);
        auto* store = get_store(id);
        if (!store) { send_err("bad store id"); break; }
        auto path = store->read_multiple_paths(leaves);
        Bytes resp;
        ser_path(resp, path);
        send_ok(resp);
        break;
    }
    case MsgType::WRITE_MULTI: {
        int id = deser_int(p);
        auto path = deser_path(p);
        auto* store = get_store(id);
        if (!store) { send_err("bad store id"); break; }
        store->write_multiple_paths(path);
        send_ok();
        break;
    }
    case MsgType::BATCH_READ: {
        int n = deser_int(p);
        Bytes resp;
        ser_int(resp, n);
        for (int i = 0; i < n; ++i) {
            int id = deser_int(p);
            int leaf = deser_int(p);
            auto* store = get_store(id);
            if (!store) { send_err("bad store id in BATCH_READ"); return; }
            auto path = store->read_path(leaf);
            ser_path(resp, path);
        }
        send_ok(resp);
        break;
    }
    case MsgType::BATCH_WRITE: {
        int n = deser_int(p);
        for (int i = 0; i < n; ++i) {
            int id = deser_int(p);
            auto path = deser_path(p);
            auto* store = get_store(id);
            if (!store) { send_err("bad store id in BATCH_WRITE"); return; }
            store->write_multiple_paths(path);
        }
        send_ok();
        break;
    }
    case MsgType::DESTROY: {
        int id = deser_int(p);
        state.stores.erase(id);
        send_ok();
        break;
    }
    case MsgType::SETUP_BENCH: {
        state.stores.clear();
        state.next_id = 0;
        int mode = deser_int(p);       // 0=standalone, 1=tiered
        int backend_i = deser_int(p);
        int N = deser_int(p);
        int n = deser_int(p);          // hot set size (0 for standalone)
        int bucket_size = deser_int(p);
        int value_size = deser_int(p);
        int security = deser_int(p);   // 0=FO, 1=TM
        int use_split = deser_int(p);
        int data_count = (p < payload.data() + payload.size()) ? deser_int(p) : 0;
        int hot_be_i = (p < payload.data() + payload.size()) ? deser_int(p) : 0;
        int use_hot_be = (p < payload.data() + payload.size()) ? deser_int(p) : 0;
        int epoch_values = (p < payload.data() + payload.size()) ? deser_int(p) : 0;

        auto backend = static_cast<OmapBackend>(backend_i);
        auto hot_be = static_cast<OmapBackend>(hot_be_i);
        std::cerr << "[StorageServer] SETUP_BENCH: mode=" << mode
                  << " backend=" << backend_i << " N=" << N
                  << " n=" << n << " value_size=" << value_size << "\n";

        auto sec_mode = (security == 0) ? SecurityMode::FullOblivious
                                        : SecurityMode::TierMembership;
        Bytes result;
        if (load_setup_cache(payload, state, result)) {
            send_ok(result);
            std::cerr << "[StorageServer] SETUP_BENCH cached, state_size="
                      << result.size() << " stores=" << state.stores.size()
                      << "\n";
            break;
        }

        if (mode == 0) {
            int init_n = (data_count > 0) ? data_count : N;
            result = bench_setup::setup_standalone_on_server(
                state, backend, N, bucket_size, value_size, init_n);
        } else if (mode == 2) {
            result = bench_setup::setup_index_data_on_server(
                state, backend, N, bucket_size, value_size);
        } else {
            result = bench_setup::setup_tiered_on_server(
                state, backend, N, n, bucket_size, value_size,
                sec_mode, use_split != 0, hot_be, use_hot_be != 0,
                epoch_values != 0);
        }
        save_setup_cache(payload, state, result);
        send_ok(result);
        std::cerr << "[StorageServer] SETUP_BENCH done, state_size="
                  << result.size() << " stores=" << state.stores.size() << "\n";
        break;
    }
    default:
        send_err("unknown message type");
        break;
    }
}

}  // namespace tiered_omap
