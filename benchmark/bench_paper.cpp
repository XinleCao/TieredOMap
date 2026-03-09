// bench_paper.cpp — All paper experiments in one binary.
// Usage:
//   ./bench_paper --exp=<name> [options]
//
// Experiments:
//   bandwidth   — Bandwidth & rounds vs N, all backends
//   skewness    — Effect of Zipf s on hot-key answer time
//   hotsize     — Effect of hot-set size n
//   latency     — End-to-end latency with simulated RTT
//   dynamic     — Dynamic hot-set hit rate over time
//   backend_cmp — Head-to-head backend comparison
//   all         — Run all of the above sequentially

#include "tiered_omap/tiered_omap.h"
#include "tiered_omap/omap/avl_omap.h"
#include "tiered_omap/omap/bplus_omap.h"
#include "tiered_omap/omap/da_ost_omap.h"
#include "tiered_omap/network/network_storage.h"
#include "tiered_omap/workload.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <vector>

using namespace tiered_omap;
using Clock = std::chrono::high_resolution_clock;

// ═══════════════════════════════════════════════════════════════════════════
// Config
// ═══════════════════════════════════════════════════════════════════════════

struct Cfg {
    std::string exp = "all";
    int Q = 200;
    int warmup = 50;
    int rtt_us = 0;
    double s = 1.0;
    int n = 1024;
    int max_logN = 20;
    int value_size = 256;
    std::string outdir = "results";
    std::string host;     // empty = local storage
    int port = 12345;
    StorageCreator storage_creator;
};

Cfg parse_args(int argc, char** argv) {
    Cfg c;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto eq = a.find('=');
        if (eq == std::string::npos) continue;
        auto k = a.substr(0, eq), v = a.substr(eq + 1);
        if (k == "--exp") c.exp = v;
        else if (k == "--Q") c.Q = std::stoi(v);
        else if (k == "--warmup") c.warmup = std::stoi(v);
        else if (k == "--rtt_us") c.rtt_us = std::stoi(v);
        else if (k == "--rtt_ms") c.rtt_us = static_cast<int>(std::stod(v) * 1000);
        else if (k == "--s") c.s = std::stod(v);
        else if (k == "--n") c.n = std::stoi(v);
        else if (k == "--max_logN") c.max_logN = std::stoi(v);
        else if (k == "--outdir") c.outdir = v;
        else if (k == "--value_size") c.value_size = std::stoi(v);
        else if (k == "--host") c.host = v;
        else if (k == "--port") c.port = std::stoi(v);
    }

    if (!c.host.empty()) {
        auto channel = std::make_shared<TcpChannel>(
            TcpChannel::connect(c.host, c.port));
        c.storage_creator = make_network_creator(channel);
        std::cout << "Connected to ORAM server at "
                  << c.host << ":" << c.port << "\n";
    }

    return c;
}

// ═══════════════════════════════════════════════════════════════════════════
// Helpers
// ═══════════════════════════════════════════════════════════════════════════

static void ensure_dir(const std::string& dir) {
    mkdir(dir.c_str(), 0755);
}

static std::vector<std::pair<int, Bytes>> make_data(int N, int value_size = 256) {
    std::vector<std::pair<int, Bytes>> d;
    d.reserve(N);
    for (int i = 0; i < N; ++i) {
        Bytes v(value_size, 0);
        std::memcpy(v.data(), &i, std::min(sizeof(int), static_cast<size_t>(value_size)));
        d.emplace_back(i, std::move(v));
    }
    return d;
}

static std::vector<int> make_hot_keys(int n) {
    std::vector<int> h(n);
    std::iota(h.begin(), h.end(), 0);
    return h;
}

struct BackendSpec {
    const char* label;
    OmapBackend backend;
};

static const BackendSpec ALL_BACKENDS[] = {
    {"AVL",     OmapBackend::AVL},
    {"BPlus",   OmapBackend::BPlus},
    {"DaAvl",   OmapBackend::DaAvl},
    {"DaBplus", OmapBackend::DaBplus},
};

static std::unique_ptr<OmapInterface> make_standalone(OmapBackend be, int N,
                                                       int bs = 4,
                                                       StorageCreator sc = nullptr) {
    switch (be) {
    case OmapBackend::BPlus:
        return std::make_unique<BPlusOmap>(N, 8, bs, sc);
    case OmapBackend::DaAvl:
        return std::make_unique<DaOstOmap>(N, OdsTreeType::AVL, 0, bs, 8, sc);
    case OmapBackend::DaBplus:
        return std::make_unique<DaOstOmap>(N, OdsTreeType::BPlus, 0, bs, 8, sc);
    default:
        return std::make_unique<AVLOmap>(N, bs, sc);
    }
}

struct RunResult {
    double avg_bw = 0;
    double avg_rounds = 0;
    double avg_hot_rounds = 0;
    double avg_answer_rounds = 0;
    double hit_rate = 0;
    double avg_latency_ms = 0;
};

// ═══════════════════════════════════════════════════════════════════════════
// Exp 1: Bandwidth & Rounds vs N
// ═══════════════════════════════════════════════════════════════════════════

