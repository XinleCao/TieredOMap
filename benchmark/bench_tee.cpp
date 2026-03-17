#include "tiered_omap/tee/tee_avl_omap.h"
#include "tiered_omap/tee/tee_omap.h"
#include "tiered_omap/workload.h"
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <sys/stat.h>
#include <unordered_set>
#include <vector>

using namespace tiered_omap;
using namespace tiered_omap::tee;
using Clock = std::chrono::steady_clock;

struct Config {
    int min_logN = 14;
    int max_logN = 22;
    int n = 1024;
    int value_size = 256;
    int Q = 200;
    double s = 0.99;
    std::string outdir = "tee_results";
    bool append = false;
};

static Config parse_args(int argc, char** argv) {
    Config c;
    for (int i = 1; i + 1 < argc; i += 2) {
        std::string k = argv[i], v = argv[i + 1];
        if (k == "--min_logN") c.min_logN = std::stoi(v);
        else if (k == "--max_logN") c.max_logN = std::stoi(v);
        else if (k == "--n") c.n = std::stoi(v);
        else if (k == "--val") c.value_size = std::stoi(v);
        else if (k == "--Q") c.Q = std::stoi(v);
        else if (k == "--s") c.s = std::stod(v);
        else if (k == "--outdir") c.outdir = v;
        else if (k == "--append") { c.append = true; --i; }
    }
    return c;
}

static std::vector<std::pair<int, Bytes>> make_data(int N, int val_size) {
    std::vector<std::pair<int, Bytes>> data;
    data.reserve(N);
    for (int i = 0; i < N; ++i)
        data.push_back({i, pad_bytes(int_to_bytes(i), val_size)});
    return data;
}

static std::vector<int> make_hot_keys(int n) {
    std::vector<int> hk(n);
    for (int i = 0; i < n; ++i) hk[i] = i;
    return hk;
}

static void ensure_dir(const std::string& dir) {
    ::mkdir(dir.c_str(), 0755);
}

// ═══════════════════════════════════════════════════════════════════════════
// Main experiment: Scalability with N.
//
// For each logN, test four configs:
//   1. flat_avl        – baseline oblivious AVL OMAP
//   2. tiered_packed   – TieredOMAP (packed hot, FO mode)
//   3. tiered_packed_TM – TieredOMAP (packed hot, TM mode)
//
// Metrics per query:
//   answer_us  – wall-clock time until the answer is available
//                (flat: end of search; FO: early_cb after hot tier; TM: end)
//   total_us   – wall-clock time for entire operation to finish
//   (In FO mode answer_us < total_us because cold tier runs after early response.)
// ═══════════════════════════════════════════════════════════════════════════

