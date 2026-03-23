#include "tiered_omap/omap/da_ost_omap.h"
#include "tiered_omap/tiered_omap.h"
#include "tiered_omap/workload.h"
#include "tiered_omap/common.h"
#include <gtest/gtest.h>
#include <numeric>

using namespace tiered_omap;

TEST(DaOstMaint, SearchThenRemoveSameKey) {
    DaOstOmap omap(64, OdsTreeType::AVL);
    std::vector<std::pair<int,Bytes>> data;
    for (int i = 0; i < 32; ++i) data.emplace_back(i, int_to_bytes(i));
    omap.init(data);

    for (int trial = 0; trial < 10; ++trial) {
        int key = trial % 32;
        auto val = omap.search(key);
        EXPECT_EQ(bytes_to_int(val), key);
        omap.remove(key);
        omap.insert(key, int_to_bytes(key + 1000));
        val = omap.search(key);
        EXPECT_EQ(bytes_to_int(val), key + 1000);
        omap.remove(key);
        omap.insert(key, int_to_bytes(key));
    }
}

TEST(DaOstMaint, TwoOmapPromoteDemote) {
    int n = 32;
    int N = 128;
    int hot_cap = n * 2;
    int cold_cap = N;

    DaOstOmap hot_omap(hot_cap, OdsTreeType::AVL);
    DaOstOmap cold_omap(cold_cap, OdsTreeType::AVL);

    std::vector<std::pair<int,Bytes>> hot_data, cold_data;
    for (int i = 0; i < n; ++i) hot_data.emplace_back(i, int_to_bytes(i));
    for (int i = n; i < N; ++i) cold_data.emplace_back(i, int_to_bytes(i));

    hot_omap.init(hot_data);
    cold_omap.init(cold_data);

    // Do some searches first
    for (int i = 0; i < 20; ++i) {
        hot_omap.search(i % n);
        cold_omap.search(n + (i % (N - n)));
    }

    // Demote key 5 from hot to cold
    auto raw = hot_omap.search(5);
    hot_omap.remove(5);
    cold_omap.insert(5, raw);

    // Verify 5 is in cold now
    auto val = cold_omap.search(5);
    EXPECT_EQ(bytes_to_int(val), 5);

    // Promote key 50 from cold to hot
    raw = cold_omap.search(50);
    cold_omap.remove(50);
    hot_omap.insert(50, raw);

    // Verify 50 is in hot now
    val = hot_omap.search(50);
    EXPECT_EQ(bytes_to_int(val), 50);

    // Do more searches to verify stability
    for (int i = 0; i < 30; ++i) {
        if (i != 5 && i < n) hot_omap.search(i);
        else if (i == 5) cold_omap.search(i);
    }
    hot_omap.search(50);
    cold_omap.search(5);
}

TEST(DaOstMaint, RepeatedPromoteDemoteCycles) {
    int n = 32;
    int N = 128;

    DaOstOmap hot_omap(n * 2, OdsTreeType::AVL);
    DaOstOmap cold_omap(N, OdsTreeType::AVL);

    std::vector<std::pair<int,Bytes>> hot_data, cold_data;
    for (int i = 0; i < n; ++i) hot_data.emplace_back(i, int_to_bytes(i));
    for (int i = n; i < N; ++i) cold_data.emplace_back(i, int_to_bytes(i));

    hot_omap.init(hot_data);
    cold_omap.init(cold_data);

    // Simulate multiple maintenance cycles
    for (int cycle = 0; cycle < 20; ++cycle) {
        int demote_key = cycle % n;
        int promote_key = n + (cycle % (N - n));

        // Demote
        auto raw = hot_omap.search(demote_key);
        hot_omap.remove(demote_key);
        cold_omap.insert(demote_key, raw);

        // Promote
        raw = cold_omap.search(promote_key);
        cold_omap.remove(promote_key);
        hot_omap.insert(promote_key, raw);

        // Some normal accesses
        for (int i = 0; i < 5; ++i) {
            hot_omap.dummy_access();
            cold_omap.dummy_access();
        }
    }
}

