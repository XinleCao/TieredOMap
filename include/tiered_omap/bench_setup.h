#pragma once

#include "tiered_omap/tiered_omap.h"
#include "tiered_omap/network/tcp_channel.h"
#include "tiered_omap/network/storage_server.h"
#include "tiered_omap/omap/avl_omap.h"
#include "tiered_omap/omap/bplus_omap.h"
#include "tiered_omap/omap/da_ost_omap.h"
#include <memory>

namespace tiered_omap {
namespace bench_setup {

// Server-side: create OMAP locally, register storages with the server's
// ClientState, and return serialized client state blob.
Bytes setup_standalone_on_server(
    StorageServer::ClientState& stores,
    OmapBackend backend, int N, int bucket_size, int value_size,
    int init_n = 0);

Bytes setup_tiered_on_server(
    StorageServer::ClientState& stores,
    OmapBackend backend, int N, int n, int bucket_size, int value_size,
    SecurityMode mode, bool use_split,
    OmapBackend hot_backend = OmapBackend::AVL, bool use_hot_backend = false);

Bytes setup_index_data_on_server(
    StorageServer::ClientState& stores,
    OmapBackend backend, int N, int bucket_size, int value_size);

// Client-side: reconstruct OMAP from the server state blob + TCP channel.
std::unique_ptr<OmapInterface> restore_standalone(
    const uint8_t*& p, std::shared_ptr<TcpChannel> channel);

std::unique_ptr<TieredOMap> restore_tiered(
    const uint8_t*& p, std::shared_ptr<TcpChannel> channel);

struct IndexDataParts {
    std::unique_ptr<OmapInterface> index;
    PathORAM data;
};

IndexDataParts restore_index_data(
    const uint8_t*& p, std::shared_ptr<TcpChannel> channel);

}  // namespace bench_setup
}  // namespace tiered_omap
