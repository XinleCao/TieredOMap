#pragma once

#include "tiered_omap/network/tcp_channel.h"
#include "tiered_omap/oram/binary_tree_storage.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace tiered_omap {

class StorageServer {
public:
    explicit StorageServer(int port);
    ~StorageServer();

    // Blocking: accepts clients in a loop (one thread per client).
    void run();
    void stop();

    int port() const { return port_; }

private:
    void handle_client(TcpChannel channel);

    // Per-connection store map (each client thread has its own via args).
    struct ClientState {
        std::unordered_map<int, std::unique_ptr<BinaryTreeStorage>> stores;
        int next_id = 0;
    };

    void dispatch(ClientState& state, MsgType type,
                  const Bytes& payload, TcpChannel& channel);

    int port_;
    int server_fd_ = -1;
    std::atomic<bool> running_{false};
};

}  // namespace tiered_omap
