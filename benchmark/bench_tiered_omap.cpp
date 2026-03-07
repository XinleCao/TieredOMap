#include "tiered_omap/tiered_omap.h"
#include "tiered_omap/omap/avl_omap.h"
#include "tiered_omap/workload.h"
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

using namespace tiered_omap;

// ── CLI parsing ──────────────────────────────────────────────────────────────

struct Config {
    std::string exp = "all";
    int N = 0;
    int n = 0;
    double s = 0;
    int Q = 500;
    bool split = true;
    std::string mode = "fo";
    bool csv = false;
    std::string outdir = "results";
    double write_ratio = 0.2;
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
        else if (key == "--split") c.split = (val == "1" || val == "true");
        else if (key == "--mode") c.mode = val;
        else if (key == "--csv") c.csv = (val == "1" || val == "true");
        else if (key == "--outdir") c.outdir = val;
        else if (key == "--write_ratio") c.write_ratio = std::stod(val);
    }
    return c;
}

// ── Helpers ──────────────────────────────────────────────────────────────────

struct RunResult {
    double avg_rounds = 0;
    double avg_bw = 0;
    double avg_ans_rounds = 0;
    double hot_hit_pct = 0;
};

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

RunResult run_zipf(TieredOMap& tm, int N, int n, double alpha, int Q,
                   double write_ratio, uint64_t seed = 123) {
    ZipfSampler z(N, alpha, seed);
    std::mt19937_64 rng(seed + 1);
    std::uniform_real_distribution<> urd(0, 1);

    double tot_r = 0, tot_b = 0, tot_a = 0;
    int hot_hits = 0;
    for (int q = 0; q < Q; ++q) {
        int k = z.sample();
        Bytes wval;
        const Bytes* wptr = nullptr;
        if (urd(rng) < write_ratio) {
            wval = int_to_bytes(k + 1000000);
            wptr = &wval;
        }
        auto r = tm.access(k, wptr);
        tot_r += r.total_bw.rounds;
        tot_b += r.total_bw.total_bytes();
        tot_a += r.rounds_to_answer;
        if (r.found_in_hot) ++hot_hits;
    }
    return {tot_r / Q, tot_b / Q, tot_a / Q,
            100.0 * hot_hits / Q};
}

RunResult run_baseline_zipf(AVLOmap& bl, int N, double alpha, int Q,
                            double write_ratio, uint64_t seed = 123) {
    ZipfSampler z(N, alpha, seed);
    std::mt19937_64 rng(seed + 1);
    std::uniform_real_distribution<> urd(0, 1);

    double tot_r = 0, tot_b = 0;
    for (int q = 0; q < Q; ++q) {
        int k = z.sample();
        Bytes wval;
        const Bytes* wptr = nullptr;
        if (urd(rng) < write_ratio) {
            wval = int_to_bytes(k + 1000000);
            wptr = &wval;
        }
        bl.search(k, wptr);
        tot_r += bl.last_stats().rounds;
        tot_b += bl.last_stats().total_bytes();
    }
    return {tot_r / Q, tot_b / Q, tot_r / Q, 0};
}

// ── CSV writer ───────────────────────────────────────────────────────────────

class CsvWriter {
public:
    CsvWriter(const std::string& path) : out_(path) {
        if (!out_.is_open())
            std::cerr << "Warning: cannot open " << path << "\n";
    }
    void header(const std::vector<std::string>& cols) {
        for (size_t i = 0; i < cols.size(); ++i) {
            if (i) out_ << ",";
            out_ << cols[i];
        }
        out_ << "\n";
    }
    template <typename... Args>
    void row(Args&&... args) {
        write_vals(std::forward<Args>(args)...);
        out_ << "\n";
    }
private:
    template <typename T>
    void write_vals(T&& v) { out_ << v; }
    template <typename T, typename... Rest>
    void write_vals(T&& v, Rest&&... rest) {
        out_ << v << ",";
        write_vals(std::forward<Rest>(rest)...);
    }
    std::ofstream out_;
};

// ── Experiments ──────────────────────────────────────────────────────────────

