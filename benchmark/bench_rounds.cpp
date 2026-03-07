#include "tiered_omap/tiered_omap.h"
#include "tiered_omap/omap/avl_omap.h"
#include "tiered_omap/workload.h"
#include <cmath>
#include <iostream>
#include <iomanip>
#include <numeric>
#include <algorithm>
#include <vector>

using namespace tiered_omap;

int main() {
    const int n = 1024;
    const int Q = 500;
    const int W = 200;

    auto make_data = [](int N) {
        std::vector<std::pair<int,Bytes>> d;
        for (int i = 0; i < N; ++i) d.emplace_back(i, int_to_bytes(i));
        return d;
    };
    auto make_hk = [](int nn) {
        std::vector<int> h(nn);
        std::iota(h.begin(), h.end(), 0);
        return h;
    };

    // ── Exp 1: Round reduction vs N ──
    std::cout << "=== Round Reduction vs N (n=" << n << " s=1.0) ===\n\n";
    std::cout << std::left
              << std::setw(8) << "log2N"
              << std::setw(10) << "BL_rnd"
              << std::setw(10) << "hot_rnd"
              << std::setw(10) << "exp_rnd"
              << std::setw(10) << "hot_cut"
              << std::setw(10) << "exp_cut"
              << std::setw(10) << "hit%"
              << "\n" << std::string(68, '-') << "\n";

    for (int N : {1<<12, 1<<14, 1<<16, 1<<18, 1<<20}) {
        int an = std::min(n, N/2);
        auto data = make_data(N);
        auto hk = make_hk(an);

        AVLOmap bl(N); bl.init(data);
        ZipfSampler zb(N, 1.0, 42);
        double bl_rnd = 0;
        for (int q = 0; q < W; ++q) bl.search(zb.sample());
        for (int q = 0; q < Q; ++q) { bl.search(zb.sample()); bl_rnd += bl.last_stats().rounds; }
        bl_rnd /= Q;

        TieredOMapConfig tc;
        tc.total_keys = N; tc.hot_set_size = an;
        tc.mode = SecurityMode::TierMembership; tc.use_split_oram = true;
        TieredOMap tm(tc); tm.init(data, hk);
        ZipfSampler zt(N, 1.0, 42);
        double ans_sum = 0, hot_sum = 0;
        int hot_cnt = 0;
        for (int q = 0; q < W; ++q) tm.access(zt.sample());
        for (int q = 0; q < Q; ++q) {
            auto r = tm.access(zt.sample());
            ans_sum += r.rounds_to_answer;
            if (r.found_in_hot) { hot_sum += r.rounds_to_answer; ++hot_cnt; }
        }
        double exp_rnd = ans_sum / Q;
        double hot_rnd = hot_cnt > 0 ? hot_sum / hot_cnt : exp_rnd;

        std::cout << std::fixed
                  << std::setw(8)  << (int)std::log2(N)
                  << std::setw(10) << std::setprecision(1) << bl_rnd
                  << std::setw(10) << hot_rnd
                  << std::setw(10) << exp_rnd
                  << std::setw(10) << std::setprecision(0) << (1 - hot_rnd/bl_rnd)*100 << "%"
                  << std::setw(10) << (1 - exp_rnd/bl_rnd)*100 << "%"
                  << std::setw(10) << hot_cnt*100/Q << "%"
                  << "\n";
    }

    // ── Exp 2: Skewness sweep with latency data ──
    std::cout << "\n=== Skewness vs WAN-80 Latency (N=2^16 n=1024) ===\n\n";
    std::cout << std::left
              << std::setw(6)  << "s"
              << std::setw(8)  << "hit%"
              << std::setw(12) << "BL_W80(ms)"
              << std::setw(12) << "hot_W80(ms)"
              << std::setw(12) << "exp_W80(ms)"
              << std::setw(10) << "hot_save"
              << "\n" << std::string(60, '-') << "\n";
    {
        int N = 1 << 16;
        auto data = make_data(N);
        auto hk = make_hk(n);
        AVLOmap bl(N); bl.init(data);

        for (double s : {0.5, 0.8, 1.0, 1.2, 1.5}) {
            ZipfSampler zb(N, s, 42);
            double bl_rnd = 0;
            for (int q = 0; q < W; ++q) bl.search(zb.sample());
            for (int q = 0; q < Q; ++q) { bl.search(zb.sample()); bl_rnd += bl.last_stats().rounds; }
            bl_rnd /= Q;

            TieredOMapConfig tc;
            tc.total_keys = N; tc.hot_set_size = n;
            tc.mode = SecurityMode::TierMembership; tc.use_split_oram = true;
            TieredOMap tm(tc); tm.init(data, hk);
            ZipfSampler zt(N, s, 42);
            double ans_sum = 0, hot_sum = 0;
            int hot_cnt = 0;
            for (int q = 0; q < W; ++q) tm.access(zt.sample());
            for (int q = 0; q < Q; ++q) {
                auto r = tm.access(zt.sample());
                ans_sum += r.rounds_to_answer;
                if (r.found_in_hot) { hot_sum += r.rounds_to_answer; ++hot_cnt; }
            }
            double exp_rnd = ans_sum / Q;
            double hot_rnd = hot_cnt > 0 ? hot_sum / hot_cnt : exp_rnd;

            double bl_ms = bl_rnd * 80, hot_ms = hot_rnd * 80, exp_ms = exp_rnd * 80;
            std::cout << std::fixed
                      << std::setw(6) << std::setprecision(1) << s
                      << std::setw(8) << hot_cnt*100/Q
                      << std::setw(12) << std::setprecision(0) << bl_ms
                      << std::setw(12) << hot_ms
                      << std::setw(12) << exp_ms
                      << std::setw(10) << (bl_ms - hot_ms) << "ms"
                      << "\n";
        }
    }

    // ── Exp 3: Answer rounds distribution (percentiles) ──
    std::cout << "\n=== Answer Rounds Distribution (N=2^16 n=1024 s=1.0 Q=2000) ===\n\n";
    {
        int N = 1 << 16;
        auto data = make_data(N);
        auto hk = make_hk(n);

        TieredOMapConfig tc;
        tc.total_keys = N; tc.hot_set_size = n;
        tc.mode = SecurityMode::TierMembership; tc.use_split_oram = true;
        TieredOMap tm(tc); tm.init(data, hk);
        ZipfSampler z(N, 1.0, 42);
        for (int q = 0; q < W; ++q) tm.access(z.sample());

        std::vector<int> rounds_vec;
        for (int q = 0; q < 2000; ++q) {
            auto r = tm.access(z.sample());
            rounds_vec.push_back(r.rounds_to_answer);
        }
        std::sort(rounds_vec.begin(), rounds_vec.end());

        auto pct = [&](double p) { return rounds_vec[(int)(p * rounds_vec.size())]; };
        std::cout << "Baseline (Single OMAP): always 24 rounds\n";
        std::cout << "TieredOMap answer rounds:\n";
        std::cout << "  p50=" << pct(0.50) << "  p75=" << pct(0.75)
                  << "  p90=" << pct(0.90) << "  p95=" << pct(0.95)
                  << "  p99=" << pct(0.99) << "\n";
        std::cout << "  min=" << rounds_vec.front() << "  max=" << rounds_vec.back() << "\n";

        int below_bl = 0;
        for (int r : rounds_vec) if (r < 24) ++below_bl;
        std::cout << "  " << below_bl*100/2000 << "% of queries answered faster than Single OMAP\n";
    }

    return 0;
}
