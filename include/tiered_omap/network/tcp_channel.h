#pragma once

#include "tiered_omap/common.h"
#include <cstdint>
#include <string>

namespace tiered_omap {

enum class MsgType : uint8_t {
    CREATE      = 0x01,
    RESET       = 0x02,
    FILL_DATA   = 0x03,
    BULK_LOAD   = 0x04,
    READ_PATH   = 0x05,
    WRITE_PATH  = 0x06,
    READ_MULTI  = 0x07,
    WRITE_MULTI = 0x08,
    DESTROY     = 0x09,
    SETUP_BENCH = 0x10,
    BATCH_READ  = 0x11,
    BATCH_WRITE = 0x12,

    OK          = 0x80,
    ERROR       = 0x81,
};

class TcpChannel {
public:
    TcpChannel() = default;
    explicit TcpChannel(int fd) : fd_(fd) {}
    ~TcpChannel();

    TcpChannel(TcpChannel&& o) noexcept : fd_(o.fd_) { o.fd_ = -1; }
    TcpChannel& operator=(TcpChannel&& o) noexcept;

    TcpChannel(const TcpChannel&) = delete;
    TcpChannel& operator=(const TcpChannel&) = delete;

    static TcpChannel connect(const std::string& host, int port);
    static int listen(int port, int backlog = 16);
    static TcpChannel accept(int server_fd);

    void send_msg(MsgType type, const uint8_t* data, size_t len);
    void send_msg(MsgType type, const Bytes& payload) {
        send_msg(type, payload.data(), payload.size());
    }

    // Returns false on clean disconnect.
    bool recv_msg(MsgType& type, Bytes& payload);

    // Send request + block for OK response. Throws on error response.
    Bytes request(MsgType type, const Bytes& payload);

    void close();
    bool is_open() const { return fd_ >= 0; }
    int fd() const { return fd_; }

    void set_recv_timeout(int seconds);

private:
    void send_raw(const uint8_t* data, size_t len);
    bool recv_raw(uint8_t* buf, size_t len);

    int fd_ = -1;
};

// Serialization helpers (little-endian).
inline void ser_int(Bytes& buf, int v) {
    size_t pos = buf.size();
    buf.resize(pos + 4);
    std::memcpy(buf.data() + pos, &v, 4);
}

inline int deser_int(const uint8_t*& p) {
    int v;
    std::memcpy(&v, p, 4);
    p += 4;
    return v;
}

inline void ser_bytes(Bytes& buf, const Bytes& data) {
    ser_int(buf, static_cast<int>(data.size()));
    buf.insert(buf.end(), data.begin(), data.end());
}

inline Bytes deser_bytes(const uint8_t*& p) {
    int len = deser_int(p);
    Bytes v(p, p + len);
    p += len;
    return v;
}

void ser_block(Bytes& buf, const Block& b);
Block deser_block(const uint8_t*& p);

void ser_path(Bytes& buf,
              const std::unordered_map<int, std::vector<Block>>& path);
std::unordered_map<int, std::vector<Block>> deser_path(const uint8_t*& p);

}  // namespace tiered_omap
