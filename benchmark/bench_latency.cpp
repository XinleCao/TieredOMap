#include "tiered_omap/tiered_omap.h"
#include "tiered_omap/omap/avl_omap.h"
#include "tiered_omap/workload.h"
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

using namespace tiered_omap;
using Clock = std::chrono::high_resolution_clock;

struct Config {
    std::string exp = "all";
    int N = 0;
    int n = 0;
    double s = 0;
    int Q = 500;
    int warmup = 200;
    int rtt_us = 0;        // per-round RTT in microseconds
    double write_ratio = 0.2;
    std::string outdir = "results";
};

Config parse_args(int argc, char** argv) {
    Config c;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto eq = a.find('=');
        if (eq == std::string::npos) continue;
        std::string key = a.substr(0, eq);
        std::string val = a.substr(eq + 1);
        if (key == "--exp") c.exp = val;
        else if (key == "--N") c.N = std::stoi(val);
        else if (key == "--n") c.n = std::stoi(val);
        else if (key == "--s") c.s = std::stod(val);
        else if (key == "--Q") c.Q = std::stoi(val);
        else if (key == "--warmup") c.warmup = std::stoi(val);
        else if (key == "--rtt_us") c.rtt_us = std::stoi(val);
        else if (key == "--rtt_ms") c.rtt_us = static_cast<int>(std::stod(val) * 1000);
        else if (key == "--write_ratio") c.write_ratio = std::stod(val);
        else if (key == "--outdir") c.outdir = val;
    }
    return c;
}

// ── Helpers ──────────────────────────────────────────────────────────────────

std::vector<std::pair<int, Bytes>> make_data(int N) {
    std::vector<std::pair<int, Bytes>> data;
    data.reserve(N);
    for (int i = 0; i < N; ++i) data.emplace_back(i, int_to_bytes(i));
    return data;
}

std::vector<int> make_hot_keys(int n) {
    std::vector<int> hk(n);
    std::iota(hk.begin(), hk.end(), 0);
    return hk;
}

struct LatencyResult {
    double avg_total_us = 0;
    double avg_ans_us = 0;
    double avg_bw = 0;
    double avg_rounds = 0;
    double avg_ans_rounds = 0;
    double hot_hit_pct = 0;
};

LatencyResult run_latency(TieredOMap& tm, int N, int n, double alpha,
                          int Q, int warmup, double write_ratio,
                          uint64_t seed = 42) {
    ZipfSampler z(N, alpha, seed);
    std::mt19937_64 rng(seed + 1);
    std::uniform_real_distribution<> urd(0, 1);

    for (int q = 0; q < warmup; ++q) {
        int k = z.sample();
        Bytes wval;
        const Bytes* wptr = nullptr;
        if (urd(rng) < write_ratio) {
            wval = int_to_bytes(k + 1000000);
            wptr = &wval;
        }
        tm.access(k, wptr);
    }

    double tot_total_us = 0, tot_ans_us = 0, tot_bw = 0;
    double tot_rounds = 0, tot_ans_rounds = 0;
    int hot_hits = 0;

    for (int q = 0; q < Q; ++q) {
        int k = z.sample();
        Bytes wval;
        const Bytes* wptr = nullptr;
        if (urd(rng) < write_ratio) {
            wval = int_to_bytes(k + 1000000);
            wptr = &wval;
        }
        auto t0 = Clock::now();
        auto r = tm.access(k, wptr);
        auto t1 = Clock::now();

        double elapsed_us = std::chrono::duration<double, std::micro>(t1 - t0).count();
        tot_total_us += elapsed_us;
        double ans_frac = (r.total_bw.rounds > 0)
            ? static_cast<double>(r.rounds_to_answer) / r.total_bw.rounds
            : 1.0;
        tot_ans_us += elapsed_us * ans_frac;
        tot_bw += r.total_bw.total_bytes();
        tot_rounds += r.total_bw.rounds;
        tot_ans_rounds += r.rounds_to_answer;
        if (r.found_in_hot) ++hot_hits;
    }
    return {tot_total_us / Q, tot_ans_us / Q, tot_bw / Q,
            tot_rounds / Q, tot_ans_rounds / Q, 100.0 * hot_hits / Q};
}