static void exp_bandwidth(const Cfg& cfg) {
    std::cout << "\n=== Exp: Bandwidth & Rounds vs N ===\n";
    ensure_dir(cfg.outdir);
    std::ofstream csv(cfg.outdir + "/bandwidth_vs_N.csv");
    csv << "logN,backend,type,avg_bw_KB,avg_rounds\n";

    for (int logN = 12; logN <= cfg.max_logN; logN += 2) {
        int N = 1 << logN;
        int n = std::min(cfg.n, N / 2);
        auto data = make_data(N, cfg.value_size);
        auto hk = make_hot_keys(n);

        for (auto& [label, be] : ALL_BACKENDS) {
            // Standalone baseline
            {
                auto omap = make_standalone(be, N, 4, cfg.storage_creator);
                omap->init(data);
                ZipfSampler z(N, cfg.s, 42);
                for (int i = 0; i < cfg.warmup; ++i) omap->search(z.sample());
                double bw = 0, rnd = 0;
                for (int i = 0; i < cfg.Q; ++i) {
                    omap->search(z.sample());
                    bw += omap->last_stats().total_bytes();
                    rnd += omap->last_stats().rounds;
                }
                bw /= cfg.Q; rnd /= cfg.Q;
                csv << logN << "," << label << ",standalone,"
                    << std::fixed << std::setprecision(2) << bw / 1024 << ","
                    << std::setprecision(1) << rnd << "\n";
                std::cout << "  logN=" << logN << " " << label
                          << " standalone: " << (int)(bw/1024) << "KB "
                          << (int)rnd << "rnd\n";
            }

            // TieredOMap (FullOblivious)
            {
                TieredOMapConfig tc;
                tc.total_keys = N; tc.hot_set_size = n;
                tc.storage_creator = cfg.storage_creator;
                tc.mode = SecurityMode::FullOblivious;
                tc.backend = be;
                TieredOMap tm(tc); tm.init(data, hk);
                ZipfSampler z(N, cfg.s, 42);
                for (int i = 0; i < cfg.warmup; ++i) tm.access(z.sample());
                double bw = 0, rnd = 0, ans = 0;
                int hot_cnt = 0;
                for (int i = 0; i < cfg.Q; ++i) {
                    auto r = tm.access(z.sample());
                    bw += r.total_bw.total_bytes();
                    rnd += r.total_bw.rounds;
                    ans += r.rounds_to_answer;
                    if (r.found_in_hot) ++hot_cnt;
                }
                bw /= cfg.Q; rnd /= cfg.Q; ans /= cfg.Q;
                csv << logN << "," << label << ",tiered,"
                    << std::fixed << std::setprecision(2) << bw / 1024 << ","
                    << std::setprecision(1) << rnd << "\n";
                std::cout << "  logN=" << logN << " " << label
                          << " tiered: " << (int)(bw/1024) << "KB "
                          << (int)rnd << "rnd  ans=" << std::fixed
                          << std::setprecision(1) << ans
                          << " hit=" << hot_cnt*100/cfg.Q << "%\n";
            }
        }
        std::cout << "\n";
    }
    csv.close();
    std::cout << "  -> " << cfg.outdir << "/bandwidth_vs_N.csv\n";
}

// ═══════════════════════════════════════════════════════════════════════════
// Exp 2: Skewness effect
// ═══════════════════════════════════════════════════════════════════════════

static void exp_skewness(const Cfg& cfg) {
    std::cout << "\n=== Exp: Skewness Effect ===\n";
    ensure_dir(cfg.outdir);
    std::ofstream csv(cfg.outdir + "/skewness.csv");
    csv << "zipf_s,backend,avg_answer_rnd,hit_pct\n";

    int N = 1 << 16;
    int n = cfg.n;
    auto data = make_data(N, cfg.value_size);
    auto hk = make_hot_keys(n);

    for (double s : {0.5, 0.7, 0.9, 1.0, 1.1, 1.3, 1.5}) {
        for (auto& [label, be] : ALL_BACKENDS) {
            TieredOMapConfig tc;
            tc.total_keys = N; tc.hot_set_size = n;
            tc.mode = SecurityMode::FullOblivious;
            tc.backend = be;
            tc.storage_creator = cfg.storage_creator;
            TieredOMap tm(tc); tm.init(data, hk);
            ZipfSampler z(N, s, 42);
            for (int i = 0; i < cfg.warmup; ++i) tm.access(z.sample());
            double ans = 0; int hot = 0;
            for (int i = 0; i < cfg.Q; ++i) {
                auto r = tm.access(z.sample());
                ans += r.rounds_to_answer;
                if (r.found_in_hot) ++hot;
            }
            ans /= cfg.Q;
            double hit = 100.0 * hot / cfg.Q;
            csv << s << "," << label << "," << std::fixed
                << std::setprecision(2) << ans << "," << hit << "\n";
            std::cout << "  s=" << s << " " << label << ": ans_rnd="
                      << std::setprecision(1) << ans << " hit=" << hit << "%\n";
        }
    }
    csv.close();
    std::cout << "  -> " << cfg.outdir << "/skewness.csv\n";
}

// ═══════════════════════════════════════════════════════════════════════════
// Exp 3: Hot-set size effect
// ═══════════════════════════════════════════════════════════════════════════

