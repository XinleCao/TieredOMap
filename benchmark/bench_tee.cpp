#include "tiered_omap/tee/tee_avl_omap.h"
#include "tiered_omap/tee/tee_omap.h"
#include "tiered_omap/workload.h"
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

using namespace tiered_omap;
using namespace tiered_omap::tee;
using Clock = std::chrono::steady_clock;

// ── Helpers ────────────────────────────────────────────────────────────────

static std::vector<std::pair<int, Bytes>> make_data(int n, int val_size) {
    std::vector<std::pair<int, Bytes>> data;
    data.reserve(n);
    for (int i = 0; i < n; ++i)
        data.push_back({i, int_to_bytes(i)});
    // Pad values to val_size.
    for (auto& [k, v] : data)
        v = pad_bytes(v, val_size);
    return data;
}

static std::vector<int> make_hot_keys(int n) {
    std::vector<int> hk(n);
    for (int i = 0; i < n; ++i) hk[i] = i;
    return hk;
}

static void print_header() {
    std::printf("%-12s %-6s %-6s %-6s %-10s %-10s %-10s %-10s %-10s %-10s\n",
        "experiment", "logN", "n", "valSz",
        "hot_pg", "cold_up", "cold_low", "total_pg",
        "avg_us", "mode");
}

// ── Experiment 1: Flat OMAP vs TieredOMAP page faults ───────────────────

static void exp_page_faults(int logN, int val_size, int Q) {
    int N = 1 << logN;
    int n = std::max(N / 16, 4);  // hot = N/16
    auto data = make_data(N, val_size);
    auto hk = make_hot_keys(n);

    ZipfSampler zipf(N, 0.99);

    // ── Flat OMAP (single AVL over one ORAM, no split) ──────────────────
    {
        TeeAvlOmap flat(N, val_size, 4, 0);
        flat.init(data);

        uint64_t total_pages = 0;
        auto t0 = Clock::now();
        for (int q = 0; q < Q; ++q) {
            int key = zipf.sample();
            flat.search(key);
            total_pages += flat.last_stats().total_pages();
        }
        auto t1 = Clock::now();
        double avg_us = std::chrono::duration<double, std::micro>(t1 - t0).count() / Q;
        std::printf("%-12s %-6d %-6s %-6d %-10.1f %-10s %-10.1f %-10.1f %-10.1f %-10s\n",
            "flat_omap", logN, "-", val_size,
            0.0, "-", double(total_pages) / Q, double(total_pages) / Q,
            avg_us, "N/A");
    }

    // ── TieredOMAP (FullOblivious) ──────────────────────────────────────
    {
        TeeOmapConfig cfg;
        cfg.total_keys = N;
        cfg.hot_set_size = n;
        cfg.value_size = val_size;
        cfg.mode = TeeSecurityMode::FullOblivious;
        cfg.use_split_oram = true;

        TeeOmap omap(cfg);
        omap.init(data, hk);

        uint64_t h_pages = 0, cu_pages = 0, cl_pages = 0, tot_pages = 0;
        auto t0 = Clock::now();
        for (int q = 0; q < Q; ++q) {
            int key = zipf.sample();
            auto r = omap.access(key);
            h_pages += r.hot_pages;
            cu_pages += r.cold_up_pages;
            cl_pages += r.cold_low_pages;
            tot_pages += r.total_pages;
        }
        auto t1 = Clock::now();
        double avg_us = std::chrono::duration<double, std::micro>(t1 - t0).count() / Q;
        std::printf("%-12s %-6d %-6d %-6d %-10.1f %-10.1f %-10.1f %-10.1f %-10.1f %-10s\n",
            "tiered_FO", logN, n, val_size,
            double(h_pages) / Q, double(cu_pages) / Q,
            double(cl_pages) / Q, double(tot_pages) / Q,
            avg_us, "FullObliv");
    }

    // ── TieredOMAP (TierMembership) ─────────────────────────────────────
    {
        TeeOmapConfig cfg;
        cfg.total_keys = N;
        cfg.hot_set_size = n;
        cfg.value_size = val_size;
        cfg.mode = TeeSecurityMode::TierMembership;
        cfg.use_split_oram = true;

        TeeOmap omap(cfg);
        omap.init(data, hk);

        uint64_t h_pages = 0, cu_pages = 0, cl_pages = 0, tot_pages = 0;
        int hot_hits = 0;
        auto t0 = Clock::now();
        for (int q = 0; q < Q; ++q) {
            int key = zipf.sample();
            auto r = omap.access(key);
            h_pages += r.hot_pages;
            cu_pages += r.cold_up_pages;
            cl_pages += r.cold_low_pages;
            tot_pages += r.total_pages;
            if (r.found_in_hot) ++hot_hits;
        }
        auto t1 = Clock::now();
        double avg_us = std::chrono::duration<double, std::micro>(t1 - t0).count() / Q;
        std::printf("%-12s %-6d %-6d %-6d %-10.1f %-10.1f %-10.1f %-10.1f %-10.1f TierMem(%d%%)\n",
            "tiered_TM", logN, n, val_size,
            double(h_pages) / Q, double(cu_pages) / Q,
            double(cl_pages) / Q, double(tot_pages) / Q,
            avg_us, hot_hits * 100 / Q);
    }
}