LatencyResult run_baseline_latency(AVLOmap& bl, int N, double alpha,
                                   int Q, int warmup, double write_ratio,
                                   uint64_t seed = 42) {
    ZipfSampler z(N, alpha, seed);
    std::mt19937_64 rng(seed + 1);
    std::uniform_real_distribution<> urd(0, 1);

    for (int q = 0; q < warmup; ++q) {
        int k = z.sample();
        Bytes wval;
        const Bytes* wptr = nullptr;
        if (urd(rng) < write_ratio) {
            wval = int_to_bytes(k + 1000000);
            wptr = &wval;
        }
        bl.search(k, wptr);
    }

    double tot_us = 0, tot_bw = 0, tot_rounds = 0;
    for (int q = 0; q < Q; ++q) {
        int k = z.sample();
        Bytes wval;
        const Bytes* wptr = nullptr;
        if (urd(rng) < write_ratio) {
            wval = int_to_bytes(k + 1000000);
            wptr = &wval;
        }
        auto t0 = Clock::now();
        bl.search(k, wptr);
        auto t1 = Clock::now();
        tot_us += std::chrono::duration<double, std::micro>(t1 - t0).count();
        tot_bw += bl.last_stats().total_bytes();
        tot_rounds += bl.last_stats().rounds;
    }
    return {tot_us / Q, tot_us / Q, tot_bw / Q, tot_rounds / Q, tot_rounds / Q, 0};
}

// ── Paper Figure: Bandwidth vs N (fig:exp-bandwidth) ────────────────────────

void exp_bandwidth(const Config& cfg) {
    std::vector<int> Ns = {1<<12, 1<<14, 1<<16, 1<<18, 1<<20};
    int n = (cfg.n > 0) ? cfg.n : 1024;
    double s = (cfg.s > 0) ? cfg.s : 1.0;

    std::cout << "=== fig:exp-bandwidth  Bandwidth vs N "
              << "(n=" << n << " s=" << s << " Q=" << cfg.Q << ") ===\n\n";

    std::cout << std::left
              << std::setw(8)  << "log2N"
              << std::setw(10) << "BL(KB)"
              << std::setw(12) << "NoSplit(KB)"
              << std::setw(10) << "TM(KB)"
              << std::setw(10) << "FO(KB)"
              << std::setw(8)  << "hit%"
              << std::setw(10) << "BL_rnd"
              << std::setw(10) << "TM_rnd"
              << std::setw(10) << "TM_ans"
              << "\n" << std::string(88, '-') << "\n";

    for (int N : Ns) {
        int actual_n = std::min(n, N / 2);
        auto data = make_data(N);
        auto hk = make_hot_keys(actual_n);

        AVLOmap bl(N);
        bl.init(data);
        auto bl_r = run_baseline_latency(bl, N, s, cfg.Q, cfg.warmup, cfg.write_ratio);

        TieredOMapConfig ns_cfg;
        ns_cfg.total_keys = N; ns_cfg.hot_set_size = actual_n;
        ns_cfg.mode = SecurityMode::FullOblivious;
        ns_cfg.use_split_oram = false;
        TieredOMap ns_tm(ns_cfg);
        ns_tm.init(data, hk);
        auto ns_r = run_latency(ns_tm, N, actual_n, s, cfg.Q, cfg.warmup, cfg.write_ratio);

        TieredOMapConfig tm_cfg;
        tm_cfg.total_keys = N; tm_cfg.hot_set_size = actual_n;
        tm_cfg.mode = SecurityMode::TierMembership;
        tm_cfg.use_split_oram = true;
        TieredOMap tm(tm_cfg);
        tm.init(data, hk);
        auto tm_r = run_latency(tm, N, actual_n, s, cfg.Q, cfg.warmup, cfg.write_ratio);

        TieredOMapConfig fo_cfg;
        fo_cfg.total_keys = N; fo_cfg.hot_set_size = actual_n;
        fo_cfg.mode = SecurityMode::FullOblivious;
        fo_cfg.use_split_oram = true;
        TieredOMap fo(fo_cfg);
        fo.init(data, hk);
        auto fo_r = run_latency(fo, N, actual_n, s, cfg.Q, cfg.warmup, cfg.write_ratio);

        int logN = static_cast<int>(std::log2(N));
        std::cout << std::setw(8) << logN
                  << std::setw(10) << static_cast<int>(bl_r.avg_bw / 1024)
                  << std::setw(12) << static_cast<int>(ns_r.avg_bw / 1024)
                  << std::setw(10) << static_cast<int>(tm_r.avg_bw / 1024)
                  << std::setw(10) << static_cast<int>(fo_r.avg_bw / 1024)
                  << std::setw(8)  << static_cast<int>(tm_r.hot_hit_pct)
                  << std::setw(10) << std::setprecision(1) << bl_r.avg_rounds
                  << std::setw(10) << tm_r.avg_rounds
                  << std::setw(10) << tm_r.avg_ans_rounds
                  << "\n";
    }
    std::cout << "\n";
}

