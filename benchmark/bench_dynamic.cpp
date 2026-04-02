#include "tiered_omap/tiered_omap.h"
#include "tiered_omap/omap/avl_omap.h"
#include "tiered_omap/workload.h"
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <string>
#include <vector>

using namespace tiered_omap;

struct DynConfig {
    std::string exp = "all";
    int N = 256;
    int n = 32;
    double s = 1.0;
    int total_ops = 2000;
    int window = 50;
    int observation_window = 64;
    int swap_interval = 64;
    int cache_size = 8;
    int shift_offset = 0;
    int shift_at = 0;
    std::string outdir = "results";
};

DynConfig parse_args(int argc, char** argv) {
    DynConfig c;
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
        else if (key == "--total_ops") c.total_ops = std::stoi(val);
        else if (key == "--window") c.window = std::stoi(val);
        else if (key == "--B_obs") c.observation_window = std::stoi(val);
        else if (key == "--B_swap") c.swap_interval = std::stoi(val);
        else if (key == "--B") c.cache_size = std::stoi(val);
        else if (key == "--shift") c.shift_offset = std::stoi(val);
        else if (key == "--shift_at") c.shift_at = std::stoi(val);
        else if (key == "--outdir") c.outdir = val;
    }
    return c;
}

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

// ── Hit rate convergence ─────────────────────────────────────────────────────

void exp_hitrate(const DynConfig& cfg) {
    std::cout << "=== Hit Rate Convergence (N=" << cfg.N << " n=" << cfg.n
              << " s=" << cfg.s << " B_obs=" << cfg.observation_window
              << " B_swap=" << cfg.swap_interval
              << " B=" << cfg.cache_size << ") ===\n\n";

    auto data = make_data(cfg.N);
    auto hk = make_hot_keys(cfg.n);

    TieredOMapConfig tc;
    tc.total_keys = cfg.N;
    tc.hot_set_size = cfg.n;
    tc.mode = SecurityMode::FullOblivious;
    tc.maintenance.enabled = true;
    tc.maintenance.observation_window = cfg.observation_window;
    tc.maintenance.swap_interval = cfg.swap_interval;
    tc.maintenance.cache_size = cfg.cache_size;

    TieredOMap tmap(tc);
    tmap.init(data, hk);

    ZipfSampler z(cfg.N, cfg.s, 42);

    std::ofstream csv(cfg.outdir + "/hitrate_convergence.csv");
    csv << "op,hot_hits,queries,hit_rate,hot_size\n";

    int hot_hits = 0, queries = 0;
    for (int i = 0; i < cfg.total_ops; ++i) {
        int k = z.sample();
        auto r = tmap.access(k);
        if (r.found_in_hot) ++hot_hits;
        ++queries;

        if ((i + 1) % cfg.window == 0) {
            double rate = 100.0 * hot_hits / queries;
            csv << (i + 1) << "," << hot_hits << "," << queries << ","
                << rate << "," << tmap.hot_set_size() << "\n";
            std::cout << "op=" << std::setw(6) << (i + 1)
                      << "  hit_rate=" << std::setprecision(1) << std::fixed
                      << std::setw(5) << rate << "%"
                      << "  hot_size=" << tmap.hot_set_size() << "\n";
            hot_hits = queries = 0;
        }
    }
    std::cout << "\n";
}

// ── Workload drift recovery ──────────────────────────────────────────────────

