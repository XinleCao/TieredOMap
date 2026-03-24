// bench_paper.cpp — All paper experiments in one binary.
// Usage:
//   ./bench_paper --exp=<name> [options]
//
// Experiments (paper-aligned):
//   bandwidth       — Bandwidth & rounds vs N (AVL, Single vs TM+split)
//   modes           — Mode comparison: Single / TM+split / FO+split
//   backend_cmp     — Standalone OMAP: all 4 backends
//   tiered_backend  — All backends inside tiered (TM+split)
//   latency         — End-to-end latency (TM+split, all backends)
//   throughput      — Throughput vs N (AVL + DA-B+)
//   skewness        — Effect of Zipf s (AVL, TM+split)
//   hotsize         — Effect of hot-set size n (AVL, FO+split)
//   split_ablation  — Split-ORAM ablation: TM vs TM+split
//   dynamic         — Dynamic hot-set convergence
//   drift           — Workload drift adaptation
//   wan_static      — WAN validation: static hot set
//   wan_dynamic     — WAN validation: dynamic maintenance
//   all             — Run all of the above sequentially

#include "tiered_omap/tiered_omap.h"
#include "tiered_omap/omap/avl_omap.h"
#include "tiered_omap/omap/bplus_omap.h"
#include "tiered_omap/omap/da_ost_omap.h"
#include "tiered_omap/network/network_storage.h"
#include "tiered_omap/network/tcp_channel.h"
#include "tiered_omap/bench_setup.h"
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
    std::string host;
    int port = 12345;
    std::string backend_filter;
    StorageCreator storage_creator;
    std::shared_ptr<TcpChannel> channel;
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
        else if (k == "--backend") c.backend_filter = v;
    }

    if (!c.host.empty()) {
        c.channel = std::make_shared<TcpChannel>(
            TcpChannel::connect(c.host, c.port));
        c.storage_creator = make_network_creator(c.channel);
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
    if (N >= (1 << 18))
        std::cout << "    [init] generating " << N << " entries..." << std::flush;
    std::vector<std::pair<int, Bytes>> d;
    d.reserve(N);
    for (int i = 0; i < N; ++i) {
        Bytes v(value_size, 0);
        std::memcpy(v.data(), &i, std::min(sizeof(int), static_cast<size_t>(value_size)));
        d.emplace_back(i, std::move(v));
    }
    if (N >= (1 << 18)) std::cout << " done\n";
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
                                                       StorageCreator sc = nullptr,
                                                       bool index_mode = false) {
    switch (be) {
    case OmapBackend::BPlus: {
        auto bp = std::make_unique<BPlusOmap>(N, 8, bs, sc);
        if (index_mode) bp->set_index_mode(true);
        return bp;
    }
    case OmapBackend::DaAvl:
        return std::make_unique<DaOstOmap>(N, OdsTreeType::AVL, 0, bs, 8, sc);
    case OmapBackend::DaBplus:
        return std::make_unique<DaOstOmap>(N, OdsTreeType::BPlus, 0, bs, 8, sc);
    default:
        return std::make_unique<AVLOmap>(N, bs, sc);
    }
}

// Fair baseline: index OMAP (4B refs) + data ORAM (full values).
// Same index/data separation as TieredOMap, but single-tier.
struct IndexDataOmap {
    std::unique_ptr<OmapInterface> index;
    PathORAM data;
    BandwidthStats last_bw;

    void init_local(OmapBackend be, int N, int value_size,
                    StorageCreator sc = nullptr) {
        data = PathORAM(N, 4, 7, sc);
        std::unordered_map<int, Bytes> dmap;
        for (int i = 0; i < N; ++i) {
            Bytes v(value_size, 0);
            std::memcpy(v.data(), &i,
                        std::min(sizeof(int), static_cast<size_t>(value_size)));
            dmap[i] = std::move(v);
        }
        data.init(dmap);

        index = make_standalone(be, N, 4, sc, /*index_mode=*/true);
        std::vector<std::pair<int, Bytes>> idata;
        idata.reserve(N);
        for (int i = 0; i < N; ++i)
            idata.emplace_back(i, int_to_bytes(i));
        index->init(idata);
    }

    Bytes search(int key, const Bytes* new_value = nullptr) {
        Bytes ref = index->search(key);
        auto ibw = index->last_stats();
        int blk = ref.empty() ? -1 : bytes_to_int(ref);
        Bytes val;
        if (blk >= 0)
            val = data.access(blk, new_value);
        else
            data.dummy_access();
        auto dbw = data.last_stats();
        last_bw.bytes_downloaded = ibw.bytes_downloaded + dbw.bytes_downloaded;
        last_bw.bytes_uploaded = ibw.bytes_uploaded + dbw.bytes_uploaded;
        last_bw.rounds = ibw.rounds + dbw.rounds;
        return val;
    }

    void set_round_delay_us(int us) {
        index->set_round_delay_us(us);
        data.set_round_delay_us(us);
    }

    double total_bytes() const { return last_bw.total_bytes(); }
    double rounds() const { return last_bw.rounds; }
};

static IndexDataOmap setup_fair_baseline(const Cfg& cfg, OmapBackend be,
                                         int N, int vs = 0) {
    int value_size = vs > 0 ? vs : cfg.value_size;
    IndexDataOmap ido;
    ido.init_local(be, N, value_size, cfg.storage_creator);
    return ido;
}

static std::unique_ptr<OmapInterface> setup_standalone(
    const Cfg& cfg, OmapBackend be, int capacity, int bs = 4,
    int vs = 0, int init_n = 0) {
    int value_size = vs > 0 ? vs : cfg.value_size;
    int data_count = init_n > 0 ? init_n : capacity;
    if (cfg.channel) {
        Bytes payload;
        ser_int(payload, 0);
        ser_int(payload, static_cast<int>(be));
        ser_int(payload, capacity);
        ser_int(payload, 0);
        ser_int(payload, bs);
        ser_int(payload, value_size);
        ser_int(payload, 0);
        ser_int(payload, 0);
        ser_int(payload, data_count);
        cfg.channel->send_msg(MsgType::SETUP_BENCH, payload);
        MsgType resp_type; Bytes resp;
        cfg.channel->recv_msg(resp_type, resp);
        if (resp_type != MsgType::OK) throw std::runtime_error("SETUP_BENCH failed");
        const uint8_t* p = resp.data();
        return bench_setup::restore_standalone(p, cfg.channel);
    }
    auto data = make_data(data_count, value_size);
    auto omap = make_standalone(be, capacity, bs);
    omap->init(data);
    return omap;
}

static std::unique_ptr<TieredOMap> setup_tiered(
    const Cfg& cfg, OmapBackend be, int N, int n,
    SecurityMode mode = SecurityMode::TierMembership,
    bool use_split = true, int vs = 0,
    OmapBackend hot_be = OmapBackend::AVL, bool use_hot_be = false,
    MaintenanceConfig maint = {}) {
    int value_size = vs > 0 ? vs : cfg.value_size;
    if (cfg.channel) {
        Bytes payload;
        ser_int(payload, 1);
        ser_int(payload, static_cast<int>(be));
        ser_int(payload, N);
        ser_int(payload, n);
        ser_int(payload, 4);
        ser_int(payload, value_size);
        ser_int(payload, mode == SecurityMode::TierMembership ? 1 : 0);
        ser_int(payload, use_split ? 1 : 0);
        ser_int(payload, 0);
        ser_int(payload, static_cast<int>(hot_be));
        ser_int(payload, use_hot_be ? 1 : 0);
        cfg.channel->send_msg(MsgType::SETUP_BENCH, payload);
        MsgType resp_type; Bytes resp;
        cfg.channel->recv_msg(resp_type, resp);
        if (resp_type != MsgType::OK) throw std::runtime_error("SETUP_BENCH tiered failed");
        const uint8_t* p = resp.data();
        return bench_setup::restore_tiered(p, cfg.channel);
    }
    auto data = make_data(N, value_size);
    auto hk = make_hot_keys(n);
    TieredOMapConfig tc;
    tc.total_keys = N; tc.hot_set_size = n;
    tc.mode = mode; tc.use_split_oram = use_split;
    tc.bucket_size = 4; tc.backend = be;
    tc.use_hot_backend = use_hot_be;
    tc.hot_backend = hot_be;
    tc.maintenance = maint;
    auto tm = std::make_unique<TieredOMap>(tc);
    tm->init(data, hk);
    return tm;
}

struct Progress {
    int exp_idx = 0;
    int exp_total = 13;
    Clock::time_point run_start = Clock::now();
    Clock::time_point phase_start = Clock::now();

    std::string fmt_elapsed() {
        auto e = std::chrono::duration<double>(Clock::now() - run_start).count();
        int m = (int)(e / 60), s = (int)e % 60;
        return std::to_string(m) + "m" + std::to_string(s) + "s";
    }

    void begin_exp(const std::string& name) {
        ++exp_idx;
        phase_start = Clock::now();
        std::cout << "\n╔══ [" << exp_idx << "/" << exp_total << "] "
                  << name << "  (total elapsed: " << fmt_elapsed() << ")\n";
    }

    void config(const std::string& label) {
        phase_start = Clock::now();
        std::cout << "  ▸ " << label << std::flush;
    }

    void config_done(const std::string& summary) {
        auto dt = std::chrono::duration<double>(Clock::now() - phase_start).count();
        std::cout << " → " << summary;
        if (dt > 1.0) std::cout << "  [" << std::fixed << std::setprecision(1) << dt << "s]";
        std::cout << "\n";
    }

    void query_tick(int i, int total) {
        if (total < 200) return;
        int step = total / 5;
        if (step > 0 && i > 0 && i % step == 0) {
            int pct = 100 * i / total;
            auto dt = std::chrono::duration<double>(Clock::now() - phase_start).count();
            double eta = dt / i * (total - i);
            std::cout << "    " << pct << "% (" << i << "/" << total
                      << ") eta " << (int)eta << "s\n";
        }
    }
};