// ── Paper Figure: Skewness (fig:exp-skewness) ───────────────────────────────

void exp_skewness(const Config& cfg) {
    std::vector<double> ss = {0.5, 0.8, 1.0, 1.2, 1.5};
    int N = (cfg.N > 0) ? cfg.N : (1 << 16);
    int n = (cfg.n > 0) ? cfg.n : 1024;

    std::cout << "=== fig:exp-skewness  Skewness sweep "
              << "(N=" << N << " n=" << n << " Q=" << cfg.Q << ") ===\n\n";

    auto data = make_data(N);
    auto hk = make_hot_keys(n);

    AVLOmap bl(N);
    bl.init(data);

    std::cout << std::left
              << std::setw(6) << "s"
              << std::setw(8) << "hit%"
              << std::setw(12) << "BL(KB)"
              << std::setw(12) << "TM(KB)"
              << std::setw(10) << "speedup"
              << "\n" << std::string(48, '-') << "\n";

    for (double s : ss) {
        auto bl_r = run_baseline_latency(bl, N, s, cfg.Q, cfg.warmup, cfg.write_ratio);

        TieredOMapConfig tm_cfg;
        tm_cfg.total_keys = N; tm_cfg.hot_set_size = n;
        tm_cfg.mode = SecurityMode::TierMembership;
        tm_cfg.use_split_oram = true;
        TieredOMap tm(tm_cfg);
        tm.init(data, hk);
        auto tm_r = run_latency(tm, N, n, s, cfg.Q, cfg.warmup, cfg.write_ratio);

        double speedup = bl_r.avg_bw / tm_r.avg_bw;
        std::cout << std::setw(6) << std::setprecision(1) << std::fixed << s
                  << std::setw(8) << static_cast<int>(tm_r.hot_hit_pct)
                  << std::setw(12) << static_cast<int>(bl_r.avg_bw / 1024)
                  << std::setw(12) << static_cast<int>(tm_r.avg_bw / 1024)
                  << std::setw(10) << std::setprecision(2) << speedup
                  << "\n";
    }
    std::cout << "\n";
}

// ── Paper Figure: Hot-Set Size (fig:exp-hotsize) ────────────────────────────

void exp_hotsize(const Config& cfg) {
    std::vector<int> ns = {1<<4, 1<<5, 1<<6, 1<<7, 1<<8, 1<<9, 1<<10, 1<<11, 1<<12};
    int N = (cfg.N > 0) ? cfg.N : (1 << 16);
    double s = (cfg.s > 0) ? cfg.s : 1.0;

    std::cout << "=== fig:exp-hotsize  Hot-set size sweep "
              << "(N=" << N << " s=" << s << " Q=" << cfg.Q << ") ===\n\n";

    auto data = make_data(N);

    AVLOmap bl(N);
    bl.init(data);
    auto bl_r = run_baseline_latency(bl, N, s, cfg.Q, cfg.warmup, cfg.write_ratio);

    std::cout << std::left
              << std::setw(8) << "log2n"
              << std::setw(12) << "BL(KB)"
              << std::setw(12) << "Full(KB)"
              << std::setw(8)  << "hit%"
              << "\n" << std::string(40, '-') << "\n";

    for (int n : ns) {
        if (n >= N) continue;
        auto hk = make_hot_keys(n);

        TieredOMapConfig tm_cfg;
        tm_cfg.total_keys = N; tm_cfg.hot_set_size = n;
        tm_cfg.mode = SecurityMode::TierMembership;
        tm_cfg.use_split_oram = true;
        TieredOMap tm(tm_cfg);
        tm.init(data, hk);
        auto tm_r = run_latency(tm, N, n, s, cfg.Q, cfg.warmup, cfg.write_ratio);

        int logn = static_cast<int>(std::log2(n));
        std::cout << std::setw(8) << logn
                  << std::setw(12) << static_cast<int>(bl_r.avg_bw / 1024)
                  << std::setw(12) << static_cast<int>(tm_r.avg_bw / 1024)
                  << std::setw(8)  << static_cast<int>(tm_r.hot_hit_pct)
                  << "\n";
    }
    std::cout << "\n";
}

