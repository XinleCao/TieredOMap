#include "tiered_omap/tee/tee_avl_omap.h"
#include "tiered_omap/tee/tee_omap.h"
#include "tiered_omap/workload.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <vector>

using namespace tiered_omap;
using namespace tiered_omap::tee;
using Clock = std::chrono::steady_clock;

struct Config {
    int min_logN = 14;
    int max_logN = 22;
    int n = 1024;
    int value_size = 256;
    int Q = 1000;
    double s = 1.0;
    std::vector<int> betas{10, 32, 100, 300, 1000};
    std::string outdir = "tee_batch_results";
};

struct BatchMetrics {
    double hot_release_us = 0.0;
    double cold_release_us = 0.0;
    double complete_us = 0.0;
    uint64_t pages = 0;
    int hot = 0;
    int cold = 0;
};

static std::vector<int> parse_betas(const std::string& s) {
    std::vector<int> out;
    std::stringstream ss(s);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        if (!tok.empty()) out.push_back(std::stoi(tok));
    }
    return out.empty() ? std::vector<int>{100} : out;
}

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
        else if (k == "--betas") c.betas = parse_betas(v);
        else if (k == "--outdir") c.outdir = v;
    }
    return c;
}

static void ensure_dir(const std::string& dir) {
    ::mkdir(dir.c_str(), 0755);
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

static std::vector<std::vector<int>> make_batches(
    int N, double skew, int beta, int logical_queries) {
    int batches = std::max(1, (logical_queries + beta - 1) / beta);
    ZipfSampler zipf(N, skew, 42);

    std::vector<std::vector<int>> out;
    out.reserve(batches);
    for (int b = 0; b < batches; ++b) {
        std::vector<int> batch;
        batch.reserve(beta);
        for (int i = 0; i < beta; ++i)
            batch.push_back(zipf.sample());
        out.push_back(std::move(batch));
    }
    return out;
}

static BatchMetrics run_flat_batch(TeeAvlOmap& flat,
                                   const std::vector<int>& batch) {
    BatchMetrics m;
    auto t0 = Clock::now();
    for (int key : batch) {
        auto q0 = Clock::now();
        flat.search(key);
        auto q1 = Clock::now();
        m.complete_us += std::chrono::duration<double, std::micro>(
            q1 - q0).count();
        m.pages += flat.last_stats().total_pages();
    }
    auto t1 = Clock::now();
    m.complete_us = std::chrono::duration<double, std::micro>(
        t1 - t0).count();
    m.cold_release_us = m.complete_us;
    m.hot_release_us = 0.0;
    m.cold = static_cast<int>(batch.size());
    return m;
}

static BatchMetrics run_fo_batch(TeeOmap& omap,
                                 const std::vector<int>& batch) {
    BatchMetrics m;
    auto t0 = Clock::now();
    for (int key : batch) {
        auto r = omap.access(key);
        if (r.found_in_hot) ++m.hot;
        else ++m.cold;
        m.pages += r.total_pages;
    }
    auto t1 = Clock::now();
    m.complete_us = std::chrono::duration<double, std::micro>(
        t1 - t0).count();
    m.cold_release_us = m.complete_us;
    m.hot_release_us = 0.0;
    return m;
}

static BatchMetrics run_batch_tm(TeeOmap& omap,
                                 const std::vector<int>& batch) {
    BatchMetrics m;
    double hot_phase_us = 0.0;
    double cold_extra_us = 0.0;

    for (int key : batch) {
        Clock::time_point early;
        auto t0 = Clock::now();
        auto r = omap.access(key, nullptr, [&](const Bytes&, bool) {
            early = Clock::now();
        });
        auto t1 = Clock::now();

        double early_us = std::chrono::duration<double, std::micro>(
            early - t0).count();
        double total_us = std::chrono::duration<double, std::micro>(
            t1 - t0).count();

        hot_phase_us += early_us;
        if (r.found_in_hot) ++m.hot;
        else {
            ++m.cold;
            cold_extra_us += std::max(0.0, total_us - early_us);
        }
        m.pages += r.total_pages;
    }

    m.hot_release_us = hot_phase_us;
    m.cold_release_us = hot_phase_us + cold_extra_us;
    m.complete_us = m.cold_release_us;
    return m;
}

static void emit_rows(std::ofstream& csv, int logN, int N, int n, int beta,
                      const std::string& mode,
                      const std::vector<BatchMetrics>& rows) {
    double hot_rel = 0.0, cold_rel = 0.0, complete = 0.0;
    double pages = 0.0, hot = 0.0, cold = 0.0;
    for (const auto& r : rows) {
        hot_rel += r.hot_release_us;
        cold_rel += r.cold_release_us;
        complete += r.complete_us;
        pages += static_cast<double>(r.pages);
        hot += r.hot;
        cold += r.cold;
    }

    double batches = static_cast<double>(rows.size());
    double logical = hot + cold;
    double hit_pct = logical > 0 ? 100.0 * hot / logical : 0.0;
    double avg_batch_hot = hot / batches;
    double avg_batch_cold = cold / batches;
    double avg_query_us = logical > 0 ? complete / logical : 0.0;
    double avg_kb = logical > 0
        ? pages * EnclaveOram::PAGE_SIZE / 1024.0 / logical
        : 0.0;

    csv << logN << "," << N << "," << n << "," << beta << ","
        << rows.size() << "," << static_cast<long long>(logical) << ","
        << mode << ","
        << std::fixed << std::setprecision(2)
        << hot_rel / batches << ","
        << cold_rel / batches << ","
        << complete / batches << ","
        << avg_query_us << ","
        << std::setprecision(1) << hit_pct << ","
        << std::setprecision(2) << avg_batch_hot << ","
        << avg_batch_cold << ","
        << std::setprecision(1) << avg_kb << "\n";
}

static void run_all(const Config& cfg) {
    ensure_dir(cfg.outdir);
    std::ofstream csv(cfg.outdir + "/tee_batch.csv");
    csv << "logN,N,n,beta,batches,logical_queries,mode,"
        << "avg_hot_release_us,avg_cold_release_us,avg_batch_complete_us,"
        << "avg_query_us,hit_pct,avg_batch_hot,avg_batch_cold,avg_enclave_KB\n";

    std::cout << "=== TEE Batch Benchmark ===\n";
    std::cout << "  Q=" << cfg.Q << "  s=" << cfg.s << "  n=" << cfg.n
              << "  val=" << cfg.value_size << "\n";

    for (int logN = cfg.min_logN; logN <= cfg.max_logN; logN += 2) {
        int N = 1 << logN;
        int n = std::min(cfg.n, N / 2);
        auto data = make_data(N, cfg.value_size);
        auto hk = make_hot_keys(n);

        for (int beta : cfg.betas) {
            auto batches = make_batches(N, cfg.s, beta, cfg.Q);

            std::cout << "\n--- logN=" << logN << " beta=" << beta
                      << " batches=" << batches.size() << " ---\n";

            {
                TeeAvlOmap flat(N, cfg.value_size, 4, 0,
                                EnclaveOramLayout::Veb);
                flat.init(data);
                std::vector<BatchMetrics> rows;
                rows.reserve(batches.size());
                for (const auto& batch : batches)
                    rows.push_back(run_flat_batch(flat, batch));
                emit_rows(csv, logN, N, n, beta, "flat_enig", rows);
            }

            {
                TeeOmapConfig tcfg;
                tcfg.total_keys = N;
                tcfg.hot_set_size = n;
                tcfg.value_size = cfg.value_size;
                tcfg.mode = TeeSecurityMode::FullOblivious;
                tcfg.use_split_oram = true;
                tcfg.oram_layout = EnclaveOramLayout::Veb;

                TeeOmap omap(tcfg);
                omap.init(data, hk);
                std::vector<BatchMetrics> rows;
                rows.reserve(batches.size());
                for (const auto& batch : batches)
                    rows.push_back(run_fo_batch(omap, batch));
                emit_rows(csv, logN, N, n, beta, "tiered_FO", rows);
            }

            {
                TeeOmapConfig tcfg;
                tcfg.total_keys = N;
                tcfg.hot_set_size = n;
                tcfg.value_size = cfg.value_size;
                tcfg.mode = TeeSecurityMode::TierMembership;
                tcfg.use_split_oram = true;
                tcfg.oram_layout = EnclaveOramLayout::Veb;

                TeeOmap omap(tcfg);
                omap.init(data, hk);
                std::vector<BatchMetrics> rows;
                rows.reserve(batches.size());
                for (const auto& batch : batches)
                    rows.push_back(run_batch_tm(omap, batch));
                emit_rows(csv, logN, N, n, beta, "tiered_BatchTM", rows);
            }

            csv.flush();
        }
    }

    std::cout << "\n  -> " << cfg.outdir << "/tee_batch.csv\n";
}

int main(int argc, char** argv) {
    Config cfg = parse_args(argc, argv);
    run_all(cfg);
    return 0;
}
