#include "tiered_omap/tee/tee_omap.h"
#include "tiered_omap/tee/tee_server.h"
#include "tiered_omap/common.h"

#include <arpa/inet.h>
#include <cstring>
#include <iostream>
#include <memory>
#include <netinet/tcp.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

using namespace tiered_omap;
using namespace tiered_omap::tee;

static void send_all(int fd, const uint8_t* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = ::send(fd, data + sent, len - sent, 0);
        if (n <= 0) throw std::runtime_error("send failed");
        sent += static_cast<size_t>(n);
    }
}

static bool recv_all(int fd, uint8_t* buf, size_t len) {
    size_t got = 0;
    while (got < len) {
        ssize_t n = ::recv(fd, buf + got, len - got, 0);
        if (n <= 0) return false;
        got += static_cast<size_t>(n);
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
    if (len > 0) return recv_all(fd, msg.data(), len);
    return true;
}

static void handle_connection(int client_fd, TeeOmap& omap) {
    int opt = 1;
    setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

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

        omap.access(req.key, write_val, early_cb, final_cb);
    }
}

int main(int argc, char** argv) {
    int min_logN = 14, max_logN = 22;
    int n = 1024, val_size = 256;
    uint16_t port = 9000;

    for (int i = 1; i + 1 < argc; i += 2) {
        std::string k = argv[i], v = argv[i + 1];
        if (k == "--min_logN") min_logN = std::stoi(v);
        else if (k == "--max_logN") max_logN = std::stoi(v);
        else if (k == "--n") n = std::stoi(v);
        else if (k == "--val") val_size = std::stoi(v);
        else if (k == "--port") port = static_cast<uint16_t>(std::stoi(v));
    }

    int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) { perror("socket"); return 1; }
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(listen_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    if (::bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        perror("bind"); return 1;
    }
    if (::listen(listen_fd, 1) < 0) {
        perror("listen"); return 1;
    }
    std::cout << "Listening on port " << port << "\n";

    for (int logN = min_logN; logN <= max_logN; logN += 2) {
        int N = 1 << logN;
        int actual_n = std::min(n, N / 2);

        std::cout << "\n=== logN=" << logN << " N=" << N
                  << " n=" << actual_n << " ===" << std::endl;

        TeeOmapConfig cfg;
        cfg.total_keys = N;
        cfg.hot_set_size = actual_n;
        cfg.value_size = val_size;
        cfg.mode = TeeSecurityMode::TierMembership;
        cfg.use_split_oram = true;

        std::cout << "Building OMAP..." << std::flush;
        std::vector<std::pair<int, Bytes>> data;
        data.reserve(N);
        for (int i = 0; i < N; ++i)
            data.push_back({i, pad_bytes(int_to_bytes(i), val_size)});
        std::vector<int> hk(actual_n);
        for (int i = 0; i < actual_n; ++i) hk[i] = i;

        auto omap = std::make_unique<TeeOmap>(cfg);
        omap->init(data, hk);
        std::cout << " done.\nWaiting for client..." << std::endl;

        while (true) {
            sockaddr_in client_addr{};
            socklen_t len = sizeof(client_addr);
            int cfd = ::accept(listen_fd,
                               reinterpret_cast<sockaddr*>(&client_addr), &len);
            if (cfd < 0) { perror("accept"); continue; }

            std::cout << "Client connected, serving queries...\n";

            Bytes first_msg;
            if (!recv_msg(cfd, first_msg)) {
                std::cout << "  (probe connection, ignoring)\n";
                ::close(cfd);
                continue;
            }

            TeeRequest req = deserialize_request(first_msg);
            const Bytes* write_val = req.op == 'W' ? &req.value : nullptr;

            auto early_cb = [&](const Bytes& val, bool found) {
                TeeResponse r; r.phase = 'H'; r.found = found; r.value = val;
                send_msg(cfd, serialize_response(r));
            };
            auto final_cb = [&](const Bytes& val, bool found) {
                TeeResponse r; r.phase = 'F'; r.found = found; r.value = val;
                send_msg(cfd, serialize_response(r));
            };

            omap->access(req.key, write_val, early_cb, final_cb);

            handle_connection(cfd, *omap);
            ::close(cfd);
            std::cout << "Client disconnected. logN=" << logN << " done.\n";
            break;
        }

        omap.reset();
    }

    ::close(listen_fd);
    std::cout << "\nAll done.\n";
    return 0;
}