// ── Paper Figure: Client-perceived latency (fig:exp-latency) ────────────────

void exp_latency(const Config& cfg) {
    int N = (cfg.N > 0) ? cfg.N : (1 << 16);
    int n = (cfg.n > 0) ? cfg.n : 1024;
    double s = (cfg.s > 0) ? cfg.s : 1.0;

    struct Scenario { const char* name; int rtt_us; };
    std::vector<Scenario> scenarios = {
        {"LAN",    100},
        {"WAN-30", 30000},
        {"WAN-80", 80000},
    };

    std::cout << "=== fig:exp-latency  Client-perceived latency "
              << "(N=" << N << " n=" << n << " s=" << s
              << " Q=" << cfg.Q << ") ===\n\n";

    auto data = make_data(N);
    auto hk = make_hot_keys(n);

    std::cout << std::left
              << std::setw(10) << "Network"
              << std::setw(16) << "SingleOMAP(ms)"
              << std::setw(14) << "TM_ans(ms)"
              << std::setw(14) << "FO_ans(ms)"
              << std::setw(14) << "FO_total(ms)"
              << "\n" << std::string(68, '-') << "\n";

    for (auto& sc : scenarios) {
        AVLOmap bl(N);
        bl.init(data);
        bl.set_round_delay_us(sc.rtt_us);
        auto bl_r = run_baseline_latency(bl, N, s, cfg.Q, cfg.warmup, cfg.write_ratio);

        TieredOMapConfig tm_cfg;
        tm_cfg.total_keys = N; tm_cfg.hot_set_size = n;
        tm_cfg.mode = SecurityMode::TierMembership;
        tm_cfg.use_split_oram = true;
        TieredOMap tm(tm_cfg);
        tm.init(data, hk);
        tm.set_round_delay_us(sc.rtt_us);
        auto tm_r = run_latency(tm, N, n, s, cfg.Q, cfg.warmup, cfg.write_ratio);

        TieredOMapConfig fo_cfg;
        fo_cfg.total_keys = N; fo_cfg.hot_set_size = n;
        fo_cfg.mode = SecurityMode::FullOblivious;
        fo_cfg.use_split_oram = true;
        TieredOMap fo(fo_cfg);
        fo.init(data, hk);
        fo.set_round_delay_us(sc.rtt_us);
        auto fo_r = run_latency(fo, N, n, s, cfg.Q, cfg.warmup, cfg.write_ratio);

        std::cout << std::setw(10) << sc.name
                  << std::setw(16) << std::setprecision(1) << std::fixed << bl_r.avg_total_us / 1000.0
                  << std::setw(14) << tm_r.avg_ans_us / 1000.0
                  << std::setw(14) << fo_r.avg_ans_us / 1000.0
                  << std::setw(14) << fo_r.avg_total_us / 1000.0
                  << "\n";
    }
    std::cout << "\n";
}

// ── Paper Table: Mode comparison (tab:exp-modes) ────────────────────────────