static Progress g_progress;

// ═══════════════════════════════════════════════════════════════════════════
// Exp: Bandwidth & Rounds vs N  (Fig 1, paper default = AVL)
// Paper: Single OMAP vs TieredOMap-TM (split), AVL backend
// ═══════════════════════════════════════════════════════════════════════════

static void exp_bandwidth(const Cfg& cfg) {
    std::cout << "\n=== Exp: Bandwidth & Rounds vs N (AVL, Single vs TM+split) ===\n";
    ensure_dir(cfg.outdir);
    std::ofstream csv(cfg.outdir + "/bandwidth_vs_N.csv");
    csv << "logN,type,avg_bw_KB,avg_rounds,avg_answer_rnd,hit_pct\n";

    OmapBackend be = OmapBackend::AVL;

    for (int logN = 12; logN <= cfg.max_logN; logN += 2) {
        int N = 1 << logN;
        int n = std::min(cfg.n, N / 2);

            {
            std::string tag = "logN=" + std::to_string(logN) + " standalone";
                g_progress.config(tag);
                auto omap = setup_standalone(cfg, be, N);
                ZipfSampler z(N, cfg.s, 42);
                for (int i = 0; i < cfg.warmup; ++i) omap->search(z.sample());
                double bw = 0, rnd = 0;
                for (int i = 0; i < cfg.Q; ++i) {
                    omap->search(z.sample());
                    bw += omap->last_stats().total_bytes();
                    rnd += omap->last_stats().rounds;
                    g_progress.query_tick(i, cfg.Q);
                }
                bw /= cfg.Q; rnd /= cfg.Q;
            csv << logN << ",standalone,"
                    << std::fixed << std::setprecision(2) << bw / 1024 << ","
                << std::setprecision(1) << rnd << "," << rnd << ",\n";
            std::ostringstream ss;
            ss << (int)(bw/1024) << "KB " << (int)rnd << "rnd";
            g_progress.config_done(ss.str());
        }

        if (!cfg.channel) {
            std::string tag = "logN=" + std::to_string(logN) + " fair";
            g_progress.config(tag);
            auto ido = setup_fair_baseline(cfg, be, N);
            ZipfSampler z(N, cfg.s, 42);
            for (int i = 0; i < cfg.warmup; ++i) ido.search(z.sample());
            double bw = 0, rnd = 0;
            for (int i = 0; i < cfg.Q; ++i) {
                ido.search(z.sample());
                bw += ido.total_bytes();
                rnd += ido.rounds();
                g_progress.query_tick(i, cfg.Q);
            }
            bw /= cfg.Q; rnd /= cfg.Q;
            csv << logN << ",fair,"
                << std::fixed << std::setprecision(2) << bw / 1024 << ","
                << std::setprecision(1) << rnd << "," << rnd << ",\n";
                std::ostringstream ss;
                ss << (int)(bw/1024) << "KB " << (int)rnd << "rnd";
                g_progress.config_done(ss.str());
            }

            {
            std::string tag = "logN=" + std::to_string(logN) + " TM+split";
                g_progress.config(tag);
            auto tm = setup_tiered(cfg, be, N, n,
                                   SecurityMode::TierMembership, true);
                ZipfSampler z(N, cfg.s, 42);
                for (int i = 0; i < cfg.warmup; ++i) tm->access(z.sample());
                double bw = 0, rnd = 0, ans = 0;
                int hot_cnt = 0;
                for (int i = 0; i < cfg.Q; ++i) {
                    auto r = tm->access(z.sample());
                    bw += r.total_bw.total_bytes();
                    rnd += r.total_bw.rounds;
                    ans += r.rounds_to_answer;
                    if (r.found_in_hot) ++hot_cnt;
                    g_progress.query_tick(i, cfg.Q);
                }
                bw /= cfg.Q; rnd /= cfg.Q; ans /= cfg.Q;
            double hit = 100.0 * hot_cnt / cfg.Q;
            csv << logN << ",TM_split,"
                    << std::fixed << std::setprecision(2) << bw / 1024 << ","
                << std::setprecision(1) << rnd << "," << ans << ","
                << hit << "\n";
                std::ostringstream ss;
                ss << (int)(bw/1024) << "KB " << (int)rnd << "rnd  ans="
                   << std::fixed << std::setprecision(1) << ans
               << " hit=" << (int)hit << "%";
                g_progress.config_done(ss.str());
        }
    }
    csv.close();
    std::cout << "  -> " << cfg.outdir << "/bandwidth_vs_N.csv\n";
}

// ═══════════════════════════════════════════════════════════════════════════
// Exp: Mode comparison  (Tab 2)
// Paper: Single OMAP / TM+split / FO+split, AVL backend, N=2^16 and 2^20
// ═══════════════════════════════════════════════════════════════════════════

static void exp_modes(const Cfg& cfg) {
    std::cout << "\n=== Exp: Mode Comparison (Single / TM+split / FO+split) ===\n";
    ensure_dir(cfg.outdir);
    std::ofstream csv(cfg.outdir + "/modes.csv");
    csv << "logN,mode,avg_bw_KB,avg_rounds,avg_answer_rnd,hit_pct\n";

    OmapBackend be = OmapBackend::AVL;

    std::vector<int> mode_logNs = {16, 20};
    if (cfg.max_logN >= 24) mode_logNs.push_back(24);
    for (int logN : mode_logNs) {
        int N = 1 << logN;
        int n = std::min(cfg.n, N / 2);

        // Single OMAP baseline
        {
            g_progress.config("logN=" + std::to_string(logN) + " Single");
            auto omap = setup_standalone(cfg, be, N);
            ZipfSampler z(N, cfg.s, 42);
            for (int i = 0; i < cfg.warmup; ++i) omap->search(z.sample());
            double bw = 0, rnd = 0;
            for (int i = 0; i < cfg.Q; ++i) {
                omap->search(z.sample());
                bw += omap->last_stats().total_bytes();
                rnd += omap->last_stats().rounds;
            }
            bw /= cfg.Q; rnd /= cfg.Q;
            csv << logN << ",Single,"
                << std::fixed << std::setprecision(2) << bw / 1024 << ","
                << std::setprecision(1) << rnd << "," << rnd << ",\n";
            std::ostringstream ss;
            ss << (int)(bw/1024) << "KB " << (int)rnd << "rnd";
            g_progress.config_done(ss.str());
        }

        if (!cfg.channel) {
            g_progress.config("logN=" + std::to_string(logN) + " Fair");
            auto ido = setup_fair_baseline(cfg, be, N);
            ZipfSampler z(N, cfg.s, 42);
            for (int i = 0; i < cfg.warmup; ++i) ido.search(z.sample());
            double bw = 0, rnd = 0;
            for (int i = 0; i < cfg.Q; ++i) {
                ido.search(z.sample());
                bw += ido.total_bytes();
                rnd += ido.rounds();
            }
            bw /= cfg.Q; rnd /= cfg.Q;
            csv << logN << ",Fair,"
                << std::fixed << std::setprecision(2) << bw / 1024 << ","
                << std::setprecision(1) << rnd << "," << rnd << ",\n";
            std::ostringstream ss;
            ss << (int)(bw/1024) << "KB " << (int)rnd << "rnd";
            g_progress.config_done(ss.str());
        }

        struct ModeSpec { const char* label; SecurityMode mode; };
        ModeSpec modes[] = {
            {"TM_split", SecurityMode::TierMembership},
            {"FO_split", SecurityMode::FullOblivious},
        };

        for (auto& [ml, m] : modes) {
            g_progress.config("logN=" + std::to_string(logN) + " " + ml);
            auto tm = setup_tiered(cfg, be, N, n, m, true);
            ZipfSampler z(N, cfg.s, 42);
            for (int i = 0; i < cfg.warmup; ++i) tm->access(z.sample());
            double bw = 0, rnd = 0, ans = 0;
            int hot = 0;
            for (int i = 0; i < cfg.Q; ++i) {
                auto r = tm->access(z.sample());
                bw += r.total_bw.total_bytes();
                rnd += r.total_bw.rounds;
                ans += r.rounds_to_answer;
                if (r.found_in_hot) ++hot;
            }
            bw /= cfg.Q; rnd /= cfg.Q; ans /= cfg.Q;
            double hit = 100.0 * hot / cfg.Q;
            csv << logN << "," << ml << ","
                << std::fixed << std::setprecision(2) << bw / 1024 << ","
                << std::setprecision(1) << rnd << "," << ans << ","
                << hit << "\n";
            std::ostringstream ss;
            ss << (int)(bw/1024) << "KB " << (int)rnd << "rnd ans="
               << std::fixed << std::setprecision(1) << ans;
            g_progress.config_done(ss.str());
        }
        std::cout << "\n";
    }
    csv.close();
    std::cout << "  -> " << cfg.outdir << "/modes.csv\n";
}

// ═══════════════════════════════════════════════════════════════════════════
// Exp: Backend comparison — standalone OMAP performance  (Fig 3, 4)
// ═══════════════════════════════════════════════════════════════════════════

static void exp_backend_cmp(const Cfg& cfg) {
    std::cout << "\n=== Exp: Backend Comparison (standalone OMAP) ===\n";
    ensure_dir(cfg.outdir);
    std::ofstream csv(cfg.outdir + "/backend_cmp.csv");
    csv << "logN,backend,avg_bw_KB,avg_rounds,avg_comp_us\n";

    for (int logN = 12; logN <= cfg.max_logN; logN += 2) {
        int N = 1 << logN;
        for (auto& [label, be] : ALL_BACKENDS) {
            g_progress.config("logN=" + std::to_string(logN) + " " + label);
            auto omap = setup_standalone(cfg, be, N);
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
            std::ostringstream ss;
            ss << (int)(bw/1024) << "KB " << (int)rnd << "rnd";
            g_progress.config_done(ss.str());
        }
    }
    csv.close();
    std::cout << "  -> " << cfg.outdir << "/backend_cmp.csv\n";
}

