#include "tiered_omap/tiered_omap.h"
#include "tiered_omap/omap/avl_omap.h"
#include "tiered_omap/workload.h"
#include <chrono>
#include <cmath>
#include <iostream>
#include <iomanip>
#include <numeric>
#include <vector>

using namespace tiered_omap;
using Clock = std::chrono::high_resolution_clock;

int main() {
    const int N = 1 << 16;
    const int n = 1024;
    const double s = 1.0;
    const int Q = 200;
    const int W = 100;

    std::vector<std::pair<int, Bytes>> data;
    data.reserve(N);
    for (int i = 0; i < N; ++i) data.emplace_back(i, int_to_bytes(i));
    std::vector<int> hk(n);
    std::iota(hk.begin(), hk.end(), 0);

    auto measure = [&](const char* label, auto run_fn) {
        ZipfSampler z(N, s, 42);
        for (int q = 0; q < W; ++q) run_fn(z.sample());
        double total_us = 0;
        ZipfSampler z2(N, s, 99);
        for (int q = 0; q < Q; ++q) {
            int k = z2.sample();
            auto t0 = Clock::now();
            run_fn(k);
            auto t1 = Clock::now();
            total_us += std::chrono::duration<double, std::micro>(t1 - t0).count();
        }
        double avg_ms = total_us / Q / 1000.0;
        std::cout << std::setw(20) << label
                  << "  avg_comp=" << std::fixed << std::setprecision(3)
                  << avg_ms << " ms\n";
    };

    // Single OMAP
    {
        AVLOmap bl(N);
        bl.init(data);
        measure("Single OMAP", [&](int k) { bl.search(k); });
    }

    // TM + split
    {
        TieredOMapConfig c;
        c.total_keys = N; c.hot_set_size = n;
        c.mode = SecurityMode::TierMembership;
        c.use_split_oram = true;
        TieredOMap tm(c);
        tm.init(data, hk);
        measure("TM (split)", [&](int k) { tm.access(k); });
    }

    // FO + split
    {
        TieredOMapConfig c;
        c.total_keys = N; c.hot_set_size = n;
        c.mode = SecurityMode::FullOblivious;
        c.use_split_oram = true;
        TieredOMap fo(c);
        fo.init(data, hk);
        measure("FO (split)", [&](int k) { fo.access(k); });
    }

    return 0;
}