void exp_bandwidth(const Config& cfg) {
    std::vector<int> Ns = {1<<12, 1<<14, 1<<16, 1<<18, 1<<20};
    int n = (cfg.n > 0) ? cfg.n : 1024;
    double s = (cfg.s > 0) ? cfg.s : 1.0;
    int Q = cfg.Q;

    std::cout << "=== Bandwidth vs N (n=" << n << ", s=" << s << ", Q=" << Q << ") ===\n\n";

    CsvWriter csv(cfg.outdir + "/bandwidth_vs_N.csv");
    csv.header({"N", "n", "s", "baseline_bw", "baseline_rnd",
                "fo_bw", "fo_rnd", "fo_ans_rnd",
                "tm_bw", "tm_rnd", "tm_ans_rnd",
                "fo_nosplit_bw", "fo_nosplit_rnd", "fo_nosplit_ans_rnd",
                "hit_pct"});

    std::cout << std::left << std::setw(8) << "N"
              << std::setw(10) << "BL_bw" << std::setw(8) << "BL_rnd"
              << std::setw(10) << "FO_bw" << std::setw(10) << "FO_ans"
              << std::setw(10) << "TM_bw" << std::setw(10) << "TM_rnd"
              << std::setw(10) << "NoSp_bw"
              << std::setw(8) << "hit%"
              << "\n" << std::string(84, '-') << "\n";

    for (int N : Ns) {
        int actual_n = std::min(n, N / 2);
        auto data = make_data(N);
        auto hk = make_hot_keys(actual_n);

        AVLOmap bl(N);
        bl.init(data);
        auto bl_r = run_baseline_zipf(bl, N, s, Q, cfg.write_ratio);

        TieredOMapConfig fo_cfg;
        fo_cfg.total_keys = N; fo_cfg.hot_set_size = actual_n;
        fo_cfg.mode = SecurityMode::FullOblivious;
        fo_cfg.use_split_oram = true;
        TieredOMap fo(fo_cfg);
        fo.init(data, hk);
        auto fo_r = run_zipf(fo, N, actual_n, s, Q, cfg.write_ratio);

        TieredOMapConfig tm_cfg;
        tm_cfg.total_keys = N; tm_cfg.hot_set_size = actual_n;
        tm_cfg.mode = SecurityMode::TierMembership;
        tm_cfg.use_split_oram = true;
        TieredOMap tm(tm_cfg);
        tm.init(data, hk);
        auto tm_r = run_zipf(tm, N, actual_n, s, Q, cfg.write_ratio);

        TieredOMapConfig ns_cfg;
        ns_cfg.total_keys = N; ns_cfg.hot_set_size = actual_n;
        ns_cfg.mode = SecurityMode::FullOblivious;
        ns_cfg.use_split_oram = false;
        TieredOMap ns_tm(ns_cfg);
        ns_tm.init(data, hk);
        auto ns_r = run_zipf(ns_tm, N, actual_n, s, Q, cfg.write_ratio);

        csv.row(N, actual_n, s,
                (int)bl_r.avg_bw, bl_r.avg_rounds,
                (int)fo_r.avg_bw, fo_r.avg_rounds, fo_r.avg_ans_rounds,
                (int)tm_r.avg_bw, tm_r.avg_rounds, tm_r.avg_ans_rounds,
                (int)ns_r.avg_bw, ns_r.avg_rounds, ns_r.avg_ans_rounds,
                fo_r.hot_hit_pct);

        std::cout << std::setw(8) << N
                  << std::setw(10) << (int)bl_r.avg_bw
                  << std::setw(8) << std::setprecision(1) << std::fixed << bl_r.avg_rounds
                  << std::setw(10) << (int)fo_r.avg_bw
                  << std::setw(10) << std::setprecision(1) << fo_r.avg_ans_rounds
                  << std::setw(10) << (int)tm_r.avg_bw
                  << std::setw(10) << std::setprecision(1) << tm_r.avg_rounds
                  << std::setw(10) << (int)ns_r.avg_bw
                  << std::setw(8) << std::setprecision(0) << fo_r.hot_hit_pct
                  << "\n";
    }
    std::cout << "\n";
}