static void exp_hotsize(const Cfg& cfg) {
    std::cout << "\n=== Exp: Hot-Set Size ===\n";
    ensure_dir(cfg.outdir);
    std::ofstream csv(cfg.outdir + "/hotsize.csv");
    csv << "log_n,backend,avg_answer_rnd,avg_total_bw_KB\n";

    int N = 1 << 16;
    auto data = make_data(N, cfg.value_size);

    for (int log_n : {4, 6, 8, 10, 12}) {
        int n = 1 << log_n;
        auto hk = make_hot_keys(n);
        for (auto& [label, be] : ALL_BACKENDS) {
            TieredOMapConfig tc;
            tc.total_keys = N; tc.hot_set_size = n;
            tc.mode = SecurityMode::FullOblivious;
            tc.backend = be;
            tc.storage_creator = cfg.storage_creator;
            TieredOMap tm(tc); tm.init(data, hk);
            ZipfSampler z(N, cfg.s, 42);
            for (int i = 0; i < cfg.warmup; ++i) tm.access(z.sample());
            double ans = 0, bw = 0;
            for (int i = 0; i < cfg.Q; ++i) {
                auto r = tm.access(z.sample());
                ans += r.rounds_to_answer;
                bw += r.total_bw.total_bytes();
            }
            ans /= cfg.Q; bw /= cfg.Q;
            csv << log_n << "," << label << ","
                << std::fixed << std::setprecision(2) << ans << ","
                << bw / 1024 << "\n";
            std::cout << "  n=2^" << log_n << " " << label << ": ans_rnd="
                      << std::setprecision(1) << ans << " bw="
                      << (int)(bw/1024) << "KB\n";
        }
    }
    csv.close();
    std::cout << "  -> " << cfg.outdir << "/hotsize.csv\n";
}

// ═══════════════════════════════════════════════════════════════════════════
// Exp 4: Latency with simulated RTT
// ═══════════════════════════════════════════════════════════════════════════

static void exp_latency(const Cfg& cfg) {
    std::cout << "\n=== Exp: Latency (analytical: rounds × RTT) ===\n";
    ensure_dir(cfg.outdir);
    std::ofstream csv(cfg.outdir + "/latency.csv");
    csv << "rtt_ms,backend,type,avg_rounds,avg_answer_rnd,latency_ms,answer_latency_ms\n";

    int N = 1 << 16;
    int n = cfg.n;
    auto data = make_data(N, cfg.value_size);
    auto hk = make_hot_keys(n);

    // Measure rounds without sleep, then compute latency = rounds × RTT
    for (auto& [label, be] : ALL_BACKENDS) {
        // Standalone: measure avg rounds
        double bl_rnd;
        {
            auto omap = make_standalone(be, N, 4, cfg.storage_creator);
            omap->init(data);
            ZipfSampler z(N, cfg.s, 42);
            for (int i = 0; i < cfg.warmup; ++i) omap->search(z.sample());
            double rnd = 0;
            for (int i = 0; i < cfg.Q; ++i) {
                omap->search(z.sample());
                rnd += omap->last_stats().rounds;
            }
            bl_rnd = rnd / cfg.Q;
        }

        // TieredOMap: measure total rounds and answer rounds
        double tm_rnd, tm_ans;
        {
            TieredOMapConfig tc;
            tc.total_keys = N; tc.hot_set_size = n;
            tc.mode = SecurityMode::FullOblivious;
            tc.backend = be;
            tc.storage_creator = cfg.storage_creator;
            TieredOMap tm(tc); tm.init(data, hk);
            ZipfSampler z(N, cfg.s, 42);
            for (int i = 0; i < cfg.warmup; ++i) tm.access(z.sample());
            double rnd = 0, ans = 0;
            for (int i = 0; i < cfg.Q; ++i) {
                auto r = tm.access(z.sample());
                rnd += r.total_bw.rounds;
                ans += r.rounds_to_answer;
            }
            tm_rnd = rnd / cfg.Q;
            tm_ans = ans / cfg.Q;
        }

        for (double rtt_ms : {0.1, 1.0, 5.0, 10.0, 50.0, 100.0}) {
            double bl_lat = bl_rnd * rtt_ms;
            double tm_lat = tm_rnd * rtt_ms;
            double ans_lat = tm_ans * rtt_ms;

            csv << std::fixed << std::setprecision(1)
                << rtt_ms << "," << label << ",standalone,"
                << bl_rnd << ",," << std::setprecision(2) << bl_lat << ",\n";
            csv << std::fixed << std::setprecision(1)
                << rtt_ms << "," << label << ",tiered,"
                << tm_rnd << "," << tm_ans << ","
                << std::setprecision(2) << tm_lat << "," << ans_lat << "\n";
        }
        std::cout << "  " << label << ": standalone=" << std::fixed
                  << std::setprecision(1) << bl_rnd << "rnd  tiered="
                  << tm_rnd << "rnd  answer=" << tm_ans << "rnd\n";
    }
    csv.close();
    std::cout << "  -> " << cfg.outdir << "/latency.csv\n";
}

// ═══════════════════════════════════════════════════════════════════════════
// Exp 5: Dynamic hot-set hit rate
// ═══════════════════════════════════════════════════════════════════════════