void exp_drift(const DynConfig& cfg) {
    int shift = (cfg.shift_offset > 0) ? cfg.shift_offset : cfg.N / 4;
    int shift_at = (cfg.shift_at > 0) ? cfg.shift_at : cfg.total_ops / 2;

    std::cout << "=== Drift Recovery (shift=" << shift << " at op " << shift_at
              << ") ===\n\n";

    auto data = make_data(cfg.N);
    auto hk = make_hot_keys(cfg.n);

    TieredOMapConfig tc;
    tc.total_keys = cfg.N;
    tc.hot_set_size = cfg.n;
    tc.mode = SecurityMode::FullOblivious;
    tc.maintenance.enabled = true;
    tc.maintenance.observation_window = cfg.observation_window;
    tc.maintenance.swap_interval = cfg.swap_interval;
    tc.maintenance.cache_size = cfg.cache_size;

    TieredOMap tmap(tc);
    tmap.init(data, hk);

    ShiftingZipfSampler sz(cfg.N, cfg.s, shift, 42);

    std::ofstream csv(cfg.outdir + "/drift_recovery.csv");
    csv << "op,hot_hits,queries,hit_rate,hot_size,phase\n";

    int hot_hits = 0, queries = 0;
    for (int i = 0; i < cfg.total_ops; ++i) {
        int k = (i < shift_at) ? sz.sample_phase1() : sz.sample_phase2();
        auto r = tmap.access(k);
        if (r.found_in_hot) ++hot_hits;
        ++queries;

        if ((i + 1) % cfg.window == 0) {
            double rate = 100.0 * hot_hits / queries;
            int phase = (i < shift_at) ? 1 : 2;
            csv << (i + 1) << "," << hot_hits << "," << queries << ","
                << rate << "," << tmap.hot_set_size() << "," << phase << "\n";

            std::string marker = ((i + 1) == shift_at + cfg.window) ? " <-- SHIFT" : "";
            std::cout << "op=" << std::setw(6) << (i + 1)
                      << "  hit_rate=" << std::setprecision(1) << std::fixed
                      << std::setw(5) << rate << "%"
                      << "  hot_size=" << tmap.hot_set_size()
                      << "  phase=" << phase << marker << "\n";
            hot_hits = queries = 0;
        }
    }
    std::cout << "\n";
}

// ── Parameter sensitivity ────────────────────────────────────────────────────

void exp_sensitivity(const DynConfig& cfg) {
    std::cout << "=== Parameter Sensitivity ===\n\n";

    auto data = make_data(cfg.N);
    auto hk = make_hot_keys(cfg.n);

    std::vector<int> obs_windows = {32, 64, 128, 256, 512};
    std::vector<int> cache_sizes = {4, 8, 16, 32};

    std::ofstream csv(cfg.outdir + "/sensitivity.csv");
    csv << "B_obs,cache_size,steady_hit_rate,final_hot_size\n";

    std::cout << std::left
              << std::setw(8) << "B_obs"
              << std::setw(8) << "B"
              << std::setw(12) << "hit_rate%"
              << std::setw(10) << "hot_size"
              << "\n" << std::string(38, '-') << "\n";

    for (int Bobs : obs_windows) {
        for (int B : cache_sizes) {
            TieredOMapConfig tc;
            tc.total_keys = cfg.N;
            tc.hot_set_size = cfg.n;
            tc.mode = SecurityMode::FullOblivious;
            tc.maintenance.enabled = true;
            tc.maintenance.observation_window = Bobs;
            tc.maintenance.swap_interval = std::min(Bobs, 32);
            tc.maintenance.cache_size = B;

            TieredOMap tmap(tc);
            tmap.init(data, hk);

            ZipfSampler z(cfg.N, cfg.s, 42);

            for (int i = 0; i < cfg.total_ops / 2; ++i)
                tmap.access(z.sample());

            int hits = 0, total = cfg.total_ops / 2;
            for (int i = 0; i < total; ++i) {
                auto r = tmap.access(z.sample());
                if (r.found_in_hot) ++hits;
            }
            double rate = 100.0 * hits / total;

            csv << Bobs << "," << B << "," << rate << ","
                << tmap.hot_set_size() << "\n";

            std::cout << std::setw(8) << Bobs
                      << std::setw(8) << B
                      << std::setw(12) << std::setprecision(1) << std::fixed << rate
                      << std::setw(10) << tmap.hot_set_size()
                      << "\n";
        }
    }
    std::cout << "\n";
}

// ── Main ─────────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    DynConfig cfg = parse_args(argc, argv);
    std::string mkdir_cmd = "mkdir -p " + cfg.outdir;
    system(mkdir_cmd.c_str());
    std::cout << std::fixed;

    if (cfg.exp == "hitrate" || cfg.exp == "all") exp_hitrate(cfg);
    if (cfg.exp == "drift" || cfg.exp == "all") exp_drift(cfg);
    if (cfg.exp == "sensitivity" || cfg.exp == "all") exp_sensitivity(cfg);

    if (cfg.exp != "all" && cfg.exp != "hitrate" && cfg.exp != "drift" &&
        cfg.exp != "sensitivity") {
        std::cerr << "Unknown experiment: " << cfg.exp << "\n";
        std::cerr << "Available: all, hitrate, drift, sensitivity\n";
        return 1;
    }
    return 0;
}