TEST(DaOstMaint, TieredOMapDaAvlMaintenance) {
    int N = 512;
    int n = 64;

    std::vector<std::pair<int,Bytes>> data;
    for (int i = 0; i < N; ++i) data.emplace_back(i, int_to_bytes(i));
    std::vector<int> hk(n);
    std::iota(hk.begin(), hk.end(), 0);

    TieredOMapConfig tc;
    tc.total_keys = N; tc.hot_set_size = n;
    tc.mode = SecurityMode::FullOblivious;
    tc.backend = OmapBackend::DaAvl;
    tc.maintenance.enabled = true;
    tc.maintenance.observation_window = 128;
    tc.maintenance.swap_interval = 128;
    tc.maintenance.promote_threshold = 5;
    tc.maintenance.demote_threshold = 2;
    tc.maintenance.staleness_windows = 3;
    TieredOMap tm(tc); tm.init(data, hk);
    ZipfSampler z(N, 1.0, 42);

    for (int q = 0; q < 3000; ++q) {
        tm.access(z.sample());
    }
}

TEST(DaOstMaint, TieredOMapDaAvlPiggyback) {
    int N = 512;
    int n = 64;

    std::vector<std::pair<int,Bytes>> data;
    for (int i = 0; i < N; ++i) data.emplace_back(i, int_to_bytes(i));
    std::vector<int> hk(n);
    std::iota(hk.begin(), hk.end(), 0);

    TieredOMapConfig tc;
    tc.total_keys = N; tc.hot_set_size = n;
    tc.mode = SecurityMode::FullOblivious;
    tc.backend = OmapBackend::DaAvl;
    tc.maintenance.enabled = true;
    tc.maintenance.piggyback = true;
    tc.maintenance.observation_window = 128;
    tc.maintenance.swap_interval = 128;
    tc.maintenance.promote_threshold = 5;
    tc.maintenance.demote_threshold = 2;
    tc.maintenance.staleness_windows = 3;
    TieredOMap tm(tc); tm.init(data, hk);
    ZipfSampler z(N, 1.0, 42);

    int initial_hot = tm.hot_set_size();
    for (int q = 0; q < 3000; ++q) {
        tm.access(z.sample());
    }
    EXPECT_GT(initial_hot, 0);
}

TEST(DaOstMaint, TieredOMapDaBplusPiggyback) {
    int N = 512;
    int n = 64;

    std::vector<std::pair<int,Bytes>> data;
    for (int i = 0; i < N; ++i) data.emplace_back(i, int_to_bytes(i));
    std::vector<int> hk(n);
    std::iota(hk.begin(), hk.end(), 0);

    TieredOMapConfig tc;
    tc.total_keys = N; tc.hot_set_size = n;
    tc.mode = SecurityMode::FullOblivious;
    tc.backend = OmapBackend::DaBplus;
    tc.maintenance.enabled = true;
    tc.maintenance.piggyback = true;
    tc.maintenance.observation_window = 128;
    tc.maintenance.swap_interval = 128;
    tc.maintenance.promote_threshold = 5;
    tc.maintenance.demote_threshold = 2;
    tc.maintenance.staleness_windows = 3;
    TieredOMap tm(tc); tm.init(data, hk);
    ZipfSampler z(N, 1.0, 42);

    int initial_hot = tm.hot_set_size();
    for (int q = 0; q < 3000; ++q) {
        tm.access(z.sample());
    }
    EXPECT_GT(initial_hot, 0);
}

TEST(DaOstMaint, BPlusTwoOmapPromoteDemote) {
    int n = 32;
    int N = 128;

    DaOstOmap hot_omap(n * 2, OdsTreeType::BPlus);
    DaOstOmap cold_omap(N, OdsTreeType::BPlus);

    std::vector<std::pair<int,Bytes>> hot_data, cold_data;
    for (int i = 0; i < n; ++i) hot_data.emplace_back(i, int_to_bytes(i));
    for (int i = n; i < N; ++i) cold_data.emplace_back(i, int_to_bytes(i));

    hot_omap.init(hot_data);
    cold_omap.init(cold_data);

    for (int cycle = 0; cycle < 20; ++cycle) {
        int demote_key = cycle % n;
        int promote_key = n + (cycle % (N - n));

        auto raw = hot_omap.search(demote_key);
        hot_omap.remove(demote_key);
        cold_omap.insert(demote_key, raw);

        raw = cold_omap.search(promote_key);
        cold_omap.remove(promote_key);
        hot_omap.insert(promote_key, raw);

        for (int i = 0; i < 3; ++i) {
            hot_omap.dummy_access();
            cold_omap.dummy_access();
        }
    }
}
