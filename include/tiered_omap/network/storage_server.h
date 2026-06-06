#pragma once

#include "tiered_omap/network/tcp_channel.h"
#include "tiered_omap/oram/binary_tree_storage.h"
#include "tiered_omap/network/storage_interface.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace tiered_omap {

class StorageServer {
public:
    explicit StorageServer(int port, std::string setup_cache_dir = {},
                           std::string disk_data_dir = {});
    ~StorageServer();

    // Blocking: accepts clients in a loop (one thread per client).
    void run();
    void stop();

    int port() const { return port_; }

    // Per-connection store map (each client thread has its own via args).
    struct ClientState {
        std::unordered_map<int, std::unique_ptr<StorageInterface>> stores;
        int next_id = 0;
        bool next_store_on_disk = false;
        std::string disk_store_dir;
        std::string data_oram_cache_dir;

        int register_store(std::unique_ptr<StorageInterface> store);
    };

private:
    void handle_client(TcpChannel channel);

    void dispatch(ClientState& state, MsgType type,
                  const Bytes& payload, TcpChannel& channel);

    std::string setup_cache_path(const Bytes& payload) const;
    bool load_setup_cache(const Bytes& payload, ClientState& state,
                          Bytes& response);
    void save_setup_cache(const Bytes& payload, const ClientState& state,
                          const Bytes& response);

    int port_;
    int server_fd_ = -1;
    std::atomic<bool> running_{false};
    std::string setup_cache_dir_;
    std::string disk_data_dir_;
    std::mutex setup_cache_mutex_;
};

}  // namespace tiered_omap
