#pragma once

#include "tiered_omap/common.h"
#include "tiered_omap/tee/tee_omap.h"
#include <cstdint>
#include <functional>
#include <string>

namespace tiered_omap {
namespace tee {

// Wire protocol for TEE client ↔ TEE server.
//
// Request:  [op:1B] [key:4B] [value_len:4B] [value:*]
//   op = 'R' (read), 'W' (write)
//
// Response: [phase:1B] [found:1B] [value_len:4B] [value:*]
//   phase = 'H' (hot/early), 'F' (final)
//
// One request generates TWO responses: early (H) and final (F).

struct TeeRequest {
    char op = 'R';            // 'R' read, 'W' write
    int key = INVALID_KEY;
    Bytes value;              // only for writes
};

struct TeeResponse {
    char phase = 'H';         // 'H' hot/early, 'F' final
    bool found = false;
    Bytes value;
};

// Serialize / deserialize helpers.
Bytes serialize_request(const TeeRequest& req);
TeeRequest deserialize_request(const Bytes& data);
Bytes serialize_response(const TeeResponse& resp);
TeeResponse deserialize_response(const Bytes& data);

// TEE server: binds to a TCP port, accepts connections, and processes
// OMAP requests inside the enclave (TeeOmap).
class TeeServer {
public:
    TeeServer(const TeeOmapConfig& config, uint16_t port);

    // Initialise the OMAP with data, then start serving.
    void init(const std::vector<std::pair<int, Bytes>>& all_data,
              const std::vector<int>& hot_keys);

    // Serve one client connection (blocking).  Returns after client
    // disconnects.
    void serve_one();

    // Serve forever (blocks).
    void serve_forever();

    TeeOmap& omap() { return omap_; }

private:
    void handle_connection(int client_fd);

    TeeOmap omap_;
    uint16_t port_;
    int listen_fd_ = -1;
};

// Simple blocking client for testing.
class TeeClient {
public:
    TeeClient(const std::string& host, uint16_t port);
    ~TeeClient();

    // Send a read request.  Returns (early_response, final_response).
    std::pair<TeeResponse, TeeResponse> read(int key);

    // Send a write request.
    std::pair<TeeResponse, TeeResponse> write(int key, const Bytes& value);

private:
    std::pair<TeeResponse, TeeResponse> send_request(const TeeRequest& req);
    int fd_ = -1;
};

}  // namespace tee
}  // namespace tiered_omap