static void exp_dynamic(const Cfg& cfg) {
    std::cout << "\n=== Exp: Dynamic Hot-Set Maintenance ===\n";
    ensure_dir(cfg.outdir);
    std::ofstream csv(cfg.outdir + "/dynamic.csv");
    csv << "query_idx,backend,hit_pct,epoch\n";

    int N = 512;
    int n = 64;
    int total = 3000;
    int window = 100;

    auto data = make_data(N, cfg.value_size);
    auto hk = make_hot_keys(n);

    static const BackendSpec DYN_BACKENDS[] = {
        {"AVL",    OmapBackend::AVL},
        {"BPlus",  OmapBackend::BPlus},
        {"DaAvl",  OmapBackend::DaAvl},
        {"DaBplus",OmapBackend::DaBplus},
    };

    for (auto& [label, be] : DYN_BACKENDS) {
        bool is_da = (be == OmapBackend::DaAvl || be == OmapBackend::DaBplus);
        TieredOMapConfig tc;
        tc.total_keys = N; tc.hot_set_size = n;
        tc.mode = SecurityMode::FullOblivious;
        tc.backend = be;
        tc.storage_creator = cfg.storage_creator;
        tc.maintenance.enabled = true;
        tc.maintenance.epoch_length = 128;
        tc.maintenance.promote_threshold = 5;
        tc.maintenance.demote_threshold = 2;
        tc.maintenance.staleness_epochs = 3;
        tc.maintenance.piggyback = is_da;
        TieredOMap tm(tc); tm.init(data, hk);
        ZipfSampler z(N, cfg.s, 42);

        std::vector<int> hits;
        for (int q = 0; q < total; ++q) {
            auto r = tm.access(z.sample());
            hits.push_back(r.found_in_hot ? 1 : 0);
            if ((q + 1) % window == 0) {
                int start = q + 1 - window;
                int sum = 0;
                for (int j = start; j <= q; ++j) sum += hits[j];
                double pct = 100.0 * sum / window;
                int ep = tm.maintenance_mgr()
                             ? tm.maintenance_mgr()->current_epoch() : 0;
                csv << q + 1 << "," << label << "," << std::fixed
                    << std::setprecision(1) << pct << "," << ep << "\n";
            }
        }
        std::cout << "  " << label << " done (" << total << " queries)\n";
    }
    csv.close();
    std::cout << "  -> " << cfg.outdir << "/dynamic.csv\n";
}

// ═══════════════════════════════════════════════════════════════════════════
// Exp 6: Backend comparison — standalone OMAP performance
// ═══════════════════════════════════════════════════════════════════════════

static void exp_backend_cmp(const Cfg& cfg) {
    std::cout << "\n=== Exp: Backend Comparison (standalone OMAP) ===\n";
    ensure_dir(cfg.outdir);
    std::ofstream csv(cfg.outdir + "/backend_cmp.csv");
    csv << "logN,backend,avg_bw_KB,avg_rounds,avg_comp_us\n";

    for (int logN = 12; logN <= cfg.max_logN; logN += 2) {
        int N = 1 << logN;
        auto data = make_data(N, cfg.value_size);
        for (auto& [label, be] : ALL_BACKENDS) {
            auto omap = make_standalone(be, N, 4, cfg.storage_creator);
            omap->init(data);
            ZipfSampler z(N, cfg.s, 42);
            for (int i = 0; i < cfg.warmup; ++i) omap->search(z.sample());
            double bw = 0, rnd = 0, us = 0;
            for (int i = 0; i < cfg.Q; ++i) {
                int k = z.sample();
                auto t0 = Clock::now();
                omap->search(k);
                us += std::chrono::duration<double, std::micro>(
                    Clock::now() - t0).count();
                bw += omap->last_stats().total_bytes();
                rnd += omap->last_stats().rounds;
            }
            bw /= cfg.Q; rnd /= cfg.Q; us /= cfg.Q;
            csv << logN << "," << label << "," << std::fixed
                << std::setprecision(2) << bw / 1024 << ","
                << std::setprecision(1) << rnd << ","
                << std::setprecision(0) << us << "\n";
            std::cout << "  logN=" << logN << " " << label << ": "
                      << (int)(bw/1024) << "KB " << (int)rnd << "rnd "
                      << (int)us << "us\n";
        }
    }
    csv.close();
    std::cout << "  -> " << cfg.outdir << "/backend_cmp.csv\n";
}

// ═══════════════════════════════════════════════════════════════════════════
// Exp 7: Security mode comparison (FO vs TM vs TM+Split)
// ═══════════════════════════════════════════════════════════════════════════

static void exp_modes(const Cfg& cfg) {
    std::cout << "\n=== Exp: Security Mode Comparison ===\n";
    ensure_dir(cfg.outdir);
    std::ofstream csv(cfg.outdir + "/modes.csv");
    csv << "logN,backend,mode,avg_bw_KB,avg_rounds,avg_answer_rnd,hit_pct\n";

    struct ModeSpec {
        const char* label;
        SecurityMode mode;
        bool split;
    };
    static const ModeSpec MODES[] = {
        {"FO",       SecurityMode::FullOblivious,  false},
        {"TM",       SecurityMode::TierMembership, false},
        {"TM+Split", SecurityMode::TierMembership, true},
    };

    for (int logN = 14; logN <= cfg.max_logN; logN += 2) {
        int N = 1 << logN;
        int n = std::min(cfg.n, N / 2);
        auto data = make_data(N, cfg.value_size);
        auto hk = make_hot_keys(n);

        for (auto& [be_label, be] : ALL_BACKENDS) {
            for (auto& [m_label, mode, split] : MODES) {
                TieredOMapConfig tc;
                tc.total_keys = N; tc.hot_set_size = n;
                tc.mode = mode; tc.use_split_oram = split;
                tc.backend = be;
                tc.storage_creator = cfg.storage_creator;
                TieredOMap tm(tc); tm.init(data, hk);
                ZipfSampler z(N, cfg.s, 42);
                for (int i = 0; i < cfg.warmup; ++i) tm.access(z.sample());
                double bw = 0, rnd = 0, ans = 0;
                int hot = 0;
                for (int i = 0; i < cfg.Q; ++i) {
                    auto r = tm.access(z.sample());
                    bw += r.total_bw.total_bytes();
                    rnd += r.total_bw.rounds;
                    ans += r.rounds_to_answer;
                    if (r.found_in_hot) ++hot;
                }
                bw /= cfg.Q; rnd /= cfg.Q; ans /= cfg.Q;
                double hit = 100.0 * hot / cfg.Q;
                csv << logN << "," << be_label << "," << m_label << ","
                    << std::fixed << std::setprecision(2) << bw / 1024 << ","
                    << std::setprecision(1) << rnd << "," << ans << ","
                    << hit << "\n";
                std::cout << "  logN=" << logN << " " << be_label << " " << m_label
                          << ": " << (int)(bw/1024) << "KB " << (int)rnd
                          << "rnd ans=" << std::fixed << std::setprecision(1)
                          << ans << "\n";
            }
        }
        std::cout << "\n";
    }
    csv.close();
    std::cout << "  -> " << cfg.outdir << "/modes.csv\n";
}