// ═══════════════════════════════════════════════════════════════════════════
// Exp: Tiered backend comparison  (Tab 3)
// Paper: All backends inside tiered (TM+split)
// ═══════════════════════════════════════════════════════════════════════════

static void exp_tiered_backend(const Cfg& cfg) {
    std::cout << "\n=== Exp: Tiered Backend Comparison (TM+split) ===\n";
    ensure_dir(cfg.outdir);
    std::ofstream csv(cfg.outdir + "/tiered_backend.csv");
    csv << "logN,backend,avg_bw_KB,avg_rounds,avg_answer_rnd,hit_pct,avg_us\n";

    struct TieredSpec {
        const char* label;
        OmapBackend cold_be;
        OmapBackend hot_be;
        bool use_hot_be;
    };
    static const TieredSpec SPECS[] = {
        {"AVL",             OmapBackend::AVL,     OmapBackend::AVL,    false},
        {"BPlus",           OmapBackend::BPlus,   OmapBackend::BPlus,  false},
        {"DaAvl",           OmapBackend::DaAvl,   OmapBackend::DaAvl,  false},
        {"DaBplus",         OmapBackend::DaBplus,  OmapBackend::DaBplus, false},
        {"BPlus+DaBplus",   OmapBackend::DaBplus,  OmapBackend::BPlus,  true},
        {"BPlus+DaAvl",     OmapBackend::DaAvl,    OmapBackend::BPlus,  true},
    };

    std::vector<int> tier_logNs = {16, 20};
    if (cfg.max_logN >= 24) tier_logNs.push_back(24);
    for (int logN : tier_logNs) {
        int N = 1 << logN;
        int n = std::min(cfg.n, N / 2);
        for (auto& [label, cold_be, hot_be, use_hot] : SPECS) {
            g_progress.config("logN=" + std::to_string(logN) + " " + label);
            auto tm = setup_tiered(cfg, cold_be, N, n,
                                   SecurityMode::TierMembership, true,
                                   0, hot_be, use_hot);
            ZipfSampler z(N, cfg.s, 42);
            for (int i = 0; i < cfg.warmup; ++i) tm->access(z.sample());
            double bw = 0, rnd = 0, ans = 0, us = 0;
            int hot = 0;
            for (int i = 0; i < cfg.Q; ++i) {
                auto t0 = Clock::now();
                auto r = tm->access(z.sample());
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
            std::ostringstream ss;
            ss << (int)(bw/1024) << "KB " << (int)rnd << "rnd ans="
               << std::fixed << std::setprecision(1) << ans
               << " hit=" << (int)hit << "%";
            g_progress.config_done(ss.str());
        }
        std::cout << "\n";
    }
    csv.close();
    std::cout << "  -> " << cfg.outdir << "/tiered_backend.csv\n";
}

// ═══════════════════════════════════════════════════════════════════════════
// Exp: End-to-end Latency  (Fig 5)
// Paper: All backends, Single OMAP vs TM+split, measured wall-clock
// ═══════════════════════════════════════════════════════════════════════════

static void exp_latency(const Cfg& cfg) {
    std::cout << "\n=== Exp: End-to-end Latency (TM+split) ===\n";
    ensure_dir(cfg.outdir);
    std::ofstream csv(cfg.outdir + "/latency.csv");
    csv << "logN,backend,type,avg_latency_ms,avg_answer_ms,avg_rounds,avg_answer_rnd,hit_pct\n";

    std::vector<int> lat_logNs;
    for (int l = 16; l <= cfg.max_logN; l += 2) lat_logNs.push_back(l);

    for (int logN : lat_logNs) {
        int N = 1 << logN;
        int n = std::min(cfg.n, N / 2);
        std::cout << "\n  --- logN=" << logN << " (N=" << N << ", n=" << n << ") ---\n";

    for (auto& [label, be] : ALL_BACKENDS) {
        double bl_ms, bl_rnd;
        {
                g_progress.config("logN=" + std::to_string(logN) + " "
                                  + label + " standalone");
            auto omap = setup_standalone(cfg, be, N);
            ZipfSampler z(N, cfg.s, 42);
            for (int i = 0; i < cfg.warmup; ++i) omap->search(z.sample());
            double total_us = 0, rnd = 0;
            for (int i = 0; i < cfg.Q; ++i) {
                auto t0 = Clock::now();
                omap->search(z.sample());
                total_us += std::chrono::duration<double, std::micro>(
                    Clock::now() - t0).count();
                rnd += omap->last_stats().rounds;
                g_progress.query_tick(i, cfg.Q);
            }
            bl_ms = total_us / cfg.Q / 1000.0;
            bl_rnd = rnd / cfg.Q;
                std::ostringstream ss;
                ss << std::fixed << std::setprecision(1)
                   << bl_ms << "ms " << (int)bl_rnd << "rnd";
                g_progress.config_done(ss.str());
        }

        double tm_ms, tm_ans_ms, tm_rnd, tm_ans_rnd;
        int hot_cnt = 0;
        {
                g_progress.config("logN=" + std::to_string(logN) + " "
                                  + label + " TM+split");
                auto tm = setup_tiered(cfg, be, N, n,
                                       SecurityMode::TierMembership, true);
            ZipfSampler z(N, cfg.s, 42);
            for (int i = 0; i < cfg.warmup; ++i) tm->access(z.sample());
            double total_us = 0, ans_us = 0, rnd = 0, ans_rnd = 0;
            for (int i = 0; i < cfg.Q; ++i) {
                auto t0 = Clock::now();
                auto r = tm->access(z.sample());
                double elapsed = std::chrono::duration<double, std::micro>(
                    Clock::now() - t0).count();
                total_us += elapsed;
                rnd += r.total_bw.rounds;
                ans_rnd += r.rounds_to_answer;
                if (r.found_in_hot) ++hot_cnt;
                double ans_frac = (r.total_bw.rounds > 0)
                    ? (double)r.rounds_to_answer / r.total_bw.rounds : 1.0;
                ans_us += elapsed * ans_frac;
                g_progress.query_tick(i, cfg.Q);
            }
            tm_ms = total_us / cfg.Q / 1000.0;
            tm_ans_ms = ans_us / cfg.Q / 1000.0;
            tm_rnd = rnd / cfg.Q;
            tm_ans_rnd = ans_rnd / cfg.Q;
                std::ostringstream ss;
                ss << std::fixed << std::setprecision(1)
                   << tm_ans_ms << "ms(hot) " << tm_ms << "ms(total)";
                g_progress.config_done(ss.str());
        }

        double hit_pct = 100.0 * hot_cnt / cfg.Q;
        double expected_ms = tm_ans_ms * hit_pct / 100.0
                           + tm_ms * (1.0 - hit_pct / 100.0);

            csv << logN << "," << label << ",standalone,"
                << std::fixed << std::setprecision(1)
            << bl_ms << ",," << bl_rnd << ",,\n";
            csv << logN << "," << label << ",tiered," << tm_ms << ","
                << tm_ans_ms << "," << tm_rnd << "," << tm_ans_rnd << ","
                << hit_pct << "\n";
            csv << logN << "," << label << ",expected,"
                << expected_ms << ",,,,\n";
        }
    }
    csv.close();
    std::cout << "  -> " << cfg.outdir << "/latency.csv\n";
}

// ═══════════════════════════════════════════════════════════════════════════
// Exp: Throughput (ops/sec) vs N  (Fig 6)
// Paper: AVL + DA-B+, standalone vs tiered (TM+split)
// ═══════════════════════════════════════════════════════════════════════════

static void exp_throughput(const Cfg& cfg) {
    std::cout << "\n=== Exp: Throughput (ops/sec) vs N ===\n";
    ensure_dir(cfg.outdir);
    std::ofstream csv(cfg.outdir + "/throughput.csv");
    csv << "logN,backend,type,ops_per_sec,avg_us_per_op\n";

    static const BackendSpec TPUT_BACKENDS[] = {
        {"AVL",     OmapBackend::AVL},
        {"DaBplus", OmapBackend::DaBplus},
    };

    for (int logN = 12; logN <= cfg.max_logN; logN += 2) {
        int N = 1 << logN;
        int n = std::min(cfg.n, N / 2);
        for (auto& [label, be] : TPUT_BACKENDS) {
            {
                g_progress.config("logN=" + std::to_string(logN)
                                  + " " + label + " standalone");
            auto omap = setup_standalone(cfg, be, N);
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
                std::ostringstream ss;
                ss << (int)ops_sec << " ops/s";
                g_progress.config_done(ss.str());
            }

            {
                g_progress.config("logN=" + std::to_string(logN)
                                  + " " + label + " TM+split");
                auto tm = setup_tiered(cfg, be, N, n,
                                       SecurityMode::TierMembership, true);
                ZipfSampler z(N, cfg.s, 42);
                for (int i = 0; i < cfg.warmup; ++i) tm->access(z.sample());
                auto t0 = Clock::now();
                for (int i = 0; i < cfg.Q; ++i) tm->access(z.sample());
                double elapsed_us = std::chrono::duration<double, std::micro>(
                    Clock::now() - t0).count();
                double us_per_op = elapsed_us / cfg.Q;
                double ops_sec = 1e6 / us_per_op;
                csv << logN << "," << label << ",tiered_tm,"
                    << std::fixed << std::setprecision(1) << ops_sec << ","
                    << std::setprecision(0) << us_per_op << "\n";
                std::ostringstream ss;
                ss << (int)ops_sec << " ops/s";
                g_progress.config_done(ss.str());
            }
        }
    }
    csv.close();
    std::cout << "  -> " << cfg.outdir << "/throughput.csv\n";
}

// ═══════════════════════════════════════════════════════════════════════════
// Exp: Skewness effect  (Fig 7)
// Paper: AVL backend, TM+split, sweep Zipf s
// ═══════════════════════════════════════════════════════════════════════════

static void run_skewness_backend(const Cfg& cfg, OmapBackend be,
                                 const char* be_label, const char* file_tag) {
    int N = 1 << cfg.max_logN;
    std::string fname = std::string(file_tag) + ".csv";
    std::ofstream csv(cfg.outdir + "/" + fname);
    csv << "zipf_s,log_n,hit_pct,base_rnd,avg_ans_rnd,hot_ans_rnd,"
        << "avg_red_pct,hot_red_pct,avg_bw_KB,base_bw_KB\n";

    double base_rnd = 0, base_bw = 0;
    {
        g_progress.config(std::string(be_label) + " baseline N=" +
                          std::to_string(N));
        auto omap = setup_standalone(cfg, be, N);
        ZipfSampler z(N, 1.0, 42);
        for (int i = 0; i < cfg.warmup; ++i) omap->search(z.sample());
        for (int i = 0; i < cfg.Q; ++i) {
            omap->search(z.sample());
            base_rnd += omap->last_stats().rounds;
            base_bw += omap->last_stats().total_bytes();
        }
        base_rnd /= cfg.Q; base_bw /= cfg.Q;
        g_progress.config_done(
            "rnd=" + std::to_string((int)base_rnd) + " bw=" +
            std::to_string((int)(base_bw/1024)) + "KB");
    }

    std::vector<int> n_vals = {1024, 4096, 16384};
    std::vector<double> s_vals = {0.5, 0.7, 0.9, 1.0, 1.1, 1.3, 1.5};

    for (int n : n_vals) {
        int log_n = (int)std::round(std::log2(n));
        for (double s : s_vals) {
            std::string tag = std::string(be_label) + " n=2^" +
                std::to_string(log_n) + " s=" + std::to_string(s).substr(0,3);
            g_progress.config(tag);
            auto tm = setup_tiered(cfg, be, N, n,
                                   SecurityMode::TierMembership, true);
            ZipfSampler z(N, s, 42);
            for (int i = 0; i < cfg.warmup; ++i) tm->access(z.sample());
            double avg_ans = 0, hot_ans = 0, avg_bw = 0;
            int hot_cnt = 0;
            for (int i = 0; i < cfg.Q; ++i) {
                auto r = tm->access(z.sample());
                avg_ans += r.rounds_to_answer;
                avg_bw += r.total_bw.total_bytes();
                if (r.found_in_hot) {
                    hot_ans += r.rounds_to_answer;
                    ++hot_cnt;
                }
                g_progress.query_tick(i, cfg.Q);
            }
            avg_ans /= cfg.Q; avg_bw /= cfg.Q;
            double hit_pct = 100.0 * hot_cnt / cfg.Q;
            double hot_avg = (hot_cnt > 0) ? hot_ans / hot_cnt : 0;
            double avg_red = base_rnd > 0
                ? (base_rnd - avg_ans) / base_rnd * 100 : 0;
            double hot_red = (base_rnd > 0 && hot_cnt > 0)
                ? (base_rnd - hot_avg) / base_rnd * 100 : 0;
            csv << std::fixed << std::setprecision(2) << s << ","
                << log_n << "," << hit_pct << "," << base_rnd << ","
                << avg_ans << "," << hot_avg << ","
                << avg_red << "," << hot_red << ","
                << avg_bw/1024 << "," << base_bw/1024 << "\n";
            csv.flush();
            g_progress.config_done(
                "avg_red=" + std::to_string((int)avg_red) + "% hot_red=" +
                std::to_string((int)hot_red) + "%");
        }
    }
    csv.close();
    std::cout << "  -> " << cfg.outdir << "/" << fname << "\n";
}

static void exp_skewness(const Cfg& cfg) {
    std::cout << "\n=== Exp: Skewness + Hot-Set Size Effect ===\n";
    ensure_dir(cfg.outdir);
    run_skewness_backend(cfg, OmapBackend::AVL, "AVL", "skewness");
    run_skewness_backend(cfg, OmapBackend::BPlus, "BPlus", "skewness_bplus");
    run_skewness_backend(cfg, OmapBackend::DaBplus, "DaBplus", "skewness_dabplus");
}

// ═══════════════════════════════════════════════════════════════════════════
// Exp: Hot-set size effect  (Fig 8)
// Paper: AVL backend, FO+split, sweep n
// ═══════════════════════════════════════════════════════════════════════════

static void exp_hotsize(const Cfg& cfg) {
    std::cout << "\n=== Exp: Hot-Set Size (AVL, TM+split) ===\n";
    ensure_dir(cfg.outdir);
    int N = 1 << cfg.max_logN;
    OmapBackend be = OmapBackend::AVL;

    double base_rnd = 0;
    {
        g_progress.config("AVL baseline N=" + std::to_string(N));
        auto omap = setup_standalone(cfg, be, N);
        ZipfSampler z(N, 1.0, 42);
        for (int i = 0; i < cfg.warmup; ++i) omap->search(z.sample());
        for (int i = 0; i < cfg.Q; ++i) {
            omap->search(z.sample());
            base_rnd += omap->last_stats().rounds;
        }
        base_rnd /= cfg.Q;
        g_progress.config_done("rnd=" + std::to_string((int)base_rnd));
    }

    std::ofstream csv(cfg.outdir + "/hotsize.csv");
    csv << "zipf_s,log_n,base_rnd,avg_ans_rnd,avg_red_pct,hit_pct\n";

    std::vector<double> s_vals = {0.9, 1.0, 1.3};
    std::vector<int> log_n_vals = {6, 8, 10, 12, 14, 16};

    for (double s : s_vals) {
        for (int log_n : log_n_vals) {
            int n = 1 << log_n;
            if (n >= N / 2) continue;
            std::string tag = "s=" + std::to_string(s).substr(0,3)
                + " n=2^" + std::to_string(log_n);
            g_progress.config(tag);
            auto tm = setup_tiered(cfg, be, N, n,
                                   SecurityMode::TierMembership, true);
            ZipfSampler z(N, s, 42);
            for (int i = 0; i < cfg.warmup; ++i) tm->access(z.sample());
            double avg_ans = 0; int hot_cnt = 0;
            for (int i = 0; i < cfg.Q; ++i) {
                auto r = tm->access(z.sample());
                avg_ans += r.rounds_to_answer;
                if (r.found_in_hot) ++hot_cnt;
                g_progress.query_tick(i, cfg.Q);
            }
            avg_ans /= cfg.Q;
            double hit_pct = 100.0 * hot_cnt / cfg.Q;
            double avg_red = base_rnd > 0
                ? (base_rnd - avg_ans) / base_rnd * 100 : 0;
            csv << std::fixed << std::setprecision(2) << s << ","
                << log_n << "," << base_rnd << ","
                << avg_ans << "," << avg_red << "," << hit_pct << "\n";
            csv.flush();
            g_progress.config_done(
                "red=" + std::to_string((int)avg_red) + "% hit=" +
                std::to_string((int)hit_pct) + "%");
        }
    }
    csv.close();
    std::cout << "  -> " << cfg.outdir << "/hotsize.csv\n";
}

// ═══════════════════════════════════════════════════════════════════════════
// Exp: Split-ORAM ablation  (Tab 4)
// Paper: AVL, TM no-split vs TM+split, vary N
// ═══════════════════════════════════════════════════════════════════════════

static void exp_split_ablation(const Cfg& cfg) {
    std::cout << "\n=== Exp: Split-ORAM Ablation (AVL, TM) ===\n";
    ensure_dir(cfg.outdir);
    std::ofstream csv(cfg.outdir + "/split_ablation.csv");
    csv << "logN,split,avg_bw_KB,avg_rounds,avg_answer_rnd\n";

    OmapBackend be = OmapBackend::AVL;

    for (int logN = 14; logN <= cfg.max_logN; logN += 2) {
        int N = 1 << logN;
        int n = std::min(cfg.n, N / 2);

        for (bool split : {false, true}) {
            std::string tag = "logN=" + std::to_string(logN)
                              + (split ? " split" : " no-split");
            g_progress.config(tag);
            auto tm = setup_tiered(cfg, be, N, n,
                                   SecurityMode::TierMembership, split);
            ZipfSampler z(N, cfg.s, 42);
            for (int i = 0; i < cfg.warmup; ++i) tm->access(z.sample());
            double bw = 0, rnd = 0, ans = 0;
            for (int i = 0; i < cfg.Q; ++i) {
                auto r = tm->access(z.sample());
                bw += r.total_bw.total_bytes();
                rnd += r.total_bw.rounds;
                ans += r.rounds_to_answer;
            }
            bw /= cfg.Q; rnd /= cfg.Q; ans /= cfg.Q;
            csv << logN << "," << (split ? 1 : 0) << ","
                << std::fixed << std::setprecision(2) << bw / 1024 << ","
                << std::setprecision(1) << rnd << "," << ans << "\n";
            std::ostringstream ss;
            ss << (int)(bw/1024) << "KB " << (int)rnd << "rnd";
            g_progress.config_done(ss.str());
        }
    }
    csv.close();
    std::cout << "  -> " << cfg.outdir << "/split_ablation.csv\n";
}

// ═══════════════════════════════════════════════════════════════════════════
// Exp: Dynamic hot-set convergence  (Fig 9)
// Paper: N=2^16, n=1024, s=1.0, epoch=256, AVL backend
// ═══════════════════════════════════════════════════════════════════════════

static double zipf_H(int N, double s) {
    double h = 0;
    for (int i = 1; i <= N; ++i) h += std::pow(i, -s);
    return h;
}

static double theoretical_hit_rate(const std::unordered_set<int>& hot_keys,
                                   int N, double s, double H, int shift = 0) {
    double sum = 0;
    for (int k : hot_keys) {
        int rank0 = (k - shift % N + N) % N;
        sum += std::pow(rank0 + 1, -s);
    }
    return 100.0 * sum / H;
}

static void run_convergence(const Cfg& cfg, int logN, const char* file_tag) {
    int N = 1 << logN;
    int n = cfg.n;
    int total = 500000;
    int report_every = 2000;
    double s = cfg.s;
    double H = zipf_H(N, s);
    double optimal = 0;
    for (int i = 1; i <= n; ++i) optimal += std::pow(i, -s);
    optimal = 100.0 * optimal / H;

    std::string fname = std::string(file_tag) + ".csv";
    std::ofstream csv(cfg.outdir + "/" + fname);
    csv << "queries_K,B_obs,hit_rate_pct\n";

    auto data = make_data(N, cfg.value_size);
    auto hk = make_hot_keys(n);
    OmapBackend be = OmapBackend::AVL;

    std::vector<int> obs_windows = {16384, 32768, 65536};

    for (int B_obs : obs_windows) {
        std::string label = "B_obs=" + std::to_string(B_obs);
        g_progress.config(label.c_str());

        TieredOMapConfig tc;
        tc.total_keys = N; tc.hot_set_size = n;
        tc.mode = SecurityMode::TierMembership;
        tc.use_split_oram = true;
        tc.backend = be;
        tc.storage_creator = cfg.storage_creator;
        tc.maintenance.enabled = true;
        tc.maintenance.observation_window = B_obs;
        tc.maintenance.swap_interval = 32;
        tc.maintenance.promote_threshold = 5;
        tc.maintenance.demote_threshold = 2;
        tc.maintenance.staleness_windows = 1;
        TieredOMap tm(tc); tm.init(data, hk);
        ZipfSampler z(N, s, 42);

        double hr0 = theoretical_hit_rate(tm.hot_keys(), N, s, H);
        csv << "0," << B_obs << "," << std::fixed << std::setprecision(2)
            << hr0 << "\n";

        for (int q = 0; q < total; ++q) {
            tm.access(z.sample());
            if ((q + 1) % report_every == 0) {
                double hr = theoretical_hit_rate(tm.hot_keys(), N, s, H);
                csv << (q + 1) / 1000.0 << "," << B_obs << ","
                    << std::fixed << std::setprecision(2) << hr << "\n";
                csv.flush();
            }
            g_progress.query_tick(q, total);
        }
        g_progress.config_done("done");
    }
    csv << "# optimal," << std::fixed << std::setprecision(2) << optimal << "\n";
    csv.close();
    std::cout << "  -> " << cfg.outdir << "/" << fname << "\n";
}

static void exp_dynamic(const Cfg& cfg) {
    std::cout << "\n=== Exp: Dynamic Hot-Set Convergence ===\n";
    ensure_dir(cfg.outdir);
    run_convergence(cfg, 24, "dynamic");
    run_convergence(cfg, 20, "dynamic_N20");
}

// ═══════════════════════════════════════════════════════════════════════════
// Exp: Workload drift  (Fig 10)
// Paper: N=2^16, n=1024, s=1.0, shift at query 2000, AVL backend
// ═══════════════════════════════════════════════════════════════════════════

static void run_drift(const Cfg& cfg, int logN, const char* file_tag) {
    int N = 1 << logN;
    int n = cfg.n;
    int B_obs_fixed = 65536;
    int shift_offset = N / 2;
    int post_onset = 150000;
    int report_every = 2000;
    double s = cfg.s;
    double H = zipf_H(N, s);

    std::string fname = std::string(file_tag) + ".csv";
    std::ofstream csv(cfg.outdir + "/" + fname);
    csv << "onset_queries_K,B_swap,hit_rate_pct\n";

    auto data = make_data(N, cfg.value_size);
    auto hk = make_hot_keys(n);
    OmapBackend be = OmapBackend::AVL;

    std::vector<int> swap_intervals = {32, 64, 128, 0};

    for (int B_swap : swap_intervals) {
        bool maint_on = (B_swap > 0);
        std::string label = maint_on
            ? "B_swap=" + std::to_string(B_swap) : "no_maint";
        g_progress.config(label.c_str());

        TieredOMapConfig tc;
        tc.total_keys = N; tc.hot_set_size = n;
        tc.mode = SecurityMode::TierMembership;
        tc.use_split_oram = true;
        tc.backend = be;
        tc.storage_creator = cfg.storage_creator;
        if (maint_on) {
            tc.maintenance.enabled = true;
            tc.maintenance.observation_window = B_obs_fixed;
            tc.maintenance.swap_interval = B_swap;
            tc.maintenance.promote_threshold = 5;
            tc.maintenance.demote_threshold = 2;
            tc.maintenance.staleness_windows = 1;
        }
        TieredOMap tm(tc); tm.init(data, hk);
        ZipfSampler z(N, s, 42);

        int total = B_obs_fixed + post_onset;

        for (int q = 0; q < total; ++q) {
            int key = (z.sample() + shift_offset) % N;
            tm.access(key);
            int from_onset = q - B_obs_fixed;
            if (from_onset >= 0 && from_onset % report_every == 0) {
                double hr = theoretical_hit_rate(
                    tm.hot_keys(), N, s, H, shift_offset);
                csv << from_onset / 1000.0 << ","
                    << (maint_on ? std::to_string(B_swap) : "no_maint")
                    << "," << std::fixed << std::setprecision(2)
                    << hr << "\n";
                csv.flush();
            }
            g_progress.query_tick(q, total);
        }
        g_progress.config_done("done");
    }
    csv.close();
    std::cout << "  -> " << cfg.outdir << "/" << fname << "\n";
}

static void exp_drift(const Cfg& cfg) {
    std::cout << "\n=== Exp: Workload Drift ===\n";
    ensure_dir(cfg.outdir);
    run_drift(cfg, 24, "drift");
    run_drift(cfg, 20, "drift_N20");
}

// ═══════════════════════════════════════════════════════════════════════════
// Exp: WAN static — inject simulated RTT, static hot set, verify theory
// ═══════════════════════════════════════════════════════════════════════════

static void exp_wan_static(const Cfg& cfg) {
    std::cout << "\n=== WAN Static (simulated RTT, fair baseline) ===\n";

    int N = 1 << 14;
        int n = std::min(cfg.n, N / 2);
    int rtt_us = cfg.rtt_us > 0 ? cfg.rtt_us : 10000;
    double rtt_ms = rtt_us / 1000.0;
    int Q = std::min(cfg.Q, 50);
    OmapBackend be = OmapBackend::AVL;

    std::cout << "  N=" << N << " n=" << n << " s=" << cfg.s
              << " RTT=" << rtt_ms << "ms Q=" << Q << "\n\n";

    struct Result {
        const char* label;
        double avg_bw_kb;
        double avg_rounds;
        double avg_answer_rnd;
        double avg_latency_ms;
        double avg_answer_ms;
        double hit_pct;
    };
    std::vector<Result> results;

    auto run_zipf = [&](int seed = 42) { return ZipfSampler(N, cfg.s, seed); };

    // --- (A) Raw standalone OMAP (256B values, no index/data split) ---
            {
                auto omap = setup_standalone(cfg, be, N);
        omap->set_round_delay_us(rtt_us);
        auto z = run_zipf();
        for (int i = 0; i < 5; ++i) omap->search(z.sample());
        double bw = 0, rnd = 0, lat_us = 0;
        for (int i = 0; i < Q; ++i) {
                auto t0 = Clock::now();
            omap->search(z.sample());
            lat_us += std::chrono::duration<double, std::micro>(
                    Clock::now() - t0).count();
            bw += omap->last_stats().total_bytes();
            rnd += omap->last_stats().rounds;
        }
        results.push_back({"Raw OMAP",
            bw/Q/1024, rnd/Q, rnd/Q, lat_us/Q/1000, lat_us/Q/1000, 0});
    }

    // --- (B) Fair baseline: index OMAP(4B) + data ORAM(256B) ---
    {
        auto ido = setup_fair_baseline(cfg, be, N);
        ido.set_round_delay_us(rtt_us);
        auto z = run_zipf();
        for (int i = 0; i < 5; ++i) ido.search(z.sample());
        double bw = 0, rnd = 0, lat_us = 0;
        for (int i = 0; i < Q; ++i) {
                auto t0 = Clock::now();
            ido.search(z.sample());
            lat_us += std::chrono::duration<double, std::micro>(
                    Clock::now() - t0).count();
            bw += ido.total_bytes();
            rnd += ido.rounds();
        }
        results.push_back({"Fair baseline",
            bw/Q/1024, rnd/Q, rnd/Q, lat_us/Q/1000, lat_us/Q/1000, 0});
    }

    // --- (C) TM + split ---
    {
        auto tm = setup_tiered(cfg, be, N, n,
                               SecurityMode::TierMembership, true);
        tm->set_round_delay_us(rtt_us);
        auto z = run_zipf();
        for (int i = 0; i < 5; ++i) tm->access(z.sample());
        double bw = 0, rnd = 0, ans = 0, lat_us = 0, ans_us = 0;
        int hot = 0;
        for (int i = 0; i < Q; ++i) {
            auto t0 = Clock::now();
            auto r = tm->access(z.sample());
            double elapsed = std::chrono::duration<double, std::micro>(
                Clock::now() - t0).count();
            lat_us += elapsed;
            bw += r.total_bw.total_bytes();
            rnd += r.total_bw.rounds;
            ans += r.rounds_to_answer;
            if (r.found_in_hot) ++hot;
            double frac = r.total_bw.rounds > 0
                ? (double)r.rounds_to_answer / r.total_bw.rounds : 1.0;
            ans_us += elapsed * frac;
        }
        results.push_back({"TM+split",
            bw/Q/1024, rnd/Q, ans/Q, lat_us/Q/1000, ans_us/Q/1000,
            100.0*hot/Q});
    }

    // --- (D) FO + split ---
    {
        auto tm = setup_tiered(cfg, be, N, n,
                               SecurityMode::FullOblivious, true);
        tm->set_round_delay_us(rtt_us);
        auto z = run_zipf();
        for (int i = 0; i < 5; ++i) tm->access(z.sample());
        double bw = 0, rnd = 0, ans = 0, lat_us = 0, ans_us = 0;
            int hot = 0;
        for (int i = 0; i < Q; ++i) {
                auto t0 = Clock::now();
                auto r = tm->access(z.sample());
            double elapsed = std::chrono::duration<double, std::micro>(
                    Clock::now() - t0).count();
            lat_us += elapsed;
                bw += r.total_bw.total_bytes();
                rnd += r.total_bw.rounds;
                ans += r.rounds_to_answer;
                if (r.found_in_hot) ++hot;
            double frac = r.total_bw.rounds > 0
                ? (double)r.rounds_to_answer / r.total_bw.rounds : 1.0;
            ans_us += elapsed * frac;
        }
        results.push_back({"FO+split",
            bw/Q/1024, rnd/Q, ans/Q, lat_us/Q/1000, ans_us/Q/1000,
            100.0*hot/Q});
    }

    // --- Print results ---
    std::cout << std::left << std::setw(16) << "Config"
              << std::right
              << std::setw(9)  << "BW(KB)"
              << std::setw(8)  << "Rounds"
              << std::setw(8)  << "AnsRnd"
              << std::setw(11) << "Latency"
              << std::setw(11) << "AnsLat"
              << std::setw(7)  << "Hit%"
              << "\n";
    std::cout << std::string(70, '-') << "\n";

    for (auto& r : results)
        std::cout << std::left << std::setw(16) << r.label
                  << std::right << std::fixed
                  << std::setw(9) << std::setprecision(0) << r.avg_bw_kb
                  << std::setw(8) << std::setprecision(1) << r.avg_rounds
                  << std::setw(8) << std::setprecision(1) << r.avg_answer_rnd
                  << std::setw(10) << std::setprecision(1)
                  << r.avg_latency_ms << "ms"
                  << std::setw(10) << std::setprecision(1)
                  << r.avg_answer_ms << "ms"
                  << std::setw(7) << std::setprecision(0) << r.hit_pct
                  << "\n";

    // --- Checks ---
    auto& raw   = results[0];
    auto& fair  = results[1];
    auto& tm_sp = results[2];
    auto& fo_sp = results[3];

    auto check = [](const char* desc, bool ok) {
        std::cout << (ok ? "  [PASS] " : "  [FAIL] ") << desc << "\n";
    };

    std::cout << "\n--- Index/Data Separation (Raw vs Fair) ---\n";
    check("Fair rounds ≈ Raw rounds (+1 data ORAM)",
          fair.avg_rounds >= raw.avg_rounds
          && fair.avg_rounds <= raw.avg_rounds * 1.1);
    check("Fair BW < Raw BW (smaller index blocks)",
          fair.avg_bw_kb < raw.avg_bw_kb);

    std::cout << "\n--- Tiering Benefit (Fair baseline vs Tiered) ---\n";
    check("TM answer rounds < Fair rounds",
          tm_sp.avg_answer_rnd < fair.avg_rounds);
    check("FO answer rounds ≈ TM answer rounds (within 10%)",
          std::abs(fo_sp.avg_answer_rnd - tm_sp.avg_answer_rnd)
              / tm_sp.avg_answer_rnd < 0.10);
    check("FO total rounds ≈ Fair rounds (within 15%)",
          std::abs(fo_sp.avg_rounds - fair.avg_rounds)
              / fair.avg_rounds < 0.15);
    check("BW: TM+split < Fair baseline",
          tm_sp.avg_bw_kb < fair.avg_bw_kb);
    check("BW: TM+split < FO+split",
          tm_sp.avg_bw_kb < fo_sp.avg_bw_kb);

    std::cout << "\n--- Latency ---\n";
    check("TM latency < Fair latency",
          tm_sp.avg_latency_ms < fair.avg_latency_ms);
    check("FO answer latency < FO total latency",
          fo_sp.avg_answer_ms < fo_sp.avg_latency_ms);

    // --- Summary ---
    std::cout << "\n--- Summary ---\n";
    double idx_data_bw = 1.0 - fair.avg_bw_kb / raw.avg_bw_kb;
    double tier_rnd = 1.0 - tm_sp.avg_answer_rnd / fair.avg_rounds;
    double tier_bw = 1.0 - tm_sp.avg_bw_kb / fair.avg_bw_kb;

    std::cout << "  Index/data separation BW saving: "
              << std::fixed << std::setprecision(1) << idx_data_bw * 100
              << "% (Fair=" << (int)fair.avg_bw_kb
              << "KB vs Raw=" << (int)raw.avg_bw_kb << "KB)\n";
    std::cout << "  Tiering answer-round saving: "
              << std::setprecision(1) << tier_rnd * 100
              << "% (TM ans=" << tm_sp.avg_answer_rnd
              << " vs Fair=" << fair.avg_rounds << ")\n";
    std::cout << "  Tiering BW saving (vs fair): "
              << std::setprecision(1) << tier_bw * 100
              << "% (TM=" << (int)tm_sp.avg_bw_kb
              << "KB vs Fair=" << (int)fair.avg_bw_kb << "KB)\n";
    std::cout << "  Hit rate: " << std::setprecision(1)
              << tm_sp.hit_pct << "% (expected ~65-80%)\n";
}

// ═══════════════════════════════════════════════════════════════════════════
// Exp: WAN dynamic — simulated RTT, epoch-based maintenance
// ═══════════════════════════════════════════════════════════════════════════

static void exp_wan_dynamic(const Cfg& cfg) {
    std::cout << "\n=== WAN Dynamic (simulated RTT, epoch maintenance) ===\n";

    int N = 1 << 14;
    int n = std::min(cfg.n, N / 2);
    int rtt_us = cfg.rtt_us > 0 ? cfg.rtt_us : 10000;
    double rtt_ms = rtt_us / 1000.0;
    int Q = std::min(cfg.Q, 50);
    OmapBackend be = OmapBackend::AVL;

    MaintenanceConfig mc;
    mc.enabled = true;
    mc.observation_window = 256;
    mc.swap_interval = 256;
    mc.promote_threshold = 5;
    mc.demote_threshold = 2;
    mc.staleness_windows = 3;
    mc.piggyback = true;

    std::cout << "  N=" << N << " n=" << n << " s=" << cfg.s
              << " RTT=" << rtt_ms << "ms Q=" << Q
              << " B_obs=" << mc.observation_window
              << " B_swap=" << mc.swap_interval << "\n\n";

    struct Result {
        const char* label;
        double avg_bw_kb;
        double avg_rounds;
        double avg_answer_rnd;
        double avg_latency_ms;
        double avg_answer_ms;
        double hit_pct;
    };
    std::vector<Result> results;

    auto run_zipf = [&](int seed = 42) { return ZipfSampler(N, cfg.s, seed); };

    // --- (A) Fair baseline (single-tier, index+data split) ---
    {
        auto ido = setup_fair_baseline(cfg, be, N);
        ido.set_round_delay_us(rtt_us);
        auto z = run_zipf();
        for (int i = 0; i < 5; ++i) ido.search(z.sample());
        double bw = 0, rnd = 0, lat_us = 0;
        for (int i = 0; i < Q; ++i) {
            auto t0 = Clock::now();
            ido.search(z.sample());
            lat_us += std::chrono::duration<double, std::micro>(
                Clock::now() - t0).count();
            bw += ido.total_bytes();
            rnd += ido.rounds();
        }
        results.push_back({"Fair baseline",
            bw/Q/1024, rnd/Q, rnd/Q, lat_us/Q/1000, lat_us/Q/1000, 0});
    }

    // --- (B) TM+split (static, for reference) ---
    {
        auto tm = setup_tiered(cfg, be, N, n,
                               SecurityMode::TierMembership, true);
        tm->set_round_delay_us(rtt_us);
        auto z = run_zipf();
        for (int i = 0; i < 5; ++i) tm->access(z.sample());
        double bw = 0, rnd = 0, ans = 0, lat_us = 0, ans_us = 0;
        int hot = 0;
        for (int i = 0; i < Q; ++i) {
            auto t0 = Clock::now();
                auto r = tm->access(z.sample());
            double elapsed = std::chrono::duration<double, std::micro>(
                Clock::now() - t0).count();
            lat_us += elapsed;
                bw += r.total_bw.total_bytes();
                rnd += r.total_bw.rounds;
                ans += r.rounds_to_answer;
            if (r.found_in_hot) ++hot;
            double frac = r.total_bw.rounds > 0
                ? (double)r.rounds_to_answer / r.total_bw.rounds : 1.0;
            ans_us += elapsed * frac;
        }
        results.push_back({"TM+split",
            bw/Q/1024, rnd/Q, ans/Q, lat_us/Q/1000, ans_us/Q/1000,
            100.0*hot/Q});
    }

    // --- (C) TM+split+dynamic ---
    {
        auto tm = setup_tiered(cfg, be, N, n,
                               SecurityMode::TierMembership, true,
                               0, OmapBackend::AVL, false, mc);
        tm->set_round_delay_us(rtt_us);
        auto z = run_zipf();
        for (int i = 0; i < 50; ++i) tm->access(z.sample());
        double bw = 0, rnd = 0, ans = 0, lat_us = 0, ans_us = 0;
        int hot = 0;
        for (int i = 0; i < Q; ++i) {
            auto t0 = Clock::now();
            auto r = tm->access(z.sample());
            double elapsed = std::chrono::duration<double, std::micro>(
                Clock::now() - t0).count();
            lat_us += elapsed;
            bw += r.total_bw.total_bytes();
            rnd += r.total_bw.rounds;
            ans += r.rounds_to_answer;
            if (r.found_in_hot) ++hot;
            double frac = r.total_bw.rounds > 0
                ? (double)r.rounds_to_answer / r.total_bw.rounds : 1.0;
            ans_us += elapsed * frac;
        }
        results.push_back({"TM+split+dyn",
            bw/Q/1024, rnd/Q, ans/Q, lat_us/Q/1000, ans_us/Q/1000,
            100.0*hot/Q});
    }

    // --- (D) FO+split+dynamic ---
    {
        auto tm = setup_tiered(cfg, be, N, n,
                               SecurityMode::FullOblivious, true,
                               0, OmapBackend::AVL, false, mc);
        tm->set_round_delay_us(rtt_us);
        auto z = run_zipf();
        for (int i = 0; i < 50; ++i) tm->access(z.sample());
        double bw = 0, rnd = 0, ans = 0, lat_us = 0, ans_us = 0;
        int hot = 0;
        for (int i = 0; i < Q; ++i) {
            auto t0 = Clock::now();
            auto r = tm->access(z.sample());
            double elapsed = std::chrono::duration<double, std::micro>(
                Clock::now() - t0).count();
            lat_us += elapsed;
            bw += r.total_bw.total_bytes();
            rnd += r.total_bw.rounds;
            ans += r.rounds_to_answer;
            if (r.found_in_hot) ++hot;
            double frac = r.total_bw.rounds > 0
                ? (double)r.rounds_to_answer / r.total_bw.rounds : 1.0;
            ans_us += elapsed * frac;
        }
        results.push_back({"FO+split+dyn",
            bw/Q/1024, rnd/Q, ans/Q, lat_us/Q/1000, ans_us/Q/1000,
            100.0*hot/Q});
    }

    // --- Print results ---
    std::cout << std::left << std::setw(16) << "Config"
              << std::right
              << std::setw(9)  << "BW(KB)"
              << std::setw(8)  << "Rounds"
              << std::setw(8)  << "AnsRnd"
              << std::setw(11) << "Latency"
              << std::setw(11) << "AnsLat"
              << std::setw(7)  << "Hit%"
              << "\n";
    std::cout << std::string(70, '-') << "\n";

    for (auto& r : results)
        std::cout << std::left << std::setw(16) << r.label
                  << std::right << std::fixed
                  << std::setw(9) << std::setprecision(0) << r.avg_bw_kb
                  << std::setw(8) << std::setprecision(1) << r.avg_rounds
                  << std::setw(8) << std::setprecision(1) << r.avg_answer_rnd
                  << std::setw(10) << std::setprecision(1)
                  << r.avg_latency_ms << "ms"
                  << std::setw(10) << std::setprecision(1)
                  << r.avg_answer_ms << "ms"
                  << std::setw(7) << std::setprecision(0) << r.hit_pct
                  << "\n";

    // --- Checks ---
    auto& fair    = results[0];
    auto& tm_stat = results[1];
    auto& tm_dyn  = results[2];
    auto& fo_dyn  = results[3];

    auto check = [](const char* desc, bool ok) {
        std::cout << (ok ? "  [PASS] " : "  [FAIL] ") << desc << "\n";
    };

    std::cout << "\n--- Dynamic vs Static ---\n";
    check("TM+dyn hit rate >= TM static hit rate - 10%",
          tm_dyn.hit_pct >= tm_stat.hit_pct - 10);
    check("TM+dyn answer rounds ≈ TM static (within 20%)",
          tm_dyn.avg_answer_rnd <= tm_stat.avg_answer_rnd * 1.2);
    check("TM+dyn answer rounds < Fair rounds",
          tm_dyn.avg_answer_rnd < fair.avg_rounds);

    std::cout << "\n--- FO Dynamic ---\n";
    check("FO+dyn answer rounds ≈ TM+dyn answer rounds (within 15%)",
          std::abs(fo_dyn.avg_answer_rnd - tm_dyn.avg_answer_rnd)
              / tm_dyn.avg_answer_rnd < 0.15);

    std::cout << "\n--- Summary ---\n";
    double overhead_rnd = tm_dyn.avg_rounds / tm_stat.avg_rounds - 1.0;
    double overhead_bw  = tm_dyn.avg_bw_kb / tm_stat.avg_bw_kb - 1.0;
    std::cout << "  Dynamic round overhead: "
              << std::fixed << std::setprecision(1) << overhead_rnd * 100
              << "% (dyn=" << tm_dyn.avg_rounds
              << " vs static=" << tm_stat.avg_rounds << ")\n";
    std::cout << "  Dynamic BW overhead: "
              << std::setprecision(1) << overhead_bw * 100
              << "% (dyn=" << (int)tm_dyn.avg_bw_kb
              << "KB vs static=" << (int)tm_stat.avg_bw_kb << "KB)\n";
    std::cout << "  Dynamic hit rate: " << std::setprecision(1)
              << tm_dyn.hit_pct << "% (static: "
              << tm_stat.hit_pct << "%)\n";
}

// ═══════════════════════════════════════════════════════════════════════════
// Exp: Paper WAN — full Table 1 + Fig 3 data with hot/cold separation
// Produces one CSV row per (N, backend) with all columns for the paper.
// ═══════════════════════════════════════════════════════════════════════════

static void exp_paper_wan(const Cfg& cfg) {
    std::cout << "\n=== Paper WAN: Table 1 + Fig 3 (hot/cold separated) ===\n";
    ensure_dir(cfg.outdir);
    bool appending = !cfg.backend_filter.empty();
    std::ofstream csv(cfg.outdir + "/paper_wan.csv",
                      appending ? std::ios::app : std::ios::trunc);
    if (!appending)
        csv << "logN,backend,hit_pct,"
            << "base_bw_KB,base_rnd,base_ms,"
            << "tm_hot_bw_KB,tm_cold_bw_KB,tm_mean_bw_KB,fo_bw_KB,"
            << "tm_hot_rnd,tm_cold_rnd,tm_mean_rnd,"
            << "tm_hot_ms,tm_cold_ms,tm_mean_ms\n";

    static const BackendSpec PAPER_BE[] = {
        {"AVL",     OmapBackend::AVL},
        {"BPlus",   OmapBackend::BPlus},
        {"DaBplus", OmapBackend::DaBplus},
    };

    std::vector<int> logNs;
    for (int l = 16; l <= cfg.max_logN; l += 2) logNs.push_back(l);

    for (int logN : logNs) {
        int N = 1 << logN;
        int n = std::min(cfg.n, N / 2);
        std::cout << "\n--- logN=" << logN << " N=" << N << " n=" << n << " ---\n";

        for (auto& [label, be] : PAPER_BE) {
            if (!cfg.backend_filter.empty() && cfg.backend_filter != label)
                continue;
            int Q = cfg.Q;

            // ── (A) Standalone baseline ──
            double bl_bw = 0, bl_rnd = 0, bl_ms = 0;
            {
                g_progress.config("logN=" + std::to_string(logN) + " " + label + " base");
                auto omap = setup_standalone(cfg, be, N);
                ZipfSampler z(N, cfg.s, 42);
                for (int i = 0; i < cfg.warmup; ++i) omap->search(z.sample());
                for (int i = 0; i < Q; ++i) {
                    auto t0 = Clock::now();
                    omap->search(z.sample());
                    bl_ms += std::chrono::duration<double, std::milli>(
                        Clock::now() - t0).count();
                    bl_bw += omap->last_stats().total_bytes();
                    bl_rnd += omap->last_stats().rounds;
                    g_progress.query_tick(i, Q);
                }
                bl_bw /= Q; bl_rnd /= Q; bl_ms /= Q;
                std::ostringstream ss;
                ss << std::fixed << std::setprecision(0) << bl_bw/1024 << "KB "
                   << (int)bl_rnd << "rnd " << std::setprecision(0) << bl_ms << "ms";
                g_progress.config_done(ss.str());
            }

            // ── (B) TieredOMAP TM mode — hot/cold separated ──
            double hot_bw = 0, cold_bw = 0;
            double hot_rnd = 0, cold_rnd = 0;
            double hot_ms = 0, cold_ms = 0;
            int hot_cnt = 0, cold_cnt = 0;
            bool da_hot = (be == OmapBackend::DaBplus);
            {
                g_progress.config("logN=" + std::to_string(logN) + " " + label + " TM");
                auto tm = setup_tiered(cfg, be, N, n,
                                       SecurityMode::TierMembership, true,
                                       0, OmapBackend::BPlus, da_hot);
                ZipfSampler z(N, cfg.s, 42);
                for (int i = 0; i < cfg.warmup; ++i) tm->access(z.sample());
                for (int i = 0; i < Q; ++i) {
                    auto t0 = Clock::now();
                    auto r = tm->access(z.sample());
                    double elapsed = std::chrono::duration<double, std::milli>(
                        Clock::now() - t0).count();
                    double ans_frac = (r.total_bw.rounds > 0)
                        ? (double)r.rounds_to_answer / r.total_bw.rounds : 1.0;
                    if (r.found_in_hot) {
                        hot_bw += r.total_bw.total_bytes();
                        hot_rnd += r.rounds_to_answer;
                        hot_ms += elapsed * ans_frac;
                        ++hot_cnt;
                    } else {
                        cold_bw += r.total_bw.total_bytes();
                        cold_rnd += r.total_bw.rounds;
                        cold_ms += elapsed;
                        ++cold_cnt;
                    }
                    g_progress.query_tick(i, Q);
                }
                if (hot_cnt > 0) { hot_bw /= hot_cnt; hot_rnd /= hot_cnt; hot_ms /= hot_cnt; }
                if (cold_cnt > 0) { cold_bw /= cold_cnt; cold_rnd /= cold_cnt; cold_ms /= cold_cnt; }
                double hit = 100.0 * hot_cnt / Q;
                std::ostringstream ss;
                ss << "hit=" << std::fixed << std::setprecision(0) << hit << "% "
                   << "hot:" << (int)(hot_bw/1024) << "KB/" << (int)hot_rnd << "rnd/"
                   << std::setprecision(0) << hot_ms << "ms  "
                   << "cold:" << (int)(cold_bw/1024) << "KB/" << (int)cold_rnd << "rnd/"
                   << std::setprecision(0) << cold_ms << "ms";
                g_progress.config_done(ss.str());
            }

            // ── (C) TieredOMAP FO mode — bandwidth only ──
            double fo_bw = 0;
            {
                g_progress.config("logN=" + std::to_string(logN) + " " + label + " FO");
                auto fo = setup_tiered(cfg, be, N, n,
                                       SecurityMode::FullOblivious, true,
                                       0, OmapBackend::BPlus, da_hot);
                ZipfSampler z(N, cfg.s, 42);
                for (int i = 0; i < cfg.warmup; ++i) fo->access(z.sample());
                for (int i = 0; i < Q; ++i) {
                    auto r = fo->access(z.sample());
                    fo_bw += r.total_bw.total_bytes();
                    g_progress.query_tick(i, Q);
                }
                fo_bw /= Q;
                std::ostringstream ss;
                ss << std::fixed << std::setprecision(0) << fo_bw/1024 << "KB";
                g_progress.config_done(ss.str());
            }

            // ── Write CSV row ──
            double hit_pct = (hot_cnt + cold_cnt > 0)
                ? 100.0 * hot_cnt / (hot_cnt + cold_cnt) : 0;
            double mean_bw = hit_pct/100 * hot_bw + (1 - hit_pct/100) * cold_bw;
            double mean_rnd = hit_pct/100 * hot_rnd + (1 - hit_pct/100) * cold_rnd;
            double mean_ms = hit_pct/100 * hot_ms + (1 - hit_pct/100) * cold_ms;

            csv << logN << "," << label << ","
                << std::fixed << std::setprecision(1) << hit_pct << ","
                << std::setprecision(0) << bl_bw/1024 << ","
                << std::setprecision(0) << bl_rnd << ","
                << std::setprecision(0) << bl_ms << ","
                << std::setprecision(0) << hot_bw/1024 << ","
                << std::setprecision(0) << cold_bw/1024 << ","
                << std::setprecision(0) << mean_bw/1024 << ","
                << std::setprecision(0) << fo_bw/1024 << ","
                << std::setprecision(0) << hot_rnd << ","
                << std::setprecision(0) << cold_rnd << ","
                << std::setprecision(0) << mean_rnd << ","
                << std::setprecision(0) << hot_ms << ","
                << std::setprecision(0) << cold_ms << ","
                << std::setprecision(0) << mean_ms << "\n";
            csv.flush();
        }
    }
    csv.close();
    std::cout << "  -> " << cfg.outdir << "/paper_wan.csv\n";
}

// ═══════════════════════════════════════════════════════════════════════════
// Exp: Profile per-(backend, N, n) round counts and latency
//   Measure deterministic round/time metrics once per configuration.
//   Python script then combines with theoretical Zipf hit rates
//   to compute reduction ratios for all (s, n) combinations.
// ═══════════════════════════════════════════════════════════════════════════

static void exp_profile(const Cfg& cfg) {
    std::cout << "\n=== Exp: Profile rounds & latency per (backend, N, n) ===\n";
    ensure_dir(cfg.outdir);
    int N = 1 << cfg.max_logN;
    int Q = cfg.Q;
    int rtt_us = cfg.rtt_us;
    if (rtt_us > 0)
        std::cout << "  RTT simulation: " << rtt_us << " us per round\n";

    std::ofstream csv(cfg.outdir + "/profile.csv");
    csv << "backend,logN,log_n,base_rnd,base_ms,"
        << "hot_rnd,hot_ms,cold_rnd,cold_ms,"
        << "base_bw_KB,hot_bw_KB,cold_bw_KB\n";

    struct ProfileSpec {
        const char* label;
        OmapBackend baseline_be;
        OmapBackend cold_be;
        OmapBackend hot_be;
        bool use_hot_be;
    };
    static const ProfileSpec PROFILE_BE[] = {
        {"AVL",     OmapBackend::AVL,    OmapBackend::AVL,    OmapBackend::AVL,   false},
        {"BPlus",   OmapBackend::BPlus,  OmapBackend::BPlus,  OmapBackend::BPlus, false},
        {"DaBplus", OmapBackend::DaBplus,OmapBackend::DaBplus, OmapBackend::BPlus, true},
    };

    std::vector<int> skew_n = {1024, 4096, 16384};
    std::vector<int> full_n = {64, 256, 1024, 4096, 16384, 65536};

    for (auto& spec : PROFILE_BE) {
        const char* label = spec.label;
        OmapBackend be = spec.baseline_be;
        auto& n_vals = (be == OmapBackend::AVL) ? full_n : skew_n;

        // (A) Standalone baseline
        double bl_rnd = 0, bl_ms = 0, bl_bw = 0;
        {
            g_progress.config(std::string(label) + " baseline N=" + std::to_string(N));
            auto omap = setup_standalone(cfg, be, N);
            if (rtt_us > 0) omap->set_round_delay_us(rtt_us);
            ZipfSampler z(N, 1.0, 42);
            for (int i = 0; i < cfg.warmup; ++i) omap->search(z.sample());
            for (int i = 0; i < Q; ++i) {
                auto t0 = Clock::now();
                omap->search(z.sample());
                bl_ms += std::chrono::duration<double, std::milli>(
                    Clock::now() - t0).count();
                bl_rnd += omap->last_stats().rounds;
                bl_bw += omap->last_stats().total_bytes();
                g_progress.query_tick(i, Q);
            }
            bl_rnd /= Q; bl_ms /= Q; bl_bw /= Q;
            std::ostringstream ss;
            ss << std::fixed << std::setprecision(1)
               << bl_rnd << "rnd " << bl_ms << "ms";
            g_progress.config_done(ss.str());
        }

        // (B) Tiered for each n
        for (int n : n_vals) {
            if (n >= N / 2) continue;
            int log_n = (int)std::round(std::log2(n));
            g_progress.config(std::string(label) + " n=2^" + std::to_string(log_n));

            auto tm = setup_tiered(cfg, spec.cold_be, N, n,
                                   SecurityMode::TierMembership, true,
                                   0, spec.hot_be, spec.use_hot_be);
            if (rtt_us > 0) tm->set_round_delay_us(rtt_us);
            ZipfSampler z(N, 1.0, 42);
            for (int i = 0; i < cfg.warmup; ++i) tm->access(z.sample());

            double h_rnd = 0, h_ms = 0, h_bw = 0;
            double c_rnd = 0, c_ms = 0, c_bw = 0;
            int h_cnt = 0, c_cnt = 0;

            for (int i = 0; i < Q; ++i) {
                auto t0 = Clock::now();
                auto r = tm->access(z.sample());
                double elapsed = std::chrono::duration<double, std::milli>(
                    Clock::now() - t0).count();

                double ans_frac = (r.total_bw.rounds > 0)
                    ? (double)r.rounds_to_answer / r.total_bw.rounds : 1.0;

                if (r.found_in_hot) {
                    h_rnd += r.rounds_to_answer;
                    h_ms += elapsed * ans_frac;
                    h_bw += r.total_bw.total_bytes();
                    ++h_cnt;
                } else {
                    c_rnd += r.total_bw.rounds;
                    c_ms += elapsed;
                    c_bw += r.total_bw.total_bytes();
                    ++c_cnt;
                }
                g_progress.query_tick(i, Q);
            }

            double hot_rnd = h_cnt > 0 ? h_rnd / h_cnt : 0;
            double hot_ms  = h_cnt > 0 ? h_ms / h_cnt : 0;
            double hot_bw  = h_cnt > 0 ? h_bw / h_cnt : 0;
            double cold_rnd = c_cnt > 0 ? c_rnd / c_cnt : 0;
            double cold_ms  = c_cnt > 0 ? c_ms / c_cnt : 0;
            double cold_bw  = c_cnt > 0 ? c_bw / c_cnt : 0;

            csv << label << "," << cfg.max_logN << "," << log_n << ","
                << std::fixed << std::setprecision(2)
                << bl_rnd << "," << bl_ms << ","
                << hot_rnd << "," << hot_ms << ","
                << cold_rnd << "," << cold_ms << ","
                << bl_bw/1024 << "," << hot_bw/1024 << ","
                << cold_bw/1024 << "\n";
            csv.flush();

            std::ostringstream ss;
            ss << "h=" << h_cnt << " c=" << c_cnt
               << " h_rnd=" << std::fixed << std::setprecision(1) << hot_rnd
               << " c_rnd=" << cold_rnd
               << " h_ms=" << std::setprecision(0) << hot_ms
               << " c_ms=" << cold_ms;
            g_progress.config_done(ss.str());
        }
    }
    csv.close();
    std::cout << "  -> " << cfg.outdir << "/profile.csv\n";
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

    struct ExpEntry { const char* name; void (*fn)(const Cfg&); };
    ExpEntry exps[] = {
        {"bandwidth",       exp_bandwidth},
        {"modes",           exp_modes},
        {"backend_cmp",     exp_backend_cmp},
        {"tiered_backend",  exp_tiered_backend},
        {"latency",         exp_latency},
        {"throughput",      exp_throughput},
        {"skewness",        exp_skewness},
        {"hotsize",         exp_hotsize},
        {"split_ablation",  exp_split_ablation},
        {"dynamic",         exp_dynamic},
        {"drift",           exp_drift},
        {"wan_static",      exp_wan_static},
        {"wan_dynamic",     exp_wan_dynamic},
        {"paper_wan",       exp_paper_wan},
        {"profile",         exp_profile},
    };

    bool all = (cfg.exp == "all");
    int total = 0;
    for (auto& e : exps) if (all || cfg.exp == e.name) ++total;
    g_progress.exp_total = total;

    for (auto& e : exps) {
        if (all || cfg.exp == e.name) {
            g_progress.begin_exp(e.name);
            e.fn(cfg);
        }
    }

    auto elapsed = std::chrono::duration<double>(Clock::now() - g_progress.run_start).count();
    int m = (int)(elapsed / 60), s = (int)elapsed % 60;
    std::cout << "\n=== All done in " << m << "m" << s << "s. CSV files in: "
              << cfg.outdir << "/ ===\n";
    return 0;
}