// ── Experiment 2: Varying hot-set ratio ─────────────────────────────────

static void exp_hot_ratio(int logN, int val_size, int Q) {
    int N = 1 << logN;
    auto data = make_data(N, val_size);

    ZipfSampler zipf(N, 0.99);

    int ratios[] = {4, 8, 16, 32, 64};  // hot = N/ratio
    for (int ratio : ratios) {
        int n = std::max(N / ratio, 4);
        auto hk = make_hot_keys(n);

        TeeOmapConfig cfg;
        cfg.total_keys = N;
        cfg.hot_set_size = n;
        cfg.value_size = val_size;
        cfg.mode = TeeSecurityMode::TierMembership;
        cfg.use_split_oram = true;

        TeeOmap omap(cfg);
        omap.init(data, hk);

        uint64_t tot_pages = 0;
        int hot_hits = 0;
        auto t0 = Clock::now();
        for (int q = 0; q < Q; ++q) {
            int key = zipf.sample();
            auto r = omap.access(key);
            tot_pages += r.total_pages;
            if (r.found_in_hot) ++hot_hits;
        }
        auto t1 = Clock::now();
        double avg_us = std::chrono::duration<double, std::micro>(t1 - t0).count() / Q;
        std::printf("%-12s %-6d %-6d %-6d %-10s %-10s %-10s %-10.1f %-10.1f hit=%d%%\n",
            "ratio", logN, n, val_size,
            "-", "-", "-", double(tot_pages) / Q,
            avg_us, hot_hits * 100 / Q);
    }
}

// ── Experiment 3: Split-ORAM on vs off ──────────────────────────────────

static void exp_split_effect(int logN, int val_size, int Q) {
    int N = 1 << logN;
    int n = std::max(N / 16, 4);
    auto data = make_data(N, val_size);
    auto hk = make_hot_keys(n);

    ZipfSampler zipf(N, 0.99);

    for (bool split : {false, true}) {
        TeeOmapConfig cfg;
        cfg.total_keys = N;
        cfg.hot_set_size = n;
        cfg.value_size = val_size;
        cfg.mode = TeeSecurityMode::FullOblivious;
        cfg.use_split_oram = split;

        TeeOmap omap(cfg);
        omap.init(data, hk);

        uint64_t cu_pages = 0, cl_pages = 0, tot_pages = 0;
        auto t0 = Clock::now();
        for (int q = 0; q < Q; ++q) {
            int key = zipf.sample();
            auto r = omap.access(key);
            cu_pages += r.cold_up_pages;
            cl_pages += r.cold_low_pages;
            tot_pages += r.total_pages;
        }
        auto t1 = Clock::now();
        double avg_us = std::chrono::duration<double, std::micro>(t1 - t0).count() / Q;
        std::printf("%-12s %-6d %-6d %-6d %-10s %-10.1f %-10.1f %-10.1f %-10.1f %s\n",
            split ? "split_on" : "split_off",
            logN, n, val_size,
            "-", double(cu_pages) / Q, double(cl_pages) / Q,
            double(tot_pages) / Q, avg_us,
            split ? "split=ON" : "split=OFF");
    }
}

// ── Main ────────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    int logN = 12;
    int val_size = 64;
    int Q = 200;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--logN" && i + 1 < argc) logN = std::stoi(argv[++i]);
        else if (arg == "--val" && i + 1 < argc) val_size = std::stoi(argv[++i]);
        else if (arg == "--Q" && i + 1 < argc) Q = std::stoi(argv[++i]);
    }

    std::printf("=== TEE Benchmark: logN=%d  N=%d  val_size=%d  Q=%d ===\n\n",
        logN, 1 << logN, val_size, Q);

    std::printf("── Exp 1: Flat vs Tiered page faults ──\n");
    print_header();
    exp_page_faults(logN, val_size, Q);

    std::printf("\n── Exp 2: Varying hot-set ratio (TierMembership) ──\n");
    print_header();
    exp_hot_ratio(logN, val_size, Q);

    std::printf("\n── Exp 3: Split-ORAM effect (FullOblivious) ──\n");
    print_header();
    exp_split_effect(logN, val_size, Q);

    return 0;
}