void exp_skewness(const Config& cfg) {
    std::vector<double> ss = {0.5, 0.8, 1.0, 1.2, 1.5};
    int N = (cfg.N > 0) ? cfg.N : (1 << 16);
    int n = (cfg.n > 0) ? cfg.n : 1024;
    int Q = cfg.Q;

    std::cout << "=== Skewness (N=" << N << ", n=" << n << ", Q=" << Q << ") ===\n\n";

    CsvWriter csv(cfg.outdir + "/skewness.csv");
    csv.header({"s", "hit_pct", "baseline_bw", "fo_bw", "fo_ans_rnd",
                "tm_bw", "tm_rnd", "speedup_bw"});

    auto data = make_data(N);
    auto hk = make_hot_keys(n);

    AVLOmap bl(N);
    bl.init(data);

    for (double s : ss) {
        auto bl_r = run_baseline_zipf(bl, N, s, Q, cfg.write_ratio);

        TieredOMapConfig fo_cfg;
        fo_cfg.total_keys = N; fo_cfg.hot_set_size = n;
        fo_cfg.mode = SecurityMode::FullOblivious;
        fo_cfg.use_split_oram = true;
        TieredOMap fo(fo_cfg);
        fo.init(data, hk);
        auto fo_r = run_zipf(fo, N, n, s, Q, cfg.write_ratio);

        TieredOMapConfig tm_cfg;
        tm_cfg.total_keys = N; tm_cfg.hot_set_size = n;
        tm_cfg.mode = SecurityMode::TierMembership;
        tm_cfg.use_split_oram = true;
        TieredOMap tm(tm_cfg);
        tm.init(data, hk);
        auto tm_r = run_zipf(tm, N, n, s, Q, cfg.write_ratio);

        double speedup = bl_r.avg_bw / tm_r.avg_bw;
        double hit_pct = fo_r.hot_hit_pct;

        csv.row(s, hit_pct, (int)bl_r.avg_bw, (int)fo_r.avg_bw,
                fo_r.avg_ans_rounds, (int)tm_r.avg_bw, tm_r.avg_rounds,
                speedup);

        std::cout << "s=" << std::setw(4) << std::setprecision(1) << std::fixed << s
                  << "  hit=" << std::setw(5) << std::setprecision(0) << hit_pct << "%"
                  << "  BL=" << std::setw(8) << (int)bl_r.avg_bw
                  << "  TM=" << std::setw(8) << (int)tm_r.avg_bw
                  << "  speedup=" << std::setprecision(2) << speedup << "x"
                  << "\n";
    }
    std::cout << "\n";
}

void exp_hotsize(const Config& cfg) {
    std::vector<int> ns = {1<<4, 1<<5, 1<<6, 1<<7, 1<<8, 1<<9, 1<<10, 1<<11, 1<<12};
    int N = (cfg.N > 0) ? cfg.N : (1 << 16);
    double s = (cfg.s > 0) ? cfg.s : 1.0;
    int Q = cfg.Q;

    std::cout << "=== Hot-set size (N=" << N << ", s=" << s << ", Q=" << Q << ") ===\n\n";

    CsvWriter csv(cfg.outdir + "/hotsize.csv");
    csv.header({"n", "hit_pct", "baseline_bw", "tm_bw", "fo_bw", "fo_ans_rnd"});

    auto data = make_data(N);

    AVLOmap bl(N);
    bl.init(data);
    auto bl_r = run_baseline_zipf(bl, N, s, Q, cfg.write_ratio);

    for (int n : ns) {
        if (n >= N) continue;
        auto hk = make_hot_keys(n);

        TieredOMapConfig fo_cfg;
        fo_cfg.total_keys = N; fo_cfg.hot_set_size = n;
        fo_cfg.mode = SecurityMode::FullOblivious;
        fo_cfg.use_split_oram = true;
        TieredOMap fo(fo_cfg);
        fo.init(data, hk);
        auto fo_r = run_zipf(fo, N, n, s, Q, cfg.write_ratio);

        TieredOMapConfig tm_cfg;
        tm_cfg.total_keys = N; tm_cfg.hot_set_size = n;
        tm_cfg.mode = SecurityMode::TierMembership;
        tm_cfg.use_split_oram = true;
        TieredOMap tm(tm_cfg);
        tm.init(data, hk);
        auto tm_r = run_zipf(tm, N, n, s, Q, cfg.write_ratio);

        csv.row(n, fo_r.hot_hit_pct, (int)bl_r.avg_bw,
                (int)tm_r.avg_bw, (int)fo_r.avg_bw, fo_r.avg_ans_rounds);

        std::cout << "n=" << std::setw(6) << n
                  << "  hit=" << std::setw(5) << std::setprecision(0) << std::fixed << fo_r.hot_hit_pct << "%"
                  << "  BL=" << std::setw(8) << (int)bl_r.avg_bw
                  << "  TM=" << std::setw(8) << (int)tm_r.avg_bw
                  << "  FO=" << std::setw(8) << (int)fo_r.avg_bw
                  << "\n";
    }
    std::cout << "\n";
}

