#include "tiered_omap/tee/tee_server.h"

#include <arpa/inet.h>
#include <cstring>
#include <netinet/tcp.h>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>

namespace tiered_omap {
namespace tee {

// ── Wire format helpers ─────────────────────────────────────────────────────

Bytes serialize_request(const TeeRequest& req) {
    Bytes buf;
    buf.push_back(static_cast<uint8_t>(req.op));
    buf.resize(buf.size() + sizeof(int));
    std::memcpy(buf.data() + 1, &req.key, sizeof(int));
    int vlen = static_cast<int>(req.value.size());
    buf.resize(buf.size() + sizeof(int));
    std::memcpy(buf.data() + 1 + sizeof(int), &vlen, sizeof(int));
    buf.insert(buf.end(), req.value.begin(), req.value.end());
    return buf;
}

TeeRequest deserialize_request(const Bytes& data) {
    TeeRequest req;
    if (data.size() < 1 + sizeof(int) + sizeof(int)) return req;
    req.op = static_cast<char>(data[0]);
    std::memcpy(&req.key, data.data() + 1, sizeof(int));
    int vlen = 0;
    std::memcpy(&vlen, data.data() + 1 + sizeof(int), sizeof(int));
    if (vlen > 0 && data.size() >= 1 + 2 * sizeof(int) + vlen)
        req.value.assign(data.begin() + 1 + 2 * sizeof(int),
                         data.begin() + 1 + 2 * sizeof(int) + vlen);
    return req;
}

Bytes serialize_response(const TeeResponse& resp) {
    Bytes buf;
    buf.push_back(static_cast<uint8_t>(resp.phase));
    buf.push_back(static_cast<uint8_t>(resp.found ? 1 : 0));
    int vlen = static_cast<int>(resp.value.size());
    buf.resize(buf.size() + sizeof(int));
    std::memcpy(buf.data() + 2, &vlen, sizeof(int));
    buf.insert(buf.end(), resp.value.begin(), resp.value.end());
    return buf;
}

TeeResponse deserialize_response(const Bytes& data) {
    TeeResponse resp;
    if (data.size() < 2 + sizeof(int)) return resp;
    resp.phase = static_cast<char>(data[0]);
    resp.found = (data[1] != 0);
    int vlen = 0;
    std::memcpy(&vlen, data.data() + 2, sizeof(int));
    if (vlen > 0 && data.size() >= 2 + sizeof(int) + vlen)
        resp.value.assign(data.begin() + 2 + sizeof(int),
                          data.begin() + 2 + sizeof(int) + vlen);
    return resp;
}

// ── TCP send / recv helpers ─────────────────────────────────────────────────

static void send_all(int fd, const uint8_t* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = ::send(fd, data + sent, len - sent, 0);
        if (n <= 0) throw std::runtime_error("send failed");
        sent += n;
    }
}

static bool recv_all(int fd, uint8_t* buf, size_t len) {
    size_t got = 0;
    while (got < len) {
        ssize_t n = ::recv(fd, buf + got, len - got, 0);
        if (n <= 0) return false;
        got += n;
    }
    return true;
}

static void send_msg(int fd, const Bytes& msg) {
    uint32_t len = static_cast<uint32_t>(msg.size());
    uint8_t hdr[4];
    std::memcpy(hdr, &len, 4);
    send_all(fd, hdr, 4);
    if (len > 0) send_all(fd, msg.data(), len);
}

static bool recv_msg(int fd, Bytes& msg) {
    uint8_t hdr[4];
    if (!recv_all(fd, hdr, 4)) return false;
    uint32_t len;
    std::memcpy(&len, hdr, 4);
    msg.resize(len);
    if (len > 0)
        return recv_all(fd, msg.data(), len);
    return true;
}

// ── TeeServer ───────────────────────────────────────────────────────────────

TeeServer::TeeServer(const TeeOmapConfig& config, uint16_t port)
    : omap_(config), port_(port) {}

void TeeServer::init(const std::vector<std::pair<int, Bytes>>& all_data,
                     const std::vector<int>& hot_keys) {
    omap_.init(all_data, hot_keys);

    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) throw std::runtime_error("socket failed");

    int opt = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(listen_fd_, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port_);

    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
        throw std::runtime_error("bind failed");
    if (::listen(listen_fd_, 4) < 0)
        throw std::runtime_error("listen failed");
}

void TeeServer::serve_one() {
    sockaddr_in client_addr{};
    socklen_t len = sizeof(client_addr);
    int cfd = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&client_addr), &len);
    if (cfd < 0) throw std::runtime_error("accept failed");
    int opt = 1;
    setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
    handle_connection(cfd);
    ::close(cfd);
}

void TeeServer::serve_forever() {
    while (true) serve_one();
}

void TeeServer::handle_connection(int client_fd) {
    while (true) {
        Bytes req_data;
        if (!recv_msg(client_fd, req_data)) break;

        TeeRequest req = deserialize_request(req_data);

        const Bytes* write_val = req.op == 'W' ? &req.value : nullptr;

        auto early_cb = [&](const Bytes& val, bool found) {
            TeeResponse r;
            r.phase = 'H';
            r.found = found;
            r.value = val;
            send_msg(client_fd, serialize_response(r));
        };

        auto final_cb = [&](const Bytes& val, bool found) {
            TeeResponse r;
            r.phase = 'F';
            r.found = found;
            r.value = val;
            send_msg(client_fd, serialize_response(r));
        };

        omap_.access(req.key, write_val, early_cb, final_cb);
    }
}

// ── TeeClient ───────────────────────────────────────────────────────────────

TeeClient::TeeClient(const std::string& host, uint16_t port) {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) throw std::runtime_error("socket failed");

    int opt = 1;
    setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, host.c_str(), &addr.sin_addr);

    if (::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
        throw std::runtime_error("connect failed");
}

TeeClient::~TeeClient() {
    if (fd_ >= 0) ::close(fd_);
}

std::pair<TeeResponse, TeeResponse> TeeClient::read(int key) {
    TeeRequest req;
    req.op = 'R';
    req.key = key;
    return send_request(req);
}

std::pair<TeeResponse, TeeResponse> TeeClient::write(int key,
                                                       const Bytes& value) {
    TeeRequest req;
    req.op = 'W';
    req.key = key;
    req.value = value;
    return send_request(req);
}

std::pair<TeeResponse, TeeResponse>
TeeClient::send_request(const TeeRequest& req) {
    send_msg(fd_, serialize_request(req));

    TeeResponse early, final_r;

    Bytes resp1_data;
    if (!recv_msg(fd_, resp1_data))
        throw std::runtime_error("recv early response failed");
    early = deserialize_response(resp1_data);

    Bytes resp2_data;
    if (!recv_msg(fd_, resp2_data))
        throw std::runtime_error("recv final response failed");
    final_r = deserialize_response(resp2_data);

    return {early, final_r};
}

}  // namespace tee
}  // namespace tiered_omap