static void exp_scalability(const Config& cfg) {
    ensure_dir(cfg.outdir);
    auto mode = cfg.append ? (std::ios::app) : (std::ios::out);
    std::ofstream csv(cfg.outdir + "/tee_scalability.csv", mode);
    if (!cfg.append)
        csv << "logN,config,answer_us,total_us,hit_pct\n";

    std::cout << "=== Scalability with N ===\n";
    std::cout << std::setw(6) << "logN" << std::setw(18) << "config"
              << std::setw(12) << "ans_us" << std::setw(12) << "tot_us"
              << std::setw(8) << "hit%"
              << "\n";

    for (int logN = cfg.min_logN; logN <= cfg.max_logN; logN += 2) {
        int N = 1 << logN;
        int n = std::min(cfg.n, N / 2);
        auto data = make_data(N, cfg.value_size);
        auto hk = make_hot_keys(n);
        ZipfSampler zipf(N, cfg.s, 42);

        // ── Flat AVL OMAP (baseline) ─────────────────────────────────────
        // No tiering: answer = total.
        {
            TeeAvlOmap flat(N, cfg.value_size, 4, 0);
            flat.init(data);

            double sum_us = 0;
            for (int q = 0; q < cfg.Q; ++q) {
                int key = zipf.sample();
                auto t0 = Clock::now();
                flat.search(key);
                auto t1 = Clock::now();
                sum_us += std::chrono::duration<double, std::micro>(t1 - t0).count();
            }
            double avg = sum_us / cfg.Q;

            std::cout << std::setw(6) << logN << std::setw(18) << "flat_avl"
                      << std::setw(12) << std::fixed << std::setprecision(1) << avg
                      << std::setw(12) << avg
                      << std::setw(8) << "-" << "\n";
            csv << logN << ",flat_avl,"
                << std::fixed << std::setprecision(1) << avg << "," << avg << ",\n";
        }

        // ── TieredOMAP packed-hot, FO mode ───────────────────────────────
        // answer = time to early_cb; total = full access() return.
        {
            TeeOmapConfig tcfg;
            tcfg.total_keys = N;
            tcfg.hot_set_size = n;
            tcfg.value_size = cfg.value_size;
            tcfg.mode = TeeSecurityMode::FullOblivious;
            tcfg.use_split_oram = true;

            TeeOmap omap(tcfg);
            omap.init(data, hk);

            double sum_answer = 0, sum_total = 0;
            int hot_hits = 0;
            for (int q = 0; q < cfg.Q; ++q) {
                int key = zipf.sample();
                Clock::time_point t_answer;
                auto t0 = Clock::now();
                auto r = omap.access(key, nullptr,
                    [&](const Bytes&, bool) { t_answer = Clock::now(); });
                auto t1 = Clock::now();
                sum_answer += std::chrono::duration<double, std::micro>(t_answer - t0).count();
                sum_total += std::chrono::duration<double, std::micro>(t1 - t0).count();
                if (r.found_in_hot) ++hot_hits;
            }
            double avg_ans = sum_answer / cfg.Q;
            double avg_tot = sum_total / cfg.Q;
            double hit = 100.0 * hot_hits / cfg.Q;

            std::cout << std::setw(6) << logN << std::setw(18) << "packed_FO"
                      << std::setw(12) << std::fixed << std::setprecision(1) << avg_ans
                      << std::setw(12) << avg_tot
                      << std::setw(8) << std::setprecision(0) << hit << "\n";
            csv << logN << ",packed_FO,"
                << std::fixed << std::setprecision(1) << avg_ans << ","
                << avg_tot << "," << hit << "\n";
        }

        // ── TieredOMAP packed-hot, TM mode ───────────────────────────────
        // In TM mode, each query accesses only ONE tier: hot OR cold.
        // answer = total for every query (no early/late split).
        // Hot queries finish fast (hot tier only); cold queries are slow.
        {
            TeeOmapConfig tcfg;
            tcfg.total_keys = N;
            tcfg.hot_set_size = n;
            tcfg.value_size = cfg.value_size;
            tcfg.mode = TeeSecurityMode::TierMembership;
            tcfg.use_split_oram = true;

            TeeOmap omap(tcfg);
            omap.init(data, hk);

            double sum_hot_us = 0, sum_cold_us = 0, sum_total = 0;
            int hot_hits = 0;
            for (int q = 0; q < cfg.Q; ++q) {
                int key = zipf.sample();
                auto t0 = Clock::now();
                auto r = omap.access(key);
                auto t1 = Clock::now();
                double elapsed = std::chrono::duration<double, std::micro>(t1 - t0).count();
                sum_total += elapsed;
                if (r.found_in_hot) { sum_hot_us += elapsed; ++hot_hits; }
                else                { sum_cold_us += elapsed; }
            }
            double avg_tot = sum_total / cfg.Q;
            double avg_hot = hot_hits > 0 ? sum_hot_us / hot_hits : 0;
            double hit = 100.0 * hot_hits / cfg.Q;

            std::cout << std::setw(6) << logN << std::setw(18) << "packed_TM"
                      << std::setw(12) << std::fixed << std::setprecision(1) << avg_hot
                      << std::setw(12) << avg_tot
                      << std::setw(8) << std::setprecision(0) << hit << "\n";
            csv << logN << ",packed_TM,"
                << std::fixed << std::setprecision(1) << avg_hot << ","
                << avg_tot << "," << hit << "\n";
        }

        std::cout << "  ---\n";
        std::cout.flush();
    }
    csv.close();
    std::cout << "  -> " << cfg.outdir << "/tee_scalability.csv\n\n";
}

// ═══════════════════════════════════════════════════════════════════════════
// Hot-only queries: every query hits the hot tier.
// Shows the best-case answer latency for TieredOMAP.
// ═══════════════════════════════════════════════════════════════════════════