void exp_ablation(const Config& cfg) {
    std::vector<int> Ns = {1<<14, 1<<16, 1<<18, 1<<20};
    int n = (cfg.n > 0) ? cfg.n : 1024;
    double s = (cfg.s > 0) ? cfg.s : 1.0;
    int Q = cfg.Q;

    std::cout << "=== Split-ORAM Ablation (n=" << n << ", s=" << s << ") ===\n\n";

    CsvWriter csv(cfg.outdir + "/ablation_split.csv");
    csv.header({"N", "nosplit_bw", "split_bw", "improvement"});

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
        auto ns_r = run_zipf(ns_tm, N, actual_n, s, Q, cfg.write_ratio);

        TieredOMapConfig sp_cfg;
        sp_cfg.total_keys = N; sp_cfg.hot_set_size = actual_n;
        sp_cfg.mode = SecurityMode::TierMembership;
        sp_cfg.use_split_oram = true;
        TieredOMap sp_tm(sp_cfg);
        sp_tm.init(data, hk);
        auto sp_r = run_zipf(sp_tm, N, actual_n, s, Q, cfg.write_ratio);

        double imp = ns_r.avg_bw / sp_r.avg_bw;
        csv.row(N, (int)ns_r.avg_bw, (int)sp_r.avg_bw, imp);

        std::cout << "N=" << std::setw(8) << N
                  << "  nosplit=" << std::setw(8) << (int)ns_r.avg_bw
                  << "  split=" << std::setw(8) << (int)sp_r.avg_bw
                  << "  imp=" << std::setprecision(2) << std::fixed << imp << "x"
                  << "\n";
    }
    std::cout << "\n";
}

