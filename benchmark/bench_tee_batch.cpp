#include "tiered_omap/tee/tee_avl_omap.h"
#include "tiered_omap/tee/tee_omap.h"
#include "tiered_omap/workload.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
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
    int value_size = 32;
    int Q = 1000;
    double s = 1.0;
    std::vector<int> betas{10, 32, 100, 300, 1000};
    std::string env = "both";     // large | constrained | small | both | hardware
    int trusted_kb = 8192;        // constrained-memory budget
    double page_us = 8.0;         // synthetic per-page penalty
    bool include_flat = true;
    bool reuse_init = true;
    std::string modes = "flat,fo,batchtm";
    std::string outdir = "tee_batch_results";
};

struct BatchMetrics {
    double hot_release_us = 0.0;
    double cold_extra_us = 0.0;
    double cold_release_us = 0.0;
    double complete_us = 0.0;
    uint64_t hot_phase_pages = 0;
    uint64_t cold_extra_pages = 0;
    uint64_t pages = 0;
    int hot = 0;
    int cold = 0;
};

struct MemoryProfile {
    std::string name;
    int trusted_kb = 0;
    double page_us = 0.0;
    bool model_paging = false;
};

struct WorkingSet {
    uint64_t hot_bytes = 0;
    uint64_t cold_bytes = 0;
    uint64_t total_bytes = 0;
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

static bool parse_bool(const std::string& v) {
    return v == "1" || v == "true" || v == "yes" || v == "on";
}

static bool mode_enabled(const std::string& modes, const std::string& mode) {
    std::stringstream ss(modes);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        if (tok == "all" || tok == mode) return true;
    }
    return false;
}

