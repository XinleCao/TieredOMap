#include "tiered_omap/tiered_omap.h"
#include "tiered_omap/omap/avl_omap.h"
#include <cmath>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <random>
#include <vector>

using namespace tiered_omap;

class ZipfSampler {
public:
    ZipfSampler(int n, double alpha, uint64_t seed = 42) : rng_(seed) {
        cdf_.resize(n);
        double h = 0;
        for (int i = 1; i <= n; ++i) h += 1.0 / std::pow(i, alpha);
        double cum = 0;
        for (int i = 0; i < n; ++i) {
            cum += (1.0 / std::pow(i + 1, alpha)) / h;
            cdf_[i] = cum;
        }
    }
    int sample() {
        double u = std::uniform_real_distribution<>(0, 1)(rng_);
        return static_cast<int>(
            std::lower_bound(cdf_.begin(), cdf_.end(), u) - cdf_.begin());
    }
private:
    std::mt19937_64 rng_;
    std::vector<double> cdf_;
};

double hot_hit_ratio(int N, int n, double alpha) {
    double h = 0;
    for (int i = 1; i <= N; ++i) h += 1.0 / std::pow(i, alpha);
    double top = 0;
    for (int i = 1; i <= n; ++i) top += 1.0 / std::pow(i, alpha);
    return top / h;
}