void exp_write(const Config& cfg) {
    int N = (cfg.N > 0) ? cfg.N : (1 << 16);
    int n = (cfg.n > 0) ? cfg.n : 1024;
    double s = (cfg.s > 0) ? cfg.s : 1.0;
    int Q = cfg.Q;

    std::cout << "=== Read vs Write (N=" << N << ", n=" << n << ") ===\n\n";

    CsvWriter csv(cfg.outdir + "/read_vs_write.csv");
    csv.header({"config", "read_bw", "write_bw"});

    auto data = make_data(N);
    auto hk = make_hot_keys(n);

    auto measure_rw = [&](const std::string& label, auto make_instance) {
        auto inst = make_instance();
        double r_bw = 0, w_bw = 0;
        int r_cnt = 0, w_cnt = 0;
        ZipfSampler z(N, s, 123);
        for (int q = 0; q < Q; ++q) {
            int k = z.sample();
            auto res = inst->access(k);
            r_bw += res.total_bw.total_bytes();
            ++r_cnt;
        }
        auto inst2 = make_instance();
        ZipfSampler z2(N, s, 456);
        for (int q = 0; q < Q; ++q) {
            int k = z2.sample();
            Bytes wval = int_to_bytes(k + 1000000);
            auto res = inst2->access(k, &wval);
            w_bw += res.total_bw.total_bytes();
            ++w_cnt;
        }
        double avg_r = r_bw / r_cnt;
        double avg_w = w_bw / w_cnt;
        csv.row(label, (int)avg_r, (int)avg_w);
        std::cout << std::setw(20) << label
                  << "  read=" << std::setw(8) << (int)avg_r
                  << "  write=" << std::setw(8) << (int)avg_w
                  << "\n";
    };

    measure_rw("SingleOMAP", [&]() -> std::unique_ptr<TieredOMap> {
        TieredOMapConfig c;
        c.total_keys = N; c.hot_set_size = 0;
        c.mode = SecurityMode::FullOblivious;
        auto t = std::make_unique<TieredOMap>(c);
        std::vector<int> empty_hk;
        t->init(data, empty_hk);
        return t;
    });

    measure_rw("TM_split", [&]() -> std::unique_ptr<TieredOMap> {
        TieredOMapConfig c;
        c.total_keys = N; c.hot_set_size = n;
        c.mode = SecurityMode::TierMembership;
        c.use_split_oram = true;
        auto t = std::make_unique<TieredOMap>(c);
        t->init(data, hk);
        return t;
    });

    measure_rw("FO_split", [&]() -> std::unique_ptr<TieredOMap> {
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

void exp_modes(const Config& cfg) {
    int N = (cfg.N > 0) ? cfg.N : (1 << 16);
    int n = (cfg.n > 0) ? cfg.n : 1024;
    double s = (cfg.s > 0) ? cfg.s : 1.0;
    int Q = cfg.Q;

    std::cout << "=== Mode Comparison (N=" << N << ", n=" << n << ") ===\n\n";

    CsvWriter csv(cfg.outdir + "/modes.csv");
    csv.header({"mode", "avg_bw", "avg_rnd", "avg_ans_rnd", "hit_pct",
                "wan30_latency_ms", "wan30_ans_ms", "wan80_latency_ms", "wan80_ans_ms"});

    auto data = make_data(N);
    auto hk = make_hot_keys(n);

    AVLOmap bl(N);
    bl.init(data);
    auto bl_r = run_baseline_zipf(bl, N, s, Q, cfg.write_ratio);

    auto run_mode = [&](const std::string& label, SecurityMode m, bool split) {
        TieredOMapConfig c;
        c.total_keys = N; c.hot_set_size = n;
        c.mode = m; c.use_split_oram = split;
        TieredOMap tm(c);
        tm.init(data, hk);
        auto r = run_zipf(tm, N, n, s, Q, cfg.write_ratio);

        double wan30_lat = r.avg_rounds * 30;
        double wan30_ans = r.avg_ans_rounds * 30;
        double wan80_lat = r.avg_rounds * 80;
        double wan80_ans = r.avg_ans_rounds * 80;

        csv.row(label, (int)r.avg_bw, r.avg_rounds, r.avg_ans_rounds,
                r.hot_hit_pct, wan30_lat, wan30_ans, wan80_lat, wan80_ans);

        std::cout << std::setw(16) << label
                  << "  bw=" << std::setw(8) << (int)r.avg_bw
                  << "  rnd=" << std::setw(6) << std::setprecision(1) << std::fixed << r.avg_rounds
                  << "  ans=" << std::setw(6) << r.avg_ans_rounds
                  << "  W80_lat=" << std::setw(6) << (int)wan80_lat << "ms"
                  << "  W80_ans=" << std::setw(6) << (int)wan80_ans << "ms"
                  << "\n";
    };

    csv.row("SingleOMAP", (int)bl_r.avg_bw, bl_r.avg_rounds, bl_r.avg_rounds,
            0.0, bl_r.avg_rounds * 30, bl_r.avg_rounds * 30,
            bl_r.avg_rounds * 80, bl_r.avg_rounds * 80);
    std::cout << std::setw(16) << "SingleOMAP"
              << "  bw=" << std::setw(8) << (int)bl_r.avg_bw
              << "  rnd=" << std::setw(6) << std::setprecision(1) << std::fixed << bl_r.avg_rounds
              << "  ans=" << std::setw(6) << bl_r.avg_rounds
              << "  W80_lat=" << std::setw(6) << (int)(bl_r.avg_rounds * 80) << "ms"
              << "  W80_ans=" << std::setw(6) << (int)(bl_r.avg_rounds * 80) << "ms"
              << "\n";

    run_mode("TierMembership", SecurityMode::TierMembership, true);
    run_mode("FullOblivious", SecurityMode::FullOblivious, true);
    std::cout << "\n";
}

// ── Main ─────────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    Config cfg = parse_args(argc, argv);

    // Ensure output directory exists.
    std::string mkdir_cmd = "mkdir -p " + cfg.outdir;
    system(mkdir_cmd.c_str());

    std::cout << std::fixed;

    if (cfg.exp == "bandwidth" || cfg.exp == "all") exp_bandwidth(cfg);
    if (cfg.exp == "skewness" || cfg.exp == "all") exp_skewness(cfg);
    if (cfg.exp == "hotsize" || cfg.exp == "all") exp_hotsize(cfg);
    if (cfg.exp == "ablation" || cfg.exp == "all") exp_ablation(cfg);
    if (cfg.exp == "write" || cfg.exp == "all") exp_write(cfg);
    if (cfg.exp == "modes" || cfg.exp == "all") exp_modes(cfg);

    if (cfg.exp != "all" && cfg.exp != "bandwidth" && cfg.exp != "skewness" &&
        cfg.exp != "hotsize" && cfg.exp != "ablation" && cfg.exp != "write" &&
        cfg.exp != "modes") {
        std::cerr << "Unknown experiment: " << cfg.exp << "\n";
        std::cerr << "Available: all, bandwidth, skewness, hotsize, ablation, write, modes\n";
        return 1;
    }

    return 0;
}
