#include "tiered_omap/network/storage_server.h"
#include "tiered_omap/bench_setup.h"
#include <iostream>
#include <thread>
#include <unistd.h>

namespace tiered_omap {

StorageServer::StorageServer(int port) : port_(port) {
    server_fd_ = TcpChannel::listen(port);
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
    auto get_store = [&](int id) -> BinaryTreeStorage* {
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
