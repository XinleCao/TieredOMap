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
#include <vector>

using namespace tiered_omap;
using namespace tiered_omap::tee;
using Clock = std::chrono::steady_clock;

struct Config {
    int min_logN = 14;
    int max_logN = 24;
    int n = 1024;
    int value_size = 256;
    int Q = 200;
    double s = 1.0;
    std::string outdir = "tee_results_sgx";
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

static constexpr int PAGE_SIZE = 4096;

static void run_all(const Config& cfg) {
    ensure_dir(cfg.outdir);
    std::ofstream csv(cfg.outdir + "/tee_full.csv");
    csv << "logN,config,hot_us,cold_us,total_us,hit_pct,enclave_KB\n";

    std::cout << "=== TEE Full Benchmark ===\n";
    std::cout << std::setw(6) << "logN" << std::setw(14) << "config"
              << std::setw(10) << "hot_us" << std::setw(10) << "cold_us"
              << std::setw(10) << "total_us"
              << std::setw(8)  << "hit%"
              << std::setw(12) << "enclave_KB"
              << "\n";

    for (int logN = cfg.min_logN; logN <= cfg.max_logN; logN += 2) {
        int N = 1 << logN;
        int n = std::min(cfg.n, N / 2);
        std::cout << "\n--- logN=" << logN << " N=" << N << " n=" << n << " ---\n";

        auto data = make_data(N, cfg.value_size);
        auto hk = make_hot_keys(n);
        ZipfSampler zipf(N, cfg.s, 42);

        // ── Flat AVL OMAP (baseline) ──
        {
            TeeAvlOmap flat(N, cfg.value_size, 4, 0);
            flat.init(data);

            double sum_us = 0, sum_pages = 0;
            for (int q = 0; q < cfg.Q; ++q) {
                int key = zipf.sample();
                auto t0 = Clock::now();
                flat.search(key);
                auto t1 = Clock::now();
                sum_us += std::chrono::duration<double, std::micro>(t1 - t0).count();
                sum_pages += flat.last_stats().total_pages();
            }
            double avg_us = sum_us / cfg.Q;
            double avg_kb = sum_pages / cfg.Q * PAGE_SIZE / 1024.0;

            std::cout << std::setw(6) << logN << std::setw(14) << "flat_avl"
                      << std::setw(10) << std::fixed << std::setprecision(1) << avg_us
                      << std::setw(10) << avg_us
                      << std::setw(10) << avg_us
                      << std::setw(8) << "-"
                      << std::setw(12) << std::setprecision(0) << avg_kb << "\n";
            csv << logN << ",flat_avl,"
                << std::fixed << std::setprecision(1)
                << avg_us << "," << avg_us << "," << avg_us << ",,"
                << std::setprecision(0) << avg_kb << "\n";
        }

        // ── Packed FO ──
        {
            TeeOmapConfig tcfg;
            tcfg.total_keys = N;
            tcfg.hot_set_size = n;
            tcfg.value_size = cfg.value_size;
            tcfg.mode = TeeSecurityMode::FullOblivious;
            tcfg.use_split_oram = true;

            TeeOmap omap(tcfg);
            omap.init(data, hk);

            double sum_hot = 0, sum_cold = 0, sum_total = 0;
            double sum_pages = 0;
            int hot_cnt = 0, cold_cnt = 0;

            for (int q = 0; q < cfg.Q; ++q) {
                int key = zipf.sample();
                Clock::time_point t_answer;
                auto t0 = Clock::now();
                auto r = omap.access(key, nullptr,
                    [&](const Bytes&, bool) { t_answer = Clock::now(); });
                auto t1 = Clock::now();

                double answer_us = std::chrono::duration<double, std::micro>(
                    t_answer - t0).count();
                double total_us = std::chrono::duration<double, std::micro>(
                    t1 - t0).count();
                sum_total += total_us;
                sum_pages += r.total_pages;

                if (r.found_in_hot) {
                    sum_hot += answer_us;
                    ++hot_cnt;
                } else {
                    sum_cold += total_us;
                    ++cold_cnt;
                }
            }
            double avg_hot = hot_cnt > 0 ? sum_hot / hot_cnt : 0;
            double avg_cold = cold_cnt > 0 ? sum_cold / cold_cnt : 0;
            double avg_total = sum_total / cfg.Q;
            double hit = 100.0 * hot_cnt / cfg.Q;
            double avg_kb = sum_pages / cfg.Q * PAGE_SIZE / 1024.0;

            std::cout << std::setw(6) << logN << std::setw(14) << "packed_FO"
                      << std::setw(10) << std::fixed << std::setprecision(1) << avg_hot
                      << std::setw(10) << avg_cold
                      << std::setw(10) << avg_total
                      << std::setw(8) << std::setprecision(0) << hit
                      << std::setw(12) << avg_kb << "\n";
            csv << logN << ",packed_FO,"
                << std::fixed << std::setprecision(1)
                << avg_hot << "," << avg_cold << "," << avg_total << ","
                << hit << "," << std::setprecision(0) << avg_kb << "\n";
        }

        // ── Packed TM ──
        {
            TeeOmapConfig tcfg;
            tcfg.total_keys = N;
            tcfg.hot_set_size = n;
            tcfg.value_size = cfg.value_size;
            tcfg.mode = TeeSecurityMode::TierMembership;
            tcfg.use_split_oram = true;

            TeeOmap omap(tcfg);
            omap.init(data, hk);

            double sum_hot = 0, sum_cold = 0, sum_total = 0;
            double sum_pages = 0;
            int hot_cnt = 0, cold_cnt = 0;

            for (int q = 0; q < cfg.Q; ++q) {
                int key = zipf.sample();
                auto t0 = Clock::now();
                auto r = omap.access(key);
                auto t1 = Clock::now();

                double elapsed = std::chrono::duration<double, std::micro>(
                    t1 - t0).count();
                sum_total += elapsed;
                sum_pages += r.total_pages;

                if (r.found_in_hot) {
                    sum_hot += elapsed;
                    ++hot_cnt;
                } else {
                    sum_cold += elapsed;
                    ++cold_cnt;
                }
            }
            double avg_hot = hot_cnt > 0 ? sum_hot / hot_cnt : 0;
            double avg_cold = cold_cnt > 0 ? sum_cold / cold_cnt : 0;
            double avg_total = sum_total / cfg.Q;
            double hit = 100.0 * hot_cnt / cfg.Q;
            double avg_kb = sum_pages / cfg.Q * PAGE_SIZE / 1024.0;

            std::cout << std::setw(6) << logN << std::setw(14) << "packed_TM"
                      << std::setw(10) << std::fixed << std::setprecision(1) << avg_hot
                      << std::setw(10) << avg_cold
                      << std::setw(10) << avg_total
                      << std::setw(8) << std::setprecision(0) << hit
                      << std::setw(12) << avg_kb << "\n";
            csv << logN << ",packed_TM,"
                << std::fixed << std::setprecision(1)
                << avg_hot << "," << avg_cold << "," << avg_total << ","
                << hit << "," << std::setprecision(0) << avg_kb << "\n";
        }

        csv.flush();
        std::cout.flush();
    }
    csv.close();
    std::cout << "\n  -> " << cfg.outdir << "/tee_full.csv\n";
}

int main(int argc, char** argv) {
    Config cfg = parse_args(argc, argv);
    std::cout << "=== TEE Benchmark ===\n"
              << "  logN=" << cfg.min_logN << ".." << cfg.max_logN
              << "  n=" << cfg.n << "  val=" << cfg.value_size
              << "  Q=" << cfg.Q << "  s=" << cfg.s
              << "  outdir=" << cfg.outdir << "\n\n";
    run_all(cfg);
    std::cout << "=== Done ===\n";
    return 0;
}