// ═══════════════════════════════════════════════════════════════════════════
// Exp 8: Write overhead (search vs update vs insert)
// ═══════════════════════════════════════════════════════════════════════════

static void exp_write(const Cfg& cfg) {
    std::cout << "\n=== Exp: Write Overhead ===\n";
    ensure_dir(cfg.outdir);
    std::ofstream csv(cfg.outdir + "/write_overhead.csv");
    csv << "backend,op,avg_bw_KB,avg_rounds\n";

    int N = 1 << 16;
    auto data = make_data(N, cfg.value_size);

    for (auto& [label, be] : ALL_BACKENDS) {
        // Search
        {
            auto omap = make_standalone(be, N, 4, cfg.storage_creator);
            omap->init(data);
            ZipfSampler z(N, cfg.s, 42);
            for (int i = 0; i < cfg.warmup; ++i) omap->search(z.sample());
            double bw = 0, rnd = 0;
            for (int i = 0; i < cfg.Q; ++i) {
                omap->search(z.sample());
                bw += omap->last_stats().total_bytes();
                rnd += omap->last_stats().rounds;
            }
            csv << label << ",search," << std::fixed << std::setprecision(2)
                << bw / cfg.Q / 1024 << "," << std::setprecision(1)
                << rnd / cfg.Q << "\n";
            std::cout << "  " << label << " search: " << (int)(bw/cfg.Q/1024)
                      << "KB " << (int)(rnd/cfg.Q) << "rnd\n";
        }

        // Update (search with new value)
        {
            auto omap = make_standalone(be, N, 4, cfg.storage_creator);
            omap->init(data);
            ZipfSampler z(N, cfg.s, 42);
            Bytes val(cfg.value_size, 0);
            int marker = 999;
            std::memcpy(val.data(), &marker, std::min(sizeof(int), static_cast<size_t>(cfg.value_size)));
            for (int i = 0; i < cfg.warmup; ++i) omap->search(z.sample(), &val);
            double bw = 0, rnd = 0;
            for (int i = 0; i < cfg.Q; ++i) {
                omap->search(z.sample(), &val);
                bw += omap->last_stats().total_bytes();
                rnd += omap->last_stats().rounds;
            }
            csv << label << ",update," << std::fixed << std::setprecision(2)
                << bw / cfg.Q / 1024 << "," << std::setprecision(1)
                << rnd / cfg.Q << "\n";
            std::cout << "  " << label << " update: " << (int)(bw/cfg.Q/1024)
                      << "KB " << (int)(rnd/cfg.Q) << "rnd\n";
        }

        // Insert (add new keys beyond N)
        {
            auto omap = make_standalone(be, N + cfg.Q + cfg.warmup, 4,
                                          cfg.storage_creator);
            omap->init(data);
            double bw = 0, rnd = 0;
            auto make_val = [&](int id) {
                Bytes v(cfg.value_size, 0);
                std::memcpy(v.data(), &id, std::min(sizeof(int), static_cast<size_t>(cfg.value_size)));
                return v;
            };
            for (int i = 0; i < cfg.warmup; ++i) {
                omap->insert(N + i, make_val(N + i));
            }
            for (int i = 0; i < cfg.Q; ++i) {
                omap->insert(N + cfg.warmup + i, make_val(N + cfg.warmup + i));
                bw += omap->last_stats().total_bytes();
                rnd += omap->last_stats().rounds;
            }
            csv << label << ",insert," << std::fixed << std::setprecision(2)
                << bw / cfg.Q / 1024 << "," << std::setprecision(1)
                << rnd / cfg.Q << "\n";
            std::cout << "  " << label << " insert: " << (int)(bw/cfg.Q/1024)
                      << "KB " << (int)(rnd/cfg.Q) << "rnd\n";
        }
    }
    csv.close();
    std::cout << "  -> " << cfg.outdir << "/write_overhead.csv\n";
}

// ═══════════════════════════════════════════════════════════════════════════
// Exp 9: Workload drift — hot items shift mid-run
// ═══════════════════════════════════════════════════════════════════════════
// Exp 10: YCSB workload distributions (Zipfian / Uniform / Latest)
// ═══════════════════════════════════════════════════════════════════════════

