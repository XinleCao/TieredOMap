#include "tiered_omap/tee/tee_server.h"
#include "tiered_omap/workload.h"
#include <arpa/inet.h>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <netinet/tcp.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace tiered_omap;
using namespace tiered_omap::tee;
using Clock = std::chrono::steady_clock;

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

static int connect_with_retry(const std::string& host, uint16_t port,
                              int max_retries = 7200, int interval_s = 5) {
    for (int r = 0; r < max_retries; ++r) {
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) continue;

        struct timeval tv{10, 0};
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        int opt = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        inet_pton(AF_INET, host.c_str(), &addr.sin_addr);

        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
            struct timeval no_timeout{0, 0};
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &no_timeout, sizeof(no_timeout));
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &no_timeout, sizeof(no_timeout));
            return fd;
        }
        ::close(fd);
        if (r % 12 == 0)
            std::cout << "  [retry " << r << "] waiting for server..." << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(interval_s));
    }
    return -1;
}

struct Config {
    std::string host = "127.0.0.1";
    uint16_t port = 9000;
    int min_logN = 14, max_logN = 22;
    int n = 1024;
    int Q = 200;
    double s = 1.0;
    std::string outfile = "tee_e2e.csv";
};

static Config parse_args(int argc, char** argv) {
    Config c;
    for (int i = 1; i + 1 < argc; i += 2) {
        std::string k = argv[i], v = argv[i + 1];
        if (k == "--host") c.host = v;
        else if (k == "--port") c.port = static_cast<uint16_t>(std::stoi(v));
        else if (k == "--min_logN") c.min_logN = std::stoi(v);
        else if (k == "--max_logN") c.max_logN = std::stoi(v);
        else if (k == "--n") c.n = std::stoi(v);
        else if (k == "--Q") c.Q = std::stoi(v);
        else if (k == "--s") c.s = std::stod(v);
        else if (k == "--out") c.outfile = v;
    }
    return c;
}

int main(int argc, char** argv) {
    Config cfg = parse_args(argc, argv);

    {
        std::ifstream test(cfg.outfile);
        if (!test.good() || test.peek() == std::ifstream::traits_type::eof()) {
            std::ofstream csv(cfg.outfile);
            csv << "logN,N,n,avg_hot_early_ms,avg_cold_total_ms,avg_all_ms,hit_pct\n";
        }
    }

    for (int logN = cfg.min_logN; logN <= cfg.max_logN; logN += 2) {
        int N = 1 << logN;
        int actual_n = std::min(cfg.n, N / 2);

        std::cout << "\n=== logN=" << logN << " N=" << N
                  << " n=" << actual_n << " ===\n";
        std::cout << "Connecting to " << cfg.host << ":" << cfg.port
                  << "..." << std::endl;

        int fd = connect_with_retry(cfg.host, cfg.port);
        if (fd < 0) {
            std::cerr << "Failed to connect for logN=" << logN << "\n";
            continue;
        }
        std::cout << "Connected.\n";

        ZipfSampler zipf(N, cfg.s, 42);

        bool warmup_ok = true;
        for (int q = 0; q < 20; ++q) {
            try {
                TeeRequest req; req.op = 'R'; req.key = zipf.sample();
                send_msg(fd, serialize_request(req));
                Bytes r1, r2;
                if (!recv_msg(fd, r1) || !recv_msg(fd, r2)) {
                    warmup_ok = false;
                    break;
                }
            } catch (...) {
                warmup_ok = false;
                break;
            }
        }
        if (!warmup_ok) {
            std::cerr << "Warmup failed for logN=" << logN
                      << ", connection broken.\n";
            ::close(fd);
            continue;
        }
        std::cout << "Warmup done. Running " << cfg.Q << " queries...\n";

        double sum_hot_early = 0, sum_cold_total = 0, sum_all_total = 0;
        int hot_cnt = 0, cold_cnt = 0;

        for (int q = 0; q < cfg.Q; ++q) {
            int key = zipf.sample();
            TeeRequest req; req.op = 'R'; req.key = key;

            auto t0 = Clock::now();
            send_msg(fd, serialize_request(req));

            Bytes r1_data;
            recv_msg(fd, r1_data);
            auto t1 = Clock::now();

            Bytes r2_data;
            recv_msg(fd, r2_data);
            auto t2 = Clock::now();

            double early_ms = std::chrono::duration<double, std::milli>(
                t1 - t0).count();
            double total_ms = std::chrono::duration<double, std::milli>(
                t2 - t0).count();

            TeeResponse early = deserialize_response(r1_data);
            bool is_hot = early.found;

            sum_all_total += total_ms;
            if (is_hot) {
                sum_hot_early += early_ms;
                ++hot_cnt;
            } else {
                sum_cold_total += total_ms;
                ++cold_cnt;
            }

            if (q < 3 || q % 50 == 0) {
                std::cout << "  q=" << q << "  early="
                          << std::fixed << std::setprecision(2) << early_ms
                          << "ms  total=" << total_ms << "ms  "
                          << (is_hot ? "HOT" : "cold") << "\n";
            }
        }

        ::close(fd);

        double avg_hot = hot_cnt > 0 ? sum_hot_early / hot_cnt : 0;
        double avg_cold = cold_cnt > 0 ? sum_cold_total / cold_cnt : 0;
        double avg_all = sum_all_total / cfg.Q;
        double hit_pct = 100.0 * hot_cnt / cfg.Q;

        std::cout << "\n--- logN=" << logN << " Results ---\n"
                  << "  hit%=" << std::fixed << std::setprecision(1)
                  << hit_pct << "\n"
                  << "  avg_hot_early=" << std::setprecision(2)
                  << avg_hot << " ms  (n=" << hot_cnt << ")\n"
                  << "  avg_cold_total=" << avg_cold << " ms  (n=" << cold_cnt << ")\n"
                  << "  avg_all_total=" << avg_all << " ms\n";

        {
            std::ofstream out(cfg.outfile, std::ios::app);
            out << logN << "," << N << "," << actual_n << ","
                << std::fixed << std::setprecision(2)
                << avg_hot << "," << avg_cold << "," << avg_all << ","
                << std::setprecision(1) << hit_pct << "\n";
        }

        std::cout << "Saved to " << cfg.outfile << "\n";
        std::this_thread::sleep_for(std::chrono::seconds(3));
    }

    std::cout << "\n=== All done ===\n";
    return 0;
}
