#include "tiered_omap/network/tcp_channel.h"
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <netdb.h>
#include <netinet/tcp.h>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>

namespace tiered_omap {

TcpChannel::~TcpChannel() { close(); }

TcpChannel& TcpChannel::operator=(TcpChannel&& o) noexcept {
    if (this != &o) { close(); fd_ = o.fd_; o.fd_ = -1; }
    return *this;
}

void TcpChannel::close() {
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
}

TcpChannel TcpChannel::connect(const std::string& host, int port) {
    struct addrinfo hints{}, *res;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    std::string port_str = std::to_string(port);
    if (getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res) != 0)
        throw std::runtime_error("TcpChannel::connect: getaddrinfo failed");

    int fd = -1;
    for (auto* p = res; p; p = p->ai_next) {
        fd = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) continue;
        if (::connect(fd, p->ai_addr, p->ai_addrlen) == 0) break;
        ::close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0)
        throw std::runtime_error("TcpChannel::connect: failed to " +
                                 host + ":" + std::to_string(port));

    int flag = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
    return TcpChannel(fd);
}

int TcpChannel::listen(int port, int backlog) {
    int fd = ::socket(AF_INET6, SOCK_STREAM, 0);
    if (fd < 0) fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) throw std::runtime_error("TcpChannel::listen: socket failed");

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in6 addr{};
    addr.sin6_family = AF_INET6;
    addr.sin6_port = htons(static_cast<uint16_t>(port));
    addr.sin6_addr = in6addr_any;

    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        throw std::runtime_error(
            std::string("TcpChannel::listen: bind failed: ") + strerror(errno));
    }
    if (::listen(fd, backlog) < 0) {
        ::close(fd);
        throw std::runtime_error("TcpChannel::listen: listen failed");
    }
    return fd;
}

TcpChannel TcpChannel::accept(int server_fd) {
    struct sockaddr_storage addr;
    socklen_t len = sizeof(addr);
    int fd = ::accept(server_fd, reinterpret_cast<sockaddr*>(&addr), &len);
    if (fd < 0) throw std::runtime_error("TcpChannel::accept failed");
    int flag = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
    return TcpChannel(fd);
}

void TcpChannel::send_raw(const uint8_t* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = ::send(fd_, data + sent, len - sent, 0);
        if (n <= 0)
            throw std::runtime_error("TcpChannel::send_raw: connection lost");
        sent += static_cast<size_t>(n);
    }
}

bool TcpChannel::recv_raw(uint8_t* buf, size_t len) {
    size_t got = 0;
    while (got < len) {
        ssize_t n = ::recv(fd_, buf + got, len - got, 0);
        if (n == 0) return false;
        if (n < 0)
            throw std::runtime_error("TcpChannel::recv_raw: error");
        got += static_cast<size_t>(n);
    }
    return true;
}

void TcpChannel::send_msg(MsgType type, const uint8_t* data, size_t len) {
    uint32_t total = static_cast<uint32_t>(len + 1);
    uint8_t header[5];
    std::memcpy(header, &total, 4);
    header[4] = static_cast<uint8_t>(type);
    send_raw(header, 5);
    if (len > 0) send_raw(data, len);
}

bool TcpChannel::recv_msg(MsgType& type, Bytes& payload) {
    uint8_t header[5];
    if (!recv_raw(header, 5)) return false;
    uint32_t total;
    std::memcpy(&total, header, 4);
    type = static_cast<MsgType>(header[4]);
    payload.resize(total > 1 ? total - 1 : 0);
    if (!payload.empty())
        if (!recv_raw(payload.data(), payload.size())) return false;
    return true;
}

Bytes TcpChannel::request(MsgType type, const Bytes& payload) {
    send_msg(type, payload);
    MsgType resp_type;
    Bytes resp;
    if (!recv_msg(resp_type, resp))
        throw std::runtime_error("TcpChannel::request: disconnected");
    if (resp_type == MsgType::ERROR)
        throw std::runtime_error("Server error: " +
            std::string(resp.begin(), resp.end()));
    return resp;
}

// ─── Serialization ──────────────────────────────────────────────────────────

void ser_block(Bytes& buf, const Block& b) {
    ser_int(buf, b.key);
    ser_int(buf, b.leaf);
    ser_bytes(buf, b.value);
}

Block deser_block(const uint8_t*& p) {
    Block b;
    b.key = deser_int(p);
    b.leaf = deser_int(p);
    b.value = deser_bytes(p);
    return b;
}

void ser_path(Bytes& buf,
              const std::unordered_map<int, std::vector<Block>>& path) {
    ser_int(buf, static_cast<int>(path.size()));
    for (auto& [node, bucket] : path) {
        ser_int(buf, node);
        ser_int(buf, static_cast<int>(bucket.size()));
        for (auto& b : bucket) ser_block(buf, b);
    }
}

std::unordered_map<int, std::vector<Block>> deser_path(const uint8_t*& p) {
    int count = deser_int(p);
    std::unordered_map<int, std::vector<Block>> result;
    result.reserve(count);
    for (int i = 0; i < count; ++i) {
        int node = deser_int(p);
        int nb = deser_int(p);
        auto& bucket = result[node];
        bucket.reserve(nb);
        for (int j = 0; j < nb; ++j)
            bucket.push_back(deser_block(p));
    }
    return result;
}

}  // namespace tiered_omap