static void exp_workload(const Cfg& cfg) {
    std::cout << "\n=== Exp: YCSB Workload Distributions ===\n";
    ensure_dir(cfg.outdir);
    std::ofstream csv(cfg.outdir + "/workload_dist.csv");
    csv << "distribution,backend,avg_bw_kb,avg_answer_rnd,hit_pct\n";

    int N = 1 << 16;
    int n = cfg.n;
    auto data = make_data(N, cfg.value_size);

    // Optimal hot set per distribution:
    //   Zipfian  → top-n popular keys [0, n)
    //   Uniform  → any n keys (no natural hot set); use [0, n)
    //   Latest   → most recent n keys [N-n, N)
    auto hk_zipf = make_hot_keys(n);
    std::vector<int> hk_latest;
    for (int i = N - n; i < N; ++i) hk_latest.push_back(i);

    struct DistSpec { const char* name; int id; };
    static const DistSpec DISTS[] = {
        {"Zipfian", 0}, {"Uniform", 1}, {"Latest", 2},
    };

    for (const auto& ds : DISTS) {
        const auto& hot_keys = (ds.id == 2) ? hk_latest : hk_zipf;
        for (auto& [label, be] : ALL_BACKENDS) {
            TieredOMapConfig tc;
            tc.total_keys = N; tc.hot_set_size = n;
            tc.mode = SecurityMode::FullOblivious;
            tc.backend = be;
            tc.storage_creator = cfg.storage_creator;
            TieredOMap tm(tc); tm.init(data, hot_keys);

            ZipfSampler     z_zipf(N, cfg.s, 42);
            UniformSampler  z_unif(N, 42);
            LatestSampler   z_latest(N, 0.99, 42);

            int dist_id = ds.id;
            auto gen = [&]() -> int {
                switch (dist_id) {
                case 0: return z_zipf.sample();
                case 1: return z_unif.sample();
                default: return z_latest.sample();
                }
            };

            for (int i = 0; i < cfg.warmup; ++i) tm.access(gen());
            double bw = 0, ans = 0; int hot = 0;
            for (int i = 0; i < cfg.Q; ++i) {
                auto r = tm.access(gen());
                bw += r.total_bw.bytes_downloaded + r.total_bw.bytes_uploaded;
                ans += r.rounds_to_answer;
                if (r.found_in_hot) ++hot;
            }
            bw /= cfg.Q * 1024.0;
            ans /= cfg.Q;
            double hit = 100.0 * hot / cfg.Q;
            csv << ds.name << "," << label << "," << std::fixed
                << std::setprecision(1) << bw << "," << ans << ","
                << hit << "\n";
            std::cout << "  " << ds.name << " " << label << ": "
                      << bw << "KB " << ans << "rnd hit=" << hit << "%\n";
        }
    }
    csv.close();
    std::cout << "  -> " << cfg.outdir << "/workload_dist.csv\n";
}

// ═══════════════════════════════════════════════════════════════════════════

static void exp_drift(const Cfg& cfg) {
    std::cout << "\n=== Exp: Workload Drift ===\n";
    ensure_dir(cfg.outdir);
    std::ofstream csv(cfg.outdir + "/drift.csv");
    csv << "query_idx,backend,scenario,hit_pct\n";

    int N = 512;
    int n = 64;
    int total = 5000;
    int shift_at = 2000;
    int window = 100;

    auto data = make_data(N, cfg.value_size);
    auto hk = make_hot_keys(n);

    static const BackendSpec DRIFT_BACKENDS[] = {
        {"AVL",    OmapBackend::AVL},
        {"BPlus",  OmapBackend::BPlus},
        {"DaAvl",  OmapBackend::DaAvl},
        {"DaBplus",OmapBackend::DaBplus},
    };

    for (auto& [label, be] : DRIFT_BACKENDS) {
        bool is_da = (be == OmapBackend::DaAvl || be == OmapBackend::DaBplus);
        // Scenario 1: No maintenance (static hot set)
        {
            TieredOMapConfig tc;
            tc.total_keys = N; tc.hot_set_size = n;
            tc.mode = SecurityMode::FullOblivious;
            tc.backend = be;
            tc.storage_creator = cfg.storage_creator;
            TieredOMap tm(tc); tm.init(data, hk);

            std::vector<int> hits;
            for (int q = 0; q < total; ++q) {
                int shift = (q >= shift_at) ? n : 0;
                ZipfSampler z(N, cfg.s, 42 + q);
                int key = (z.sample() + shift) % N;
                auto r = tm.access(key);
                hits.push_back(r.found_in_hot ? 1 : 0);
                if ((q + 1) % window == 0) {
                    int start = q + 1 - window;
                    int sum = 0;
                    for (int j = start; j <= q; ++j) sum += hits[j];
                    csv << q + 1 << "," << label << ",no_maint,"
                        << std::fixed << std::setprecision(1)
                        << 100.0 * sum / window << "\n";
                }
            }
        }

        // Scenario 2: With maintenance (adaptive)
        {
            TieredOMapConfig tc;
            tc.total_keys = N; tc.hot_set_size = n;
            tc.mode = SecurityMode::FullOblivious;
            tc.backend = be;
            tc.storage_creator = cfg.storage_creator;
            tc.maintenance.enabled = true;
            tc.maintenance.epoch_length = 128;
            tc.maintenance.promote_threshold = 5;
            tc.maintenance.demote_threshold = 2;
            tc.maintenance.staleness_epochs = 3;
            tc.maintenance.piggyback = is_da;
            TieredOMap tm(tc); tm.init(data, hk);

            std::vector<int> hits;
            for (int q = 0; q < total; ++q) {
                int shift = (q >= shift_at) ? n : 0;
                ZipfSampler z(N, cfg.s, 42 + q);
                int key = (z.sample() + shift) % N;
                auto r = tm.access(key);
                hits.push_back(r.found_in_hot ? 1 : 0);
                if ((q + 1) % window == 0) {
                    int start = q + 1 - window;
                    int sum = 0;
                    for (int j = start; j <= q; ++j) sum += hits[j];
                    csv << q + 1 << "," << label << ",with_maint,"
                        << std::fixed << std::setprecision(1)
                        << 100.0 * sum / window << "\n";
                }
            }
        }
        std::cout << "  " << label << " done\n";
    }
    csv.close();
    std::cout << "  -> " << cfg.outdir << "/drift.csv\n";
}