static Config parse_args(int argc, char** argv) {
    Config c;
    for (int i = 1; i < argc; i += 2) {
        std::string k = argv[i];
        if (i + 1 >= argc)
            throw std::runtime_error("missing value for " + k);
        std::string v = argv[i + 1];
        if (k == "--min_logN") c.min_logN = std::stoi(v);
        else if (k == "--max_logN") c.max_logN = std::stoi(v);
        else if (k == "--n") c.n = std::stoi(v);
        else if (k == "--val") c.value_size = std::stoi(v);
        else if (k == "--Q") c.Q = std::stoi(v);
        else if (k == "--s") c.s = std::stod(v);
        else if (k == "--betas") c.betas = parse_betas(v);
        else if (k == "--env") c.env = v;
        else if (k == "--trusted_kb") c.trusted_kb = std::stoi(v);
        else if (k == "--page_us") c.page_us = std::stod(v);
        else if (k == "--include_flat") c.include_flat = parse_bool(v);
        else if (k == "--reuse_init") c.reuse_init = parse_bool(v);
        else if (k == "--modes") c.modes = v;
        else if (k == "--outdir") c.outdir = v;
        else throw std::runtime_error("unknown option " + k);
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

static std::vector<MemoryProfile> make_profiles(const Config& cfg) {
    if (cfg.env == "large") {
        return {{"large", 0, 0.0, false}};
    }
    if (cfg.env == "constrained" || cfg.env == "small") {
        return {{"constrained", cfg.trusted_kb, cfg.page_us, true}};
    }
    if (cfg.env == "hardware") {
        return {{"hardware", cfg.trusted_kb, 0.0, false}};
    }
    if (cfg.env == "both") {
        return {
            {"large", 0, 0.0, false},
            {"constrained", cfg.trusted_kb, cfg.page_us, true},
        };
    }
    throw std::runtime_error("unknown --env " + cfg.env);
}

static uint64_t estimate_oram_tree_bytes(int capacity, int value_size,
                                         int bucket_size) {
    int level = std::max(1, ceil_log2(std::max(capacity, 1)));
    uint64_t nodes = (uint64_t{1} << level) - 1;
    uint64_t block_bytes = 2 * sizeof(int) + static_cast<uint64_t>(value_size);
    return nodes * static_cast<uint64_t>(bucket_size) * block_bytes;
}

static uint64_t estimate_hot_working_bytes(int n, int value_size,
                                           int bucket_size) {
    uint64_t dir_bytes = static_cast<uint64_t>(std::max(n, 1)) * 20;
    return dir_bytes + estimate_oram_tree_bytes(n, value_size, bucket_size);
}

static uint64_t estimate_cold_avl_working_bytes(int N, int n, int value_size,
                                                int bucket_size,
                                                bool split_oram) {
    int node_value_size = AVL_HEADER_SIZE + value_size;
    uint64_t total = estimate_oram_tree_bytes(N, node_value_size, bucket_size);
    if (split_oram) {
        int split_depth = std::max(1, ceil_log2(std::max(n, 2)));
        int upper_cap = (1 << split_depth) - 1;
        total += estimate_oram_tree_bytes(upper_cap, node_value_size,
                                          bucket_size);
    }
    return total;
}

static WorkingSet flat_working_set(int N, int value_size, int bucket_size) {
    WorkingSet ws;
    ws.cold_bytes = estimate_cold_avl_working_bytes(
        N, 0, value_size, bucket_size, false);
    ws.total_bytes = ws.cold_bytes;
    return ws;
}

static WorkingSet tiered_working_set(int N, int n, int value_size,
                                     int bucket_size, bool split_oram) {
    WorkingSet ws;
    ws.hot_bytes = estimate_hot_working_bytes(n, value_size, bucket_size);
    ws.cold_bytes = estimate_cold_avl_working_bytes(
        N, n, value_size, bucket_size, split_oram);
    ws.total_bytes = ws.hot_bytes + ws.cold_bytes;
    return ws;
}

static double apply_memory_model(double measured_us,
                                 const MemoryProfile& profile,
                                 uint64_t working_bytes,
                                 uint64_t pages_touched) {
    if (!profile.model_paging || profile.trusted_kb <= 0
        || profile.page_us <= 0.0 || working_bytes == 0) {
        return measured_us;
    }

    double trusted_bytes = static_cast<double>(profile.trusted_kb) * 1024.0;
    if (static_cast<double>(working_bytes) <= trusted_bytes)
        return measured_us;

    double spill_ratio =
        (static_cast<double>(working_bytes) - trusted_bytes)
        / static_cast<double>(working_bytes);
    return measured_us
         + spill_ratio * static_cast<double>(pages_touched) * profile.page_us;
}

template <class Fn>
static double timed_us(Fn&& fn) {
    auto t0 = Clock::now();
    fn();
    auto t1 = Clock::now();
    return std::chrono::duration<double, std::micro>(t1 - t0).count();
}

static TeeOmapConfig make_tee_config(int N, int n, int value_size,
                                     TeeSecurityMode mode) {
    TeeOmapConfig tcfg;
    tcfg.total_keys = N;
    tcfg.hot_set_size = n;
    tcfg.value_size = value_size;
    tcfg.mode = mode;
    tcfg.use_split_oram = true;
    tcfg.oram_layout = EnclaveOramLayout::Veb;
    return tcfg;
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
        uint64_t pages = flat.last_stats().total_pages();
        m.cold_extra_pages += pages;
        m.pages += pages;
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
        m.hot_phase_pages += r.hot_pages;
        m.cold_extra_pages += r.cold_up_pages + r.cold_low_pages;
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
    Clock::time_point hot_release;

    auto t0 = Clock::now();
    auto br = omap.access_batch_tier_membership(
        batch,
        [&](const std::vector<TeeAccessResult>&) {
            hot_release = Clock::now();
        });
    auto t1 = Clock::now();

    m.hot_release_us = std::chrono::duration<double, std::micro>(
        hot_release - t0).count();
    m.cold_extra_us = std::chrono::duration<double, std::micro>(
        t1 - hot_release).count();
    m.cold_release_us = std::chrono::duration<double, std::micro>(
        t1 - t0).count();
    m.complete_us = m.cold_release_us;
    m.hot_phase_pages = br.hot_phase_pages;
    m.cold_extra_pages = br.cold_extra_pages;
    m.pages = br.total_pages;
    m.hot = br.hot_count;
    m.cold = br.cold_count;
    return m;
}

static void emit_rows(std::ofstream& csv, int logN, int N, int n, int beta,
                      const std::string& mode, bool split_oram,
                      const std::string& init_policy, double init_us,
                      const WorkingSet& ws,
                      const std::vector<MemoryProfile>& profiles,
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

    bool is_batch_tm = mode == "tiered_BatchTM";

    for (const auto& profile : profiles) {
        double modeled_hot_rel = 0.0;
        double modeled_cold_rel = 0.0;
        double modeled_complete = 0.0;
        for (const auto& r : rows) {
            if (is_batch_tm) {
                double hot_model = apply_memory_model(
                    r.hot_release_us, profile, ws.hot_bytes,
                    r.hot_phase_pages);
                double cold_extra_model = apply_memory_model(
                    r.cold_extra_us, profile, ws.cold_bytes,
                    r.cold_extra_pages);
                modeled_hot_rel += hot_model;
                modeled_cold_rel += hot_model + cold_extra_model;
                modeled_complete += hot_model + cold_extra_model;
            } else {
                double total_model = apply_memory_model(
                    r.complete_us, profile, ws.total_bytes, r.pages);
                modeled_hot_rel += 0.0;
                modeled_cold_rel += total_model;
                modeled_complete += total_model;
            }
        }

        double modeled_avg_query_us = logical > 0
            ? modeled_complete / logical
            : 0.0;

        csv << logN << "," << N << "," << n << "," << beta << ","
            << rows.size() << "," << static_cast<long long>(logical) << ","
            << profile.name << ",1," << (split_oram ? 1 : 0) << ","
            << mode << "," << init_policy << ","
            << std::fixed << std::setprecision(2)
            << init_us << ","
            << hot_rel / batches << ","
            << cold_rel / batches << ","
            << complete / batches << ","
            << avg_query_us << ","
            << modeled_hot_rel / batches << ","
            << modeled_cold_rel / batches << ","
            << modeled_complete / batches << ","
            << modeled_avg_query_us << ","
            << std::setprecision(1) << hit_pct << ","
            << std::setprecision(2) << avg_batch_hot << ","
            << avg_batch_cold << ","
            << std::setprecision(1) << avg_kb << ","
            << profile.trusted_kb << ","
            << std::setprecision(2) << profile.page_us << ","
            << std::setprecision(1)
            << static_cast<double>(ws.hot_bytes) / 1024.0 << ","
            << static_cast<double>(ws.cold_bytes) / 1024.0 << ","
            << static_cast<double>(ws.total_bytes) / 1024.0 << "\n";
    }
}

static void run_all(const Config& cfg) {
    ensure_dir(cfg.outdir);
    auto profiles = make_profiles(cfg);

    std::ofstream csv(cfg.outdir + "/tee_batch.csv");
    csv << "logN,N,n,beta,batches,logical_queries,trusted_mem,"
        << "server_only,split_oram,mode,"
        << "init_policy,init_us,"
        << "measured_hot_release_us,measured_cold_release_us,"
        << "measured_batch_complete_us,measured_avg_query_us,"
        << "modeled_hot_release_us,modeled_cold_release_us,"
        << "modeled_batch_complete_us,modeled_avg_query_us,"
        << "hit_pct,avg_batch_hot,avg_batch_cold,avg_enclave_KB,"
        << "trusted_kb,page_us,hot_working_kb,cold_working_kb,"
        << "total_working_kb\n";

    std::cout << "=== TEE Batch Benchmark ===\n";
    std::cout << "  Q=" << cfg.Q << "  s=" << cfg.s << "  n=" << cfg.n
              << "  val=" << cfg.value_size << "  env=" << cfg.env
              << "  trusted_kb=" << cfg.trusted_kb
              << "  page_us=" << cfg.page_us
              << "  reuse_init=" << (cfg.reuse_init ? "true" : "false")
              << "  modes=" << cfg.modes
              << "\n";

    bool run_flat = cfg.include_flat && mode_enabled(cfg.modes, "flat");
    bool run_fo = mode_enabled(cfg.modes, "fo");
    bool run_batchtm = mode_enabled(cfg.modes, "batchtm");

    for (int logN = cfg.min_logN; logN <= cfg.max_logN; logN += 2) {
        int N = 1 << logN;
        int n = std::min(cfg.n, N / 2);
        auto data = make_data(N, cfg.value_size);
        auto hk = make_hot_keys(n);
        WorkingSet flat_ws = flat_working_set(N, cfg.value_size, 4);
        WorkingSet tiered_ws = tiered_working_set(
            N, n, cfg.value_size, 4, true);

        std::vector<std::pair<int, std::vector<std::vector<int>>>> workloads;
        workloads.reserve(cfg.betas.size());
        for (int beta : cfg.betas)
            workloads.push_back({beta, make_batches(N, cfg.s, beta, cfg.Q)});

        const std::string init_policy =
            cfg.reuse_init ? "per_logN_mode" : "per_beta_mode";

        if (cfg.reuse_init) {
            if (run_flat) {
                std::unique_ptr<TeeAvlOmap> flat;
                double init_us = timed_us([&]() {
                    flat = std::make_unique<TeeAvlOmap>(
                        N, cfg.value_size, 4, 0, EnclaveOramLayout::Veb);
                    flat->init(data);
                });

                for (const auto& [beta, batches] : workloads) {
                    std::cout << "\n--- logN=" << logN
                              << " mode=flat_enig beta=" << beta
                              << " batches=" << batches.size() << " ---\n";
                    std::vector<BatchMetrics> rows;
                    rows.reserve(batches.size());
                    for (const auto& batch : batches)
                        rows.push_back(run_flat_batch(*flat, batch));
                    emit_rows(csv, logN, N, n, beta, "flat_enig", false,
                              init_policy, init_us, flat_ws, profiles, rows);
                    csv.flush();
                }
            }

            if (run_fo) {
                std::unique_ptr<TeeOmap> omap;
                double init_us = timed_us([&]() {
                    omap = std::make_unique<TeeOmap>(make_tee_config(
                        N, n, cfg.value_size, TeeSecurityMode::FullOblivious));
                    omap->init(data, hk);
                });

                for (const auto& [beta, batches] : workloads) {
                    std::cout << "\n--- logN=" << logN
                              << " mode=tiered_FO beta=" << beta
                              << " batches=" << batches.size() << " ---\n";
                    std::vector<BatchMetrics> rows;
                    rows.reserve(batches.size());
                    for (const auto& batch : batches)
                        rows.push_back(run_fo_batch(*omap, batch));
                    emit_rows(csv, logN, N, n, beta, "tiered_FO", true,
                              init_policy, init_us, tiered_ws, profiles, rows);
                    csv.flush();
                }
            }

            if (run_batchtm) {
                std::unique_ptr<TeeOmap> omap;
                double init_us = timed_us([&]() {
                    omap = std::make_unique<TeeOmap>(make_tee_config(
                        N, n, cfg.value_size, TeeSecurityMode::TierMembership));
                    omap->init(data, hk);
                });

                for (const auto& [beta, batches] : workloads) {
                    std::cout << "\n--- logN=" << logN
                              << " mode=tiered_BatchTM beta=" << beta
                              << " batches=" << batches.size() << " ---\n";
                    std::vector<BatchMetrics> rows;
                    rows.reserve(batches.size());
                    for (const auto& batch : batches)
                        rows.push_back(run_batch_tm(*omap, batch));
                    emit_rows(csv, logN, N, n, beta, "tiered_BatchTM", true,
                              init_policy, init_us, tiered_ws, profiles, rows);
                    csv.flush();
                }
            }
            continue;
        }

        for (const auto& [beta, batches] : workloads) {
            std::cout << "\n--- logN=" << logN << " beta=" << beta
                      << " batches=" << batches.size() << " ---\n";

            if (run_flat) {
                std::unique_ptr<TeeAvlOmap> flat;
                double init_us = timed_us([&]() {
                    flat = std::make_unique<TeeAvlOmap>(
                        N, cfg.value_size, 4, 0, EnclaveOramLayout::Veb);
                    flat->init(data);
                });
                std::vector<BatchMetrics> rows;
                rows.reserve(batches.size());
                for (const auto& batch : batches)
                    rows.push_back(run_flat_batch(*flat, batch));
                emit_rows(csv, logN, N, n, beta, "flat_enig", false,
                          init_policy, init_us, flat_ws, profiles, rows);
            }

            if (run_fo) {
                std::unique_ptr<TeeOmap> omap;
                double init_us = timed_us([&]() {
                    omap = std::make_unique<TeeOmap>(make_tee_config(
                        N, n, cfg.value_size, TeeSecurityMode::FullOblivious));
                    omap->init(data, hk);
                });
                std::vector<BatchMetrics> rows;
                rows.reserve(batches.size());
                for (const auto& batch : batches)
                    rows.push_back(run_fo_batch(*omap, batch));
                emit_rows(csv, logN, N, n, beta, "tiered_FO", true,
                          init_policy, init_us, tiered_ws, profiles, rows);
            }

            if (run_batchtm) {
                std::unique_ptr<TeeOmap> omap;
                double init_us = timed_us([&]() {
                    omap = std::make_unique<TeeOmap>(make_tee_config(
                        N, n, cfg.value_size, TeeSecurityMode::TierMembership));
                    omap->init(data, hk);
                });
                std::vector<BatchMetrics> rows;
                rows.reserve(batches.size());
                for (const auto& batch : batches)
                    rows.push_back(run_batch_tm(*omap, batch));
                emit_rows(csv, logN, N, n, beta, "tiered_BatchTM", true,
                          init_policy, init_us, tiered_ws, profiles, rows);
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