static void exp_hot_only(const Config& cfg) {
    ensure_dir(cfg.outdir);
    auto mode = cfg.append ? (std::ios::app) : (std::ios::out);
    std::ofstream csv(cfg.outdir + "/tee_hot_only.csv", mode);
    if (!cfg.append)
        csv << "logN,config,answer_us,total_us\n";

    std::cout << "=== Hot-query-only latency ===\n";
    std::cout << std::setw(6) << "logN" << std::setw(18) << "config"
              << std::setw(12) << "ans_us" << std::setw(12) << "tot_us" << "\n";

    for (int logN = cfg.min_logN; logN <= cfg.max_logN; logN += 2) {
        int N = 1 << logN;
        int n = std::min(cfg.n, N / 2);
        auto data = make_data(N, cfg.value_size);
        auto hk = make_hot_keys(n);
        ZipfSampler zipf_hot(n, cfg.s, 42);

        // Flat AVL
        {
            TeeAvlOmap flat(N, cfg.value_size, 4, 0);
            flat.init(data);

            double sum_us = 0;
            for (int q = 0; q < cfg.Q; ++q) {
                int key = zipf_hot.sample();
                auto t0 = Clock::now();
                flat.search(key);
                auto t1 = Clock::now();
                sum_us += std::chrono::duration<double, std::micro>(t1 - t0).count();
            }
            double avg = sum_us / cfg.Q;
            std::cout << std::setw(6) << logN << std::setw(18) << "flat_avl"
                      << std::setw(12) << std::fixed << std::setprecision(1) << avg
                      << std::setw(12) << avg << "\n";
            csv << logN << ",flat_avl,"
                << std::fixed << std::setprecision(1) << avg << "," << avg << "\n";
        }

        // Packed FO: all hot queries → answer_us = hot tier time
        {
            TeeOmapConfig tcfg;
            tcfg.total_keys = N;
            tcfg.hot_set_size = n;
            tcfg.value_size = cfg.value_size;
            tcfg.mode = TeeSecurityMode::FullOblivious;
            tcfg.use_split_oram = true;

            TeeOmap omap(tcfg);
            omap.init(data, hk);

            double sum_answer = 0, sum_total = 0;
            for (int q = 0; q < cfg.Q; ++q) {
                int key = zipf_hot.sample();
                Clock::time_point t_answer;
                auto t0 = Clock::now();
                omap.access(key, nullptr,
                    [&](const Bytes&, bool) { t_answer = Clock::now(); });
                auto t1 = Clock::now();
                sum_answer += std::chrono::duration<double, std::micro>(t_answer - t0).count();
                sum_total += std::chrono::duration<double, std::micro>(t1 - t0).count();
            }
            double avg_ans = sum_answer / cfg.Q;
            double avg_tot = sum_total / cfg.Q;
            std::cout << std::setw(6) << logN << std::setw(18) << "packed_FO"
                      << std::setw(12) << std::fixed << std::setprecision(1) << avg_ans
                      << std::setw(12) << avg_tot << "\n";
            csv << logN << ",packed_FO,"
                << std::fixed << std::setprecision(1) << avg_ans << "," << avg_tot << "\n";
        }

        // Packed TM: hot query → only hot tier, answer = total
        {
            TeeOmapConfig tcfg;
            tcfg.total_keys = N;
            tcfg.hot_set_size = n;
            tcfg.value_size = cfg.value_size;
            tcfg.mode = TeeSecurityMode::TierMembership;
            tcfg.use_split_oram = true;

            TeeOmap omap(tcfg);
            omap.init(data, hk);

            double sum_us = 0;
            for (int q = 0; q < cfg.Q; ++q) {
                int key = zipf_hot.sample();
                auto t0 = Clock::now();
                omap.access(key);
                auto t1 = Clock::now();
                sum_us += std::chrono::duration<double, std::micro>(t1 - t0).count();
            }
            double avg = sum_us / cfg.Q;
            std::cout << std::setw(6) << logN << std::setw(18) << "packed_TM"
                      << std::setw(12) << std::fixed << std::setprecision(1) << avg
                      << std::setw(12) << avg << "\n";
            csv << logN << ",packed_TM,"
                << std::fixed << std::setprecision(1) << avg << "," << avg << "\n";
        }

        std::cout << "  ---\n";
        std::cout.flush();
    }
    csv.close();
    std::cout << "  -> " << cfg.outdir << "/tee_hot_only.csv\n\n";
}

// ═══════════════════════════════════════════════════════════════════════════

int main(int argc, char** argv) {
    Config cfg = parse_args(argc, argv);

    std::cout << "=== TEE Benchmark (real SGX timing) ===\n"
              << "  logN=" << cfg.min_logN << ".." << cfg.max_logN
              << "  n=" << cfg.n
              << "  val=" << cfg.value_size << "  Q=" << cfg.Q
              << "  s=" << cfg.s << "  outdir=" << cfg.outdir << "\n\n";

    exp_scalability(cfg);
    exp_hot_only(cfg);

    std::cout << "=== All TEE experiments done ===\n";
    return 0;
}