// ═══════════════════════════════════════════════════════════════════════════
// Exp: Throughput (ops/sec) vs N — all backends, standalone vs tiered
// ═══════════════════════════════════════════════════════════════════════════

static void exp_throughput(const Cfg& cfg) {
    std::cout << "\n=== Exp: Throughput (ops/sec) vs N ===\n";
    ensure_dir(cfg.outdir);
    std::ofstream csv(cfg.outdir + "/throughput.csv");
    csv << "logN,backend,type,ops_per_sec,avg_us_per_op\n";

    for (int logN = 12; logN <= cfg.max_logN; logN += 2) {
        int N = 1 << logN;
        int n = std::min(cfg.n, N / 2);
        auto data = make_data(N, cfg.value_size);
        auto hk = make_hot_keys(n);

        for (auto& [label, be] : ALL_BACKENDS) {
            // Standalone
            {
                auto omap = make_standalone(be, N, 4, cfg.storage_creator);
                omap->init(data);
                ZipfSampler z(N, cfg.s, 42);
                for (int i = 0; i < cfg.warmup; ++i) omap->search(z.sample());
                auto t0 = Clock::now();
                for (int i = 0; i < cfg.Q; ++i) omap->search(z.sample());
                double elapsed_us = std::chrono::duration<double, std::micro>(
                    Clock::now() - t0).count();
                double us_per_op = elapsed_us / cfg.Q;
                double ops_sec = 1e6 / us_per_op;
                csv << logN << "," << label << ",standalone,"
                    << std::fixed << std::setprecision(1) << ops_sec << ","
                    << std::setprecision(0) << us_per_op << "\n";
                std::cout << "  logN=" << logN << " " << label
                          << " standalone: " << (int)ops_sec << " ops/s ("
                          << (int)us_per_op << " us)\n";
            }

            // Tiered (TierMembership + split)
            {
                TieredOMapConfig tc;
                tc.total_keys = N; tc.hot_set_size = n;
                tc.mode = SecurityMode::TierMembership;
                tc.use_split_oram = true;
                tc.backend = be;
                tc.storage_creator = cfg.storage_creator;
                TieredOMap tm(tc); tm.init(data, hk);
                ZipfSampler z(N, cfg.s, 42);
                for (int i = 0; i < cfg.warmup; ++i) tm.access(z.sample());
                auto t0 = Clock::now();
                for (int i = 0; i < cfg.Q; ++i) tm.access(z.sample());
                double elapsed_us = std::chrono::duration<double, std::micro>(
                    Clock::now() - t0).count();
                double us_per_op = elapsed_us / cfg.Q;
                double ops_sec = 1e6 / us_per_op;
                csv << logN << "," << label << ",tiered_tm,"
                    << std::fixed << std::setprecision(1) << ops_sec << ","
                    << std::setprecision(0) << us_per_op << "\n";
                std::cout << "  logN=" << logN << " " << label
                          << " tiered_tm: " << (int)ops_sec << " ops/s ("
                          << (int)us_per_op << " us)\n";
            }
        }
        std::cout << "\n";
    }
    csv.close();
    std::cout << "  -> " << cfg.outdir << "/throughput.csv\n";
}

// ═══════════════════════════════════════════════════════════════════════════
// Exp: Backend comparison — same N, all backends, within tiered framework
// ═══════════════════════════════════════════════════════════════════════════

static void exp_tiered_backend(const Cfg& cfg) {
    std::cout << "\n=== Exp: Tiered Backend Comparison ===\n";
    ensure_dir(cfg.outdir);
    std::ofstream csv(cfg.outdir + "/tiered_backend.csv");
    csv << "logN,backend,avg_bw_KB,avg_rounds,avg_answer_rnd,hit_pct,avg_us\n";

    for (int logN = 12; logN <= cfg.max_logN; logN += 2) {
        int N = 1 << logN;
        int n = std::min(cfg.n, N / 2);
        auto data = make_data(N, cfg.value_size);
        auto hk = make_hot_keys(n);

        for (auto& [label, be] : ALL_BACKENDS) {
            TieredOMapConfig tc;
            tc.total_keys = N; tc.hot_set_size = n;
            tc.mode = SecurityMode::TierMembership;
            tc.use_split_oram = true;
            tc.backend = be;
            tc.storage_creator = cfg.storage_creator;
            TieredOMap tm(tc); tm.init(data, hk);
            ZipfSampler z(N, cfg.s, 42);
            for (int i = 0; i < cfg.warmup; ++i) tm.access(z.sample());
            double bw = 0, rnd = 0, ans = 0, us = 0;
            int hot = 0;
            for (int i = 0; i < cfg.Q; ++i) {
                auto t0 = Clock::now();
                auto r = tm.access(z.sample());
                us += std::chrono::duration<double, std::micro>(
                    Clock::now() - t0).count();
                bw += r.total_bw.total_bytes();
                rnd += r.total_bw.rounds;
                ans += r.rounds_to_answer;
                if (r.found_in_hot) ++hot;
            }
            bw /= cfg.Q; rnd /= cfg.Q; ans /= cfg.Q; us /= cfg.Q;
            double hit = 100.0 * hot / cfg.Q;
            csv << logN << "," << label << "," << std::fixed
                << std::setprecision(2) << bw / 1024 << ","
                << std::setprecision(1) << rnd << "," << ans << ","
                << hit << "," << std::setprecision(0) << us << "\n";
            std::cout << "  logN=" << logN << " " << label << ": "
                      << (int)(bw/1024) << "KB " << (int)rnd << "rnd ans="
                      << std::fixed << std::setprecision(1) << ans
                      << " hit=" << hit << "% " << (int)us << "us\n";
        }
        std::cout << "\n";
    }
    csv.close();
    std::cout << "  -> " << cfg.outdir << "/tiered_backend.csv\n";
}