void exp_modes(const Config& cfg) {
    int N = (cfg.N > 0) ? cfg.N : (1 << 16);
    int n = (cfg.n > 0) ? cfg.n : 1024;
    double s = (cfg.s > 0) ? cfg.s : 1.0;

    struct Scenario { const char* name; int rtt_us; };
    std::vector<Scenario> scenarios = {
        {"LAN",    100},
        {"WAN-80", 80000},
    };

    std::cout << "=== tab:exp-modes  Mode comparison "
              << "(N=" << N << " n=" << n << " s=" << s
              << " Q=" << cfg.Q << ") ===\n\n";

    auto data = make_data(N);
    auto hk = make_hot_keys(n);

    std::cout << std::left
              << std::setw(10) << "Setting"
              << std::setw(16) << "Mode"
              << std::setw(10) << "BW(KB)"
              << std::setw(14) << "Latency(ms)"
              << std::setw(14) << "Answer(ms)"
              << "\n" << std::string(64, '-') << "\n";

    for (auto& sc : scenarios) {
        AVLOmap bl(N);
        bl.init(data);
        bl.set_round_delay_us(sc.rtt_us);
        auto bl_r = run_baseline_latency(bl, N, s, cfg.Q, cfg.warmup, cfg.write_ratio);

        TieredOMapConfig tm_cfg;
        tm_cfg.total_keys = N; tm_cfg.hot_set_size = n;
        tm_cfg.mode = SecurityMode::TierMembership;
        tm_cfg.use_split_oram = true;
        TieredOMap tm(tm_cfg);
        tm.init(data, hk);
        tm.set_round_delay_us(sc.rtt_us);
        auto tm_r = run_latency(tm, N, n, s, cfg.Q, cfg.warmup, cfg.write_ratio);

        TieredOMapConfig fo_cfg;
        fo_cfg.total_keys = N; fo_cfg.hot_set_size = n;
        fo_cfg.mode = SecurityMode::FullOblivious;
        fo_cfg.use_split_oram = true;
        TieredOMap fo(fo_cfg);
        fo.init(data, hk);
        fo.set_round_delay_us(sc.rtt_us);
        auto fo_r = run_latency(fo, N, n, s, cfg.Q, cfg.warmup, cfg.write_ratio);

        std::cout << std::setw(10) << sc.name
                  << std::setw(16) << "Single OMAP"
                  << std::setw(10) << static_cast<int>(bl_r.avg_bw / 1024)
                  << std::setw(14) << std::setprecision(1) << std::fixed << bl_r.avg_total_us / 1000.0
                  << std::setw(14) << bl_r.avg_total_us / 1000.0
                  << "\n";
        std::cout << std::setw(10) << ""
                  << std::setw(16) << "Tier-memb."
                  << std::setw(10) << static_cast<int>(tm_r.avg_bw / 1024)
                  << std::setw(14) << tm_r.avg_total_us / 1000.0
                  << std::setw(14) << tm_r.avg_ans_us / 1000.0
                  << "\n";
        std::cout << std::setw(10) << ""
                  << std::setw(16) << "Full obliv."
                  << std::setw(10) << static_cast<int>(fo_r.avg_bw / 1024)
                  << std::setw(14) << fo_r.avg_total_us / 1000.0
                  << std::setw(14) << fo_r.avg_ans_us / 1000.0
                  << "\n";
        if (sc.rtt_us != scenarios.back().rtt_us)
            std::cout << std::string(64, '-') << "\n";
    }
    std::cout << "\n";
}

// ── Paper Table: Split-ORAM ablation (tab:exp-split) ────────────────────────

void exp_ablation(const Config& cfg) {
    std::vector<int> Ns = {1<<14, 1<<16, 1<<18, 1<<20};
    int n = (cfg.n > 0) ? cfg.n : 1024;
    double s = (cfg.s > 0) ? cfg.s : 1.0;

    std::cout << "=== tab:exp-split  Split-ORAM ablation "
              << "(n=" << n << " s=" << s << " Q=" << cfg.Q << ") ===\n\n";

    std::cout << std::left
              << std::setw(10) << "N"
              << std::setw(14) << "NoSplit(KB)"
              << std::setw(12) << "Full(KB)"
              << std::setw(12) << "Improvement"
              << "\n" << std::string(48, '-') << "\n";

    for (int N : Ns) {
        int actual_n = std::min(n, N / 2);
        auto data = make_data(N);
        auto hk = make_hot_keys(actual_n);

        TieredOMapConfig ns_cfg;
        ns_cfg.total_keys = N; ns_cfg.hot_set_size = actual_n;
        ns_cfg.mode = SecurityMode::TierMembership;
        ns_cfg.use_split_oram = false;
        TieredOMap ns_tm(ns_cfg);
        ns_tm.init(data, hk);
        auto ns_r = run_latency(ns_tm, N, actual_n, s, cfg.Q, cfg.warmup, cfg.write_ratio);

        TieredOMapConfig sp_cfg;
        sp_cfg.total_keys = N; sp_cfg.hot_set_size = actual_n;
        sp_cfg.mode = SecurityMode::TierMembership;
        sp_cfg.use_split_oram = true;
        TieredOMap sp_tm(sp_cfg);
        sp_tm.init(data, hk);
        auto sp_r = run_latency(sp_tm, N, actual_n, s, cfg.Q, cfg.warmup, cfg.write_ratio);

        double imp = ns_r.avg_bw / sp_r.avg_bw;
        std::cout << std::setw(10) << N
                  << std::setw(14) << static_cast<int>(ns_r.avg_bw / 1024)
                  << std::setw(12) << static_cast<int>(sp_r.avg_bw / 1024)
                  << std::setw(12) << std::setprecision(2) << std::fixed << imp << "x"
                  << "\n";
    }
    std::cout << "\n";
}