int main() {
    std::cout << std::fixed;

    // ─── Fixed n, growing N ─────────────────────────────────────────────────
    // This is the realistic scenario: hot set is small and fixed,
    // database grows large.

    int n_fixed = 256;
    std::vector<double> alphas = {0.8, 1.0, 1.2, 1.5};

    struct NConfig { int N; int Q; };
    std::vector<NConfig> ns = {
        {1024,    200},
        {4096,    150},
        {16384,   100},
        {65536,   60},
    };

    std::cout << "╔════════════════════════════════════════════════════════════════════════════╗\n";
    std::cout << "║  Fixed n = " << n_fixed
              << ",  growing N.  Realistic: hot set fixed, database scales.        ║\n";
    std::cout << "╚════════════════════════════════════════════════════════════════════════════╝\n\n";

    // Part A: single-query cost
    std::cout << "── Part A: Single-query cost ──\n\n";
    std::cout << std::left
              << std::setw(8) << "N"
              << std::setw(6) << "n"
              << std::setw(7) << "logN"
              << std::setw(7) << "logn"
              << " │ "
              << std::setw(8) << "BL rnd"
              << " │ "
              << std::setw(10) << "FO ans_at"
              << std::setw(10) << "FO rnd"
              << " │ "
              << std::setw(10) << "TM h_rnd"
              << std::setw(10) << "TM h_bw%"
              << " │ "
              << std::setw(10) << "hot ans↓"
              << std::endl;
    std::cout << std::string(96, '-') << std::endl;

    for (auto& [N, Q] : ns) {
        int n = std::min(n_fixed, N / 2);

        std::vector<std::pair<int, Bytes>> data;
        for (int i = 0; i < N; ++i) data.emplace_back(i, int_to_bytes(i));
        std::vector<int> hk(n);
        std::iota(hk.begin(), hk.end(), 0);

        AVLOmap bl(N);
        bl.init(data);
        bl.search(0);
        int bl_rnd = bl.last_stats().rounds;
        int bl_bw  = bl.last_stats().total_bytes();

        TieredOMapConfig fo_cfg;
        fo_cfg.total_keys = N; fo_cfg.hot_set_size = n;
        fo_cfg.mode = SecurityMode::FullOblivious;
        fo_cfg.use_split_oram = true;
        TieredOMap fo(fo_cfg);
        fo.init(data, hk);
        auto fh = fo.access(0);

        TieredOMapConfig tm_cfg;
        tm_cfg.total_keys = N; tm_cfg.hot_set_size = n;
        tm_cfg.mode = SecurityMode::TierMembership;
        tm_cfg.use_split_oram = true;
        TieredOMap tm(tm_cfg);
        tm.init(data, hk);
        auto th = tm.access(0);

        double bw_save = (1.0 - (double)th.total_bw.total_bytes() / bl_bw) * 100;
        double ans_save = (1.0 - (double)fh.rounds_to_answer / bl_rnd) * 100;

        std::cout << std::setw(8) << N
                  << std::setw(6) << n
                  << std::setw(7) << std::setprecision(0) << std::log2(N)
                  << std::setw(7) << std::setprecision(0) << std::log2(n)
                  << " │ "
                  << std::setw(8) << bl_rnd
                  << " │ "
                  << std::setw(10) << fh.rounds_to_answer
                  << std::setw(10) << (int)fh.total_bw.rounds
                  << " │ "
                  << std::setw(10) << (int)th.total_bw.rounds
                  << std::setw(10) << std::setprecision(0) << bw_save << "%"
                  << " │ "
                  << std::setw(10) << std::setprecision(0) << ans_save << "%"
                  << std::endl;
    }

    // Part B: weighted avg under Zipf
    std::cout << "\n── Part B: Weighted average under Zipf ──\n\n";
    std::cout << std::left
              << std::setw(8)  << "N"
              << std::setw(6)  << "n"
              << std::setw(5)  << "a"
              << std::setw(7)  << "hot%"
              << " │ "
              << std::setw(9)  << "BL rnd"
              << std::setw(9)  << "BL bw"
              << " │ "
              << std::setw(10) << "FO ans"
              << std::setw(9)  << "FO bw"
              << " │ "
              << std::setw(9)  << "TM rnd"
              << std::setw(9)  << "TM bw"
              << " │ "
              << std::setw(9)  << "ans↓"
              << std::setw(9)  << "TM rnd↓"
              << std::setw(9)  << "TM bw↓"
              << std::endl;
    std::cout << std::string(112, '-') << std::endl;

    for (auto& [N, Q] : ns) {
        int n = std::min(n_fixed, N / 2);

        std::vector<std::pair<int, Bytes>> data;
        for (int i = 0; i < N; ++i) data.emplace_back(i, int_to_bytes(i));
        std::vector<int> hk(n);
        std::iota(hk.begin(), hk.end(), 0);

        AVLOmap bl_omap(N);
        bl_omap.init(data);

        for (double alpha : alphas) {
            double hr = hot_hit_ratio(N, n, alpha);

            ZipfSampler z1(N, alpha, 123);
            double bl_r = 0, bl_b = 0;
            for (int q = 0; q < Q; ++q) {
                bl_omap.search(z1.sample());
                bl_r += bl_omap.last_stats().rounds;
                bl_b += bl_omap.last_stats().total_bytes();
            }

            TieredOMapConfig fo_cfg;
            fo_cfg.total_keys = N; fo_cfg.hot_set_size = n;
            fo_cfg.mode = SecurityMode::FullOblivious;
            fo_cfg.use_split_oram = true;
            TieredOMap fo(fo_cfg);
            fo.init(data, hk);

            ZipfSampler z2(N, alpha, 123);
            double fo_a = 0, fo_b = 0;
            for (int q = 0; q < Q; ++q) {
                auto r = fo.access(z2.sample());
                fo_a += r.rounds_to_answer;
                fo_b += r.total_bw.total_bytes();
            }

            TieredOMapConfig tm_cfg;
            tm_cfg.total_keys = N; tm_cfg.hot_set_size = n;
            tm_cfg.mode = SecurityMode::TierMembership;
            tm_cfg.use_split_oram = true;
            TieredOMap tm(tm_cfg);
            tm.init(data, hk);

            ZipfSampler z3(N, alpha, 123);
            double tm_r = 0, tm_b = 0;
            for (int q = 0; q < Q; ++q) {
                auto r = tm.access(z3.sample());
                tm_r += r.total_bw.rounds;
                tm_b += r.total_bw.total_bytes();
            }

            double blr = bl_r/Q, blb = bl_b/Q;
            double foa = fo_a/Q, fob = fo_b/Q;
            double tmr = tm_r/Q, tmb = tm_b/Q;

            auto sv = [](double ours, double base) {
                char buf[16];
                snprintf(buf, sizeof(buf), "-%.0f%%", (1.0 - ours/base)*100);
                return std::string(buf);
            };

            std::cout << std::setw(8) << N
                      << std::setw(6) << n
                      << std::setw(5) << std::setprecision(1) << alpha
                      << std::setw(7) << std::setprecision(0) << (hr*100)
                      << " │ "
                      << std::setw(9) << std::setprecision(1) << blr
                      << std::setw(9) << std::setprecision(0) << blb
                      << " │ "
                      << std::setw(10) << std::setprecision(1) << foa
                      << std::setw(9) << std::setprecision(0) << fob
                      << " │ "
                      << std::setw(9) << std::setprecision(1) << tmr
                      << std::setw(9) << std::setprecision(0) << tmb
                      << " │ "
                      << std::setw(9) << sv(foa, blr)
                      << std::setw(9) << sv(tmr, blr)
                      << std::setw(9) << sv(tmb, blb)
                      << std::endl;
        }
        std::cout << std::string(112, '-') << std::endl;
    }

    // Part C: theoretical projection for very large N
    std::cout << "\n── Part C: Theoretical projection (n=" << n_fixed
              << " fixed, large N) ──\n\n";
    std::cout << "  N          logN  logn  hot%(a=1.2)  FO ans↓  TM rnd↓  TM bw↓(approx)\n";
    std::cout << std::string(72, '-') << std::endl;

    std::vector<int> big_Ns = {1<<14, 1<<16, 1<<18, 1<<20, 1<<22, 1<<24};
    for (int N : big_Ns) {
        int n = n_fixed;
        int logN = (int)std::ceil(1.44 * std::log2(std::max(N, 2)));
        int logn = (int)std::ceil(1.44 * std::log2(std::max(n, 2)));
        double hr = hot_hit_ratio(N, n, 1.2);

        double fo_ans_avg = hr * logn + (1-hr) * logN;
        double tm_rnd_avg = hr * logn + (1-hr) * logN;

        double fo_ans_save = (1.0 - fo_ans_avg / logN) * 100;
        double tm_rnd_save = (1.0 - tm_rnd_avg / logN) * 100;
        double tm_bw_save = tm_rnd_save;

        std::cout << "  " << std::setw(10) << N
                  << std::setw(6) << logN
                  << std::setw(6) << logn
                  << std::setw(12) << std::setprecision(0) << (hr*100) << "%"
                  << std::setw(9) << std::setprecision(0) << fo_ans_save << "%"
                  << std::setw(9) << std::setprecision(0) << tm_rnd_save << "%"
                  << std::setw(9) << std::setprecision(0) << tm_bw_save << "%"
                  << std::endl;
    }

    return 0;
}