// ═══════════════════════════════════════════════════════════════════════════
// Exp: Value-size sensitivity
// ═══════════════════════════════════════════════════════════════════════════

static void exp_valuesize(const Cfg& cfg) {
    std::cout << "\n=== Exp: Value-Size Sensitivity ===\n";
    ensure_dir(cfg.outdir);
    std::ofstream csv(cfg.outdir + "/valuesize.csv");
    csv << "value_bytes,type,avg_bw_KB,avg_rounds,avg_answer_rnd\n";

    int N = 1 << 16;
    int n = cfg.n;

    for (int vs : {64, 256, 1024, 4096}) {
        auto data = make_data(N, vs);
        auto hk = make_hot_keys(n);

        // Standalone baseline (AVL)
        {
            AVLOmap bl(N);
            bl.init(data);
            ZipfSampler z(N, cfg.s, 42);
            for (int i = 0; i < cfg.warmup; ++i) bl.search(z.sample());
            double bw = 0, rnd = 0;
            for (int i = 0; i < cfg.Q; ++i) {
                bl.search(z.sample());
                bw += bl.last_stats().total_bytes();
                rnd += bl.last_stats().rounds;
            }
            bw /= cfg.Q; rnd /= cfg.Q;
            csv << vs << ",standalone,"
                << std::fixed << std::setprecision(2) << bw / 1024 << ","
                << std::setprecision(1) << rnd << ",\n";
            std::cout << "  val=" << vs << "B standalone: "
                      << (int)(bw/1024) << "KB " << (int)rnd << "rnd\n";
        }

        // Tiered (TM + split)
        {
            TieredOMapConfig tc;
            tc.total_keys = N; tc.hot_set_size = n;
            tc.mode = SecurityMode::TierMembership;
            tc.use_split_oram = true;
            tc.storage_creator = cfg.storage_creator;
            TieredOMap tm(tc); tm.init(data, hk);
            ZipfSampler z(N, cfg.s, 42);
            for (int i = 0; i < cfg.warmup; ++i) tm.access(z.sample());
            double bw = 0, rnd = 0, ans = 0;
            for (int i = 0; i < cfg.Q; ++i) {
                auto r = tm.access(z.sample());
                bw += r.total_bw.total_bytes();
                rnd += r.total_bw.rounds;
                ans += r.rounds_to_answer;
            }
            bw /= cfg.Q; rnd /= cfg.Q; ans /= cfg.Q;
            csv << vs << ",tiered_tm,"
                << std::fixed << std::setprecision(2) << bw / 1024 << ","
                << std::setprecision(1) << rnd << "," << ans << "\n";
            std::cout << "  val=" << vs << "B tiered: "
                      << (int)(bw/1024) << "KB " << (int)rnd << "rnd ans="
                      << std::setprecision(1) << ans << "\n";
        }
    }
    csv.close();
    std::cout << "  -> " << cfg.outdir << "/valuesize.csv\n";
}

// ═══════════════════════════════════════════════════════════════════════════
// Main
// ═══════════════════════════════════════════════════════════════════════════

int main(int argc, char** argv) {
    Cfg cfg = parse_args(argc, argv);
    ensure_dir(cfg.outdir);

    std::cout << "TieredOMap Paper Experiments\n"
              << "  exp=" << cfg.exp << "  Q=" << cfg.Q
              << "  s=" << cfg.s << "  n=" << cfg.n
              << "  max_logN=" << cfg.max_logN
              << "  value_size=" << cfg.value_size << "\n";

    bool all = (cfg.exp == "all");
    if (all || cfg.exp == "bandwidth")   exp_bandwidth(cfg);
    if (all || cfg.exp == "skewness")    exp_skewness(cfg);
    if (all || cfg.exp == "hotsize")     exp_hotsize(cfg);
    if (all || cfg.exp == "latency")     exp_latency(cfg);
    if (all || cfg.exp == "dynamic")     exp_dynamic(cfg);
    if (all || cfg.exp == "backend_cmp") exp_backend_cmp(cfg);
    if (all || cfg.exp == "modes")       exp_modes(cfg);
    if (all || cfg.exp == "write")       exp_write(cfg);
    if (all || cfg.exp == "workload")       exp_workload(cfg);
    if (all || cfg.exp == "drift")          exp_drift(cfg);
    if (all || cfg.exp == "throughput")     exp_throughput(cfg);
    if (all || cfg.exp == "tiered_backend") exp_tiered_backend(cfg);
    if (all || cfg.exp == "valuesize")      exp_valuesize(cfg);

    std::cout << "\nAll done. CSV files in: " << cfg.outdir << "/\n";
    return 0;
}