// ── Paper Table: Read vs Write (tab:exp-write) ──────────────────────────────

void exp_write(const Config& cfg) {
    int N = (cfg.N > 0) ? cfg.N : (1 << 16);
    int n = (cfg.n > 0) ? cfg.n : 1024;
    double s = (cfg.s > 0) ? cfg.s : 1.0;

    std::cout << "=== tab:exp-write  Read vs Write cost "
              << "(N=" << N << " n=" << n << " Q=" << cfg.Q << ") ===\n\n";

    auto data = make_data(N);
    auto hk = make_hot_keys(n);

    auto measure_rw = [&](const std::string& label, auto make_instance) {
        auto inst_r = make_instance();
        ZipfSampler zr(N, s, 42);
        double r_bw = 0;
        int r_cnt = 0;
        for (int q = 0; q < cfg.warmup; ++q) { inst_r->access(zr.sample()); }
        for (int q = 0; q < cfg.Q; ++q) {
            auto res = inst_r->access(zr.sample());
            r_bw += res.total_bw.total_bytes();
            ++r_cnt;
        }

        auto inst_w = make_instance();
        ZipfSampler zw(N, s, 99);
        double w_bw = 0;
        int w_cnt = 0;
        for (int q = 0; q < cfg.warmup; ++q) {
            int k = zw.sample();
            Bytes v = int_to_bytes(k + 1000000);
            inst_w->access(k, &v);
        }
        for (int q = 0; q < cfg.Q; ++q) {
            int k = zw.sample();
            Bytes v = int_to_bytes(k + 1000000);
            auto res = inst_w->access(k, &v);
            w_bw += res.total_bw.total_bytes();
            ++w_cnt;
        }
        std::cout << std::setw(20) << label
                  << std::setw(12) << static_cast<int>(r_bw / r_cnt / 1024)
                  << std::setw(12) << static_cast<int>(w_bw / w_cnt / 1024)
                  << "\n";
    };

    std::cout << std::left
              << std::setw(20) << "Config"
              << std::setw(12) << "Read(KB)"
              << std::setw(12) << "Write(KB)"
              << "\n" << std::string(44, '-') << "\n";

    measure_rw("Single OMAP", [&]() -> std::unique_ptr<TieredOMap> {
        TieredOMapConfig c;
        c.total_keys = N; c.hot_set_size = 0;
        c.mode = SecurityMode::FullOblivious;
        auto t = std::make_unique<TieredOMap>(c);
        std::vector<int> empty;
        t->init(data, empty);
        return t;
    });

    measure_rw("TM (split)", [&]() -> std::unique_ptr<TieredOMap> {
        TieredOMapConfig c;
        c.total_keys = N; c.hot_set_size = n;
        c.mode = SecurityMode::TierMembership;
        c.use_split_oram = true;
        auto t = std::make_unique<TieredOMap>(c);
        t->init(data, hk);
        return t;
    });

    measure_rw("FO (split)", [&]() -> std::unique_ptr<TieredOMap> {
        TieredOMapConfig c;
        c.total_keys = N; c.hot_set_size = n;
        c.mode = SecurityMode::FullOblivious;
        c.use_split_oram = true;
        auto t = std::make_unique<TieredOMap>(c);
        t->init(data, hk);
        return t;
    });
    std::cout << "\n";
}

// ── Main ─────────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    Config cfg = parse_args(argc, argv);
    std::string mkdir_cmd = "mkdir -p " + cfg.outdir;
    system(mkdir_cmd.c_str());

    std::cout << std::fixed;

    auto should_run = [&](const std::string& name) {
        return cfg.exp == "all" || cfg.exp == name;
    };

    if (should_run("bandwidth")) exp_bandwidth(cfg);
    if (should_run("skewness"))  exp_skewness(cfg);
    if (should_run("hotsize"))   exp_hotsize(cfg);
    if (should_run("latency"))   exp_latency(cfg);
    if (should_run("modes"))     exp_modes(cfg);
    if (should_run("ablation"))  exp_ablation(cfg);
    if (should_run("write"))     exp_write(cfg);

    return 0;
}
