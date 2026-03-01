#include <gtest/gtest.h>
#include "tiered_omap/tiered_omap.h"

using namespace tiered_omap;

static std::vector<std::pair<int, Bytes>> make_data(int N) {
    std::vector<std::pair<int, Bytes>> data;
    for (int i = 0; i < N; ++i)
        data.emplace_back(i, int_to_bytes(i * 100));
    return data;
}

TEST(TieredOMap, FullObliviousBasic) {
    TieredOMapConfig cfg;
    cfg.total_keys = 64;
    cfg.hot_set_size = 8;
    cfg.mode = SecurityMode::FullOblivious;

    TieredOMap tmap(cfg);

    auto data = make_data(64);
    std::vector<int> hot_keys = {0, 1, 2, 3, 4, 5, 6, 7};
    tmap.init(data, hot_keys);

    // Access hot keys.
    for (int k : hot_keys) {
        auto result = tmap.access(k);
        EXPECT_EQ(bytes_to_int(result.value), k * 100);
        EXPECT_TRUE(result.found_in_hot);
    }

    // Access cold keys.
    for (int k = 8; k < 64; ++k) {
        auto result = tmap.access(k);
        EXPECT_EQ(bytes_to_int(result.value), k * 100);
        EXPECT_FALSE(result.found_in_hot);
    }
}

TEST(TieredOMap, TierMembershipBasic) {
    TieredOMapConfig cfg;
    cfg.total_keys = 32;
    cfg.hot_set_size = 4;
    cfg.mode = SecurityMode::TierMembership;

    TieredOMap tmap(cfg);

    auto data = make_data(32);
    std::vector<int> hot_keys = {0, 1, 2, 3};
    tmap.init(data, hot_keys);

    for (int k = 0; k < 32; ++k) {
        auto result = tmap.access(k);
        EXPECT_EQ(bytes_to_int(result.value), k * 100);
        EXPECT_EQ(result.found_in_hot, k < 4);
    }
}

TEST(TieredOMap, WriteAndReadBack) {
    TieredOMapConfig cfg;
    cfg.total_keys = 16;
    cfg.hot_set_size = 4;
    cfg.mode = SecurityMode::FullOblivious;

    TieredOMap tmap(cfg);

    auto data = make_data(16);
    std::vector<int> hot_keys = {0, 1, 2, 3};
    tmap.init(data, hot_keys);

    // Update a hot key.
    Bytes new_val = int_to_bytes(9999);
    auto r1 = tmap.access(0, &new_val);
    EXPECT_TRUE(r1.found_in_hot);

    auto r2 = tmap.access(0);
    EXPECT_EQ(bytes_to_int(r2.value), 9999);

    // Update a cold key.
    Bytes cold_val = int_to_bytes(8888);
    auto r3 = tmap.access(10, &cold_val);
    EXPECT_FALSE(r3.found_in_hot);

    auto r4 = tmap.access(10);
    EXPECT_EQ(bytes_to_int(r4.value), 8888);
}

TEST(TieredOMap, RepeatedAccess) {
    TieredOMapConfig cfg;
    cfg.total_keys = 32;
    cfg.hot_set_size = 4;
    cfg.mode = SecurityMode::FullOblivious;

    TieredOMap tmap(cfg);

    auto data = make_data(32);
    std::vector<int> hot_keys = {0, 1, 2, 3};
    tmap.init(data, hot_keys);

    for (int round = 0; round < 50; ++round) {
        int key = round % 32;
        auto result = tmap.access(key);
        EXPECT_EQ(bytes_to_int(result.value), key * 100);
    }
}

// ─── Split-ORAM tests ───────────────────────────────────────────────────────

TEST(TieredOMap, SplitORAM_FullOblivious) {
    TieredOMapConfig cfg;
    cfg.total_keys = 128;
    cfg.hot_set_size = 16;
    cfg.mode = SecurityMode::FullOblivious;
    cfg.use_split_oram = true;

    TieredOMap tmap(cfg);

    auto data = make_data(128);
    std::vector<int> hot_keys;
    for (int i = 0; i < 16; ++i) hot_keys.push_back(i);
    tmap.init(data, hot_keys);

    for (int k = 0; k < 128; ++k) {
        auto result = tmap.access(k);
        EXPECT_EQ(bytes_to_int(result.value), k * 100)
            << "Mismatch at key " << k;
        EXPECT_EQ(result.found_in_hot, k < 16);
    }
}

TEST(TieredOMap, SplitORAM_TierMembership) {
    TieredOMapConfig cfg;
    cfg.total_keys = 128;
    cfg.hot_set_size = 16;
    cfg.mode = SecurityMode::TierMembership;
    cfg.use_split_oram = true;

    TieredOMap tmap(cfg);

    auto data = make_data(128);
    std::vector<int> hot_keys;
    for (int i = 0; i < 16; ++i) hot_keys.push_back(i);
    tmap.init(data, hot_keys);

    for (int k = 0; k < 128; ++k) {
        auto result = tmap.access(k);
        EXPECT_EQ(bytes_to_int(result.value), k * 100)
            << "Mismatch at key " << k;
        EXPECT_EQ(result.found_in_hot, k < 16);
    }

    // TierMembership hot query should cost less bandwidth than cold.
    auto hot_r = tmap.access(0);
    auto cold_r = tmap.access(64);
    EXPECT_LT(hot_r.total_bw.total_bytes(), cold_r.total_bw.total_bytes());
}

TEST(TieredOMap, SplitORAM_BandwidthSaving) {
    int N = 256, n = 16;
    TieredOMapConfig cfg_split, cfg_nosplit;
    cfg_split.total_keys = cfg_nosplit.total_keys = N;
    cfg_split.hot_set_size = cfg_nosplit.hot_set_size = n;
    cfg_split.mode = cfg_nosplit.mode = SecurityMode::TierMembership;
    cfg_split.use_split_oram = true;
    cfg_nosplit.use_split_oram = false;

    TieredOMap tmap_s(cfg_split), tmap_ns(cfg_nosplit);

    auto data = make_data(N);
    std::vector<int> hot_keys;
    for (int i = 0; i < n; ++i) hot_keys.push_back(i);
    tmap_s.init(data, hot_keys);
    tmap_ns.init(data, hot_keys);

    auto r_split = tmap_s.access(0);
    auto r_nosplit = tmap_ns.access(0);
    EXPECT_LT(r_split.total_bw.total_bytes(), r_nosplit.total_bw.total_bytes())
        << "Split-ORAM hot query should use less bandwidth than non-split";
}
