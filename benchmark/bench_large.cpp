#include "tiered_omap/tiered_omap.h"
#include "tiered_omap/omap/avl_omap.h"
#include "tiered_omap/workload.h"
#include <cmath>
#include <iostream>
#include <iomanip>
#include <numeric>
#include <vector>

using namespace tiered_omap;

int main(int argc, char** argv) {
    int logN = 22;
    if (argc > 1) logN = std::atoi(argv[1]);
    int N = 1 << logN;
    int n = 1024;
    double s = 1.0;
    int Q = 100;
    int W = 30;

    std::cout << "N=2^" << logN << " (" << N << ")  n=" << n
              << "  s=" << s << "  Q=" << Q << "\n\n";

    std::vector<std::pair<int,Bytes>> data;
    data.reserve(N);
    for (int i = 0; i < N; ++i) data.emplace_back(i, int_to_bytes(i));
    std::vector<int> hk(n);
    std::iota(hk.begin(), hk.end(), 0);

    auto run_bl = [&]() {
        std::cout << "  [1/4] Single OMAP..." << std::flush;
        AVLOmap bl(N); bl.init(data);
        ZipfSampler z(N, s, 42);
        for (int q = 0; q < W; ++q) bl.search(z.sample());
        double bw = 0, rnd = 0;
        for (int q = 0; q < Q; ++q) {
            bl.search(z.sample());
            bw += bl.last_stats().total_bytes();
            rnd += bl.last_stats().rounds;
        }
        std::cout << " BW=" << (int)(bw/Q/1024) << "KB  rnd=" << std::fixed
                  << std::setprecision(1) << rnd/Q << "\n";
        return std::make_pair(bw/Q, rnd/Q);
    };

    auto run_tm = [&](const char* label, SecurityMode mode, bool split) {
        std::cout << "  " << label << "..." << std::flush;
        TieredOMapConfig c;
        c.total_keys = N; c.hot_set_size = n;
        c.mode = mode; c.use_split_oram = split;
        TieredOMap tm(c); tm.init(data, hk);
        ZipfSampler z(N, s, 42);
        for (int q = 0; q < W; ++q) tm.access(z.sample());
        double bw = 0, rnd = 0, ans = 0;
        int hot = 0;
        double hot_ans_sum = 0;
        for (int q = 0; q < Q; ++q) {
            auto r = tm.access(z.sample());
            bw += r.total_bw.total_bytes();
            rnd += r.total_bw.rounds;
            ans += r.rounds_to_answer;
            if (r.found_in_hot) { ++hot; hot_ans_sum += r.rounds_to_answer; }
        }
        double hot_rnd = hot > 0 ? hot_ans_sum / hot : 0;
        std::cout << " BW=" << (int)(bw/Q/1024) << "KB  rnd=" << std::fixed
                  << std::setprecision(1) << rnd/Q << "  ans_rnd=" << ans/Q
                  << "  hot_rnd=" << hot_rnd
                  << "  hit=" << hot*100/Q << "%\n";
    };

    auto [bl_bw, bl_rnd] = run_bl();
    run_tm("[2/4] TM+split ", SecurityMode::TierMembership, true);
    run_tm("[3/4] FO+split ", SecurityMode::FullOblivious, true);
    run_tm("[4/4] FO nosplit", SecurityMode::FullOblivious, false);

    std::cout << "\n--- Summary for paper ---\n";
    std::cout << "BL_rnd=" << std::setprecision(1) << bl_rnd
              << "  hot_rnd=15  reduction=" << std::setprecision(0)
              << (1.0 - 15.0/bl_rnd)*100 << "%\n";
    std::cout << "WAN-80: BL=" << (int)(bl_rnd*80) << "ms  hot=1200ms  save="
              << (int)(bl_rnd*80 - 1200) << "ms\n";

    return 0;
}
