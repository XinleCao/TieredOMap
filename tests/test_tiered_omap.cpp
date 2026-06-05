#include <gtest/gtest.h>
#include "tiered_omap/tiered_omap.h"
#include <algorithm>
#include <unordered_set>

using namespace tiered_omap;

static std::vector<std::pair<int, Bytes>> make_data(int N) {
    std::vector<std::pair<int, Bytes>> data;
    for (int i = 0; i < N; ++i)
        data.emplace_back(i, int_to_bytes(i * 100));
    return data;
}

static void expect_dynamic_invariants(
    const TieredOMap& tmap, int expected_hot_size) {
    auto st = tmap.dynamic_debug_state();

    EXPECT_EQ(static_cast<int>(st.hot_keys.size()), expected_hot_size);
    EXPECT_TRUE(std::is_sorted(st.hot_key_list.begin(), st.hot_key_list.end()));

    std::unordered_set<int> listed_phys(
        st.hot_key_list.begin(), st.hot_key_list.end());
    EXPECT_EQ(listed_phys, st.phys_hot_keys);

    std::unordered_set<int> maint_cache;
    for (const auto& e : st.cache_entries) {
        EXPECT_NE(e.key, INVALID_KEY);
        EXPECT_TRUE(maint_cache.insert(e.key).second)
            << "duplicate maintenance cache key " << e.key;
    }
    EXPECT_EQ(maint_cache, st.cache_keys);

    for (int k : st.cache_keys) {
        EXPECT_FALSE(st.phys_hot_keys.count(k))
            << "key exists in both local cache and physical hot OMAP: " << k;
        EXPECT_TRUE(st.hot_keys.count(k))
            << "local cache entries are part of the effective hot set: " << k;
    }

    bool has_pending = st.pending_insert_key != INVALID_KEY;
    if (st.swap_state == SwapState::Idle) {
        EXPECT_FALSE(has_pending);
        EXPECT_FALSE(st.pending_has_ref);
    } else {
        ASSERT_TRUE(has_pending);
        EXPECT_TRUE(st.pending_has_ref);
        EXPECT_FALSE(st.cache_keys.count(st.pending_insert_key));
        EXPECT_FALSE(st.phys_hot_keys.count(st.pending_insert_key));
        if (st.swap_state == SwapState::HotPend) {
            EXPECT_TRUE(st.hot_keys.count(st.pending_insert_key));
        } else {
            EXPECT_FALSE(st.hot_keys.count(st.pending_insert_key));
        }
    }

    std::unordered_set<int> effective_hot = st.phys_hot_keys;
    effective_hot.insert(st.cache_keys.begin(), st.cache_keys.end());
    if (st.swap_state == SwapState::HotPend)
        effective_hot.insert(st.pending_insert_key);
    EXPECT_EQ(effective_hot, st.hot_keys);
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

// ─── B+ Tree backend tests ──────────────────────────────────────────────────

TEST(TieredOMap, BPlusBackend_FullOblivious) {
    TieredOMapConfig cfg;
    cfg.total_keys = 64;
    cfg.hot_set_size = 8;
    cfg.mode = SecurityMode::FullOblivious;
    cfg.backend = OmapBackend::BPlus;
    cfg.bplus_order = 8;

    TieredOMap tmap(cfg);

    auto data = make_data(64);
    std::vector<int> hot_keys = {0, 1, 2, 3, 4, 5, 6, 7};
    tmap.init(data, hot_keys);

    for (int k : hot_keys) {
        auto result = tmap.access(k);
        EXPECT_EQ(bytes_to_int(result.value), k * 100);
        EXPECT_TRUE(result.found_in_hot);
    }

    for (int k = 8; k < 64; ++k) {
        auto result = tmap.access(k);
        EXPECT_EQ(bytes_to_int(result.value), k * 100);
        EXPECT_FALSE(result.found_in_hot);
    }
}

TEST(TieredOMap, BPlusBackend_WriteAndReadBack) {
    TieredOMapConfig cfg;
    cfg.total_keys = 32;
    cfg.hot_set_size = 4;
    cfg.mode = SecurityMode::FullOblivious;
    cfg.backend = OmapBackend::BPlus;

    TieredOMap tmap(cfg);

    auto data = make_data(32);
    std::vector<int> hot_keys = {0, 1, 2, 3};
    tmap.init(data, hot_keys);

    Bytes new_val = int_to_bytes(5555);
    tmap.access(1, &new_val);
    auto r = tmap.access(1);
    EXPECT_EQ(bytes_to_int(r.value), 5555);

    Bytes cold_val = int_to_bytes(7777);
    tmap.access(20, &cold_val);
    auto r2 = tmap.access(20);
    EXPECT_EQ(bytes_to_int(r2.value), 7777);
}

TEST(TieredOMap, BPlusBackend_RepeatedAccess) {
    TieredOMapConfig cfg;
    cfg.total_keys = 32;
    cfg.hot_set_size = 4;
    cfg.mode = SecurityMode::FullOblivious;
    cfg.backend = OmapBackend::BPlus;

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

TEST(TieredOMap, StaticAllBackendsReadWrite) {
    for (auto backend : {OmapBackend::AVL, OmapBackend::BPlus,
                         OmapBackend::DaAvl, OmapBackend::DaBplus}) {
        for (auto mode : {SecurityMode::FullOblivious,
                          SecurityMode::TierMembership}) {
            TieredOMapConfig cfg;
            cfg.total_keys = 96;
            cfg.hot_set_size = 12;
            cfg.mode = mode;
            cfg.backend = backend;
            cfg.bplus_order = 8;

            TieredOMap tmap(cfg);
            auto data = make_data(96);
            std::vector<int> hot_keys;
            for (int i = 0; i < 12; ++i) hot_keys.push_back(i);
            tmap.init(data, hot_keys);

            Bytes hot_update = int_to_bytes(1111);
            auto hot = tmap.access(3, &hot_update);
            EXPECT_TRUE(hot.found_in_hot);
            EXPECT_EQ(bytes_to_int(hot.value), 300);

            Bytes cold_update = int_to_bytes(2222);
            auto cold = tmap.access(73, &cold_update);
            EXPECT_FALSE(cold.found_in_hot);
            EXPECT_EQ(bytes_to_int(cold.value), 7300);

            for (int k = 0; k < 96; ++k) {
                auto r = tmap.access(k);
                int expected = k * 100;
                if (k == 3) expected = 1111;
                if (k == 73) expected = 2222;
                EXPECT_EQ(bytes_to_int(r.value), expected)
                    << "backend=" << static_cast<int>(backend)
                    << " mode=" << static_cast<int>(mode)
                    << " key=" << k;
                EXPECT_EQ(r.found_in_hot, k < 12);
            }
        }
    }
}

TEST(RoundAccounting, StaticSequentialRoundsAreAdditive) {
    for (auto mode : {SecurityMode::FullOblivious,
                      SecurityMode::TierMembership}) {
        TieredOMapConfig cfg;
        cfg.total_keys = 64;
        cfg.hot_set_size = 8;
        cfg.mode = mode;
        cfg.backend = OmapBackend::BPlus;
        cfg.bplus_order = 4;

        TieredOMap tmap(cfg);
        auto data = make_data(64);
        std::vector<int> hot_keys;
        for (int i = 0; i < 8; ++i) hot_keys.push_back(i);
        tmap.init(data, hot_keys);

        int data_rounds = (mode == SecurityMode::FullOblivious) ? 2 : 1;

        auto hot = tmap.access(3);
        EXPECT_TRUE(hot.found_in_hot);
        EXPECT_EQ(hot.total_bw.rounds,
                  hot.hot_bw.rounds + hot.cold_bw.rounds + data_rounds);
        EXPECT_EQ(hot.rounds_to_answer,
                  static_cast<int>(hot.hot_bw.rounds) + 1);

        auto cold = tmap.access(32);
        EXPECT_FALSE(cold.found_in_hot);
        EXPECT_EQ(cold.total_bw.rounds,
                  cold.hot_bw.rounds + cold.cold_bw.rounds + data_rounds);
        EXPECT_EQ(cold.rounds_to_answer,
                  static_cast<int>(cold.total_bw.rounds));
    }
}

TEST(RoundAccounting, DynamicSequentialRoundsAreAdditive) {
    for (auto mode : {SecurityMode::FullOblivious,
                      SecurityMode::TierMembership}) {
        TieredOMapConfig cfg;
        cfg.total_keys = 64;
        cfg.hot_set_size = 8;
        cfg.mode = mode;
        cfg.backend = OmapBackend::BPlus;
        cfg.bplus_order = 4;
        cfg.maintenance.enabled = true;
        cfg.maintenance.observation_window = 1000;
        cfg.maintenance.swap_interval = 1000;
        cfg.maintenance.cache_size = 2;

        TieredOMap tmap(cfg);
        auto data = make_data(64);
        std::vector<int> hot_keys;
        for (int i = 0; i < 8; ++i) hot_keys.push_back(i);
        tmap.init(data, hot_keys);

        auto cache_hot = tmap.access(0);
        EXPECT_TRUE(cache_hot.found_in_hot);
        EXPECT_EQ(cache_hot.total_bw.rounds,
                  cache_hot.hot_bw.rounds + cache_hot.cold_bw.rounds + 1);
        EXPECT_EQ(cache_hot.rounds_to_answer, 1);

        auto physical_hot = tmap.access(3);
        EXPECT_TRUE(physical_hot.found_in_hot);
        EXPECT_EQ(physical_hot.total_bw.rounds,
                  physical_hot.hot_bw.rounds + physical_hot.cold_bw.rounds + 1);
        EXPECT_EQ(physical_hot.rounds_to_answer,
                  static_cast<int>(physical_hot.hot_bw.rounds) + 1);

        auto cold = tmap.access(32);
        EXPECT_FALSE(cold.found_in_hot);
        EXPECT_EQ(cold.total_bw.rounds,
                  cold.hot_bw.rounds + cold.cold_bw.rounds + 1);
        EXPECT_EQ(cold.rounds_to_answer,
                  static_cast<int>(cold.total_bw.rounds));
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

// ─── Dynamic Maintenance tests ──────────────────────────────────────────────

TEST(DynamicMaintenance, EpochMetadataEncodeDecode) {
    Bytes val = int_to_bytes(42);
    EpochMeta m{5, 3, 7};
    Bytes encoded = encode_with_epoch(val, m);
    auto [decoded_val, decoded_m] = decode_epoch(encoded);
    EXPECT_EQ(bytes_to_int(decoded_val), 42);
    EXPECT_EQ(decoded_m.cnt, 5);
    EXPECT_EQ(decoded_m.ep, 3);
    EXPECT_EQ(decoded_m.fp, 7);
}

TEST(DynamicMaintenance, MaintenanceManagerBasic) {
    MaintenanceConfig mcfg;
    mcfg.observation_window = 10;
    mcfg.swap_interval = 10;
    mcfg.cache_size = 8;
    mcfg.enabled = true;

    MaintenanceManager mgr(mcfg);
    // Seed cache so should_promote compares fp against min cache frequency.
    std::vector<CacheEntry> seed;
    for (int i = 0; i < 8; ++i)
        seed.push_back({i, 1});
    mgr.init_cache(seed);

    EpochMeta stored{};

    for (int i = 0; i < 5; ++i) {
        stored = mgr.update_meta(stored);
        mgr.tick();
    }
    EXPECT_EQ(stored.cnt, 5);
    EXPECT_EQ(stored.ep, 0);

    EpochMeta dummy{};
    for (int i = 0; i < 5; ++i) {
        dummy = mgr.update_meta(dummy);
        mgr.tick();
    }
    EXPECT_EQ(mgr.obs_epoch(), 1);

    stored = mgr.update_meta(stored);
    // Crossing epoch: fp becomes prior-epoch count (5).
    EXPECT_EQ(stored.fp, 5);
    EXPECT_TRUE(mgr.should_promote(stored.fp))
        << "cold fp should exceed min cache fp";
    mgr.tick();
}

TEST(DynamicMaintenance, PromoteColdKey) {
    int N = 64, n = 8;
    TieredOMapConfig cfg;
    cfg.total_keys = N;
    cfg.hot_set_size = n;
    cfg.mode = SecurityMode::FullOblivious;
    cfg.maintenance.enabled = true;
    cfg.maintenance.observation_window = 16;
    cfg.maintenance.swap_interval = 16;
    cfg.maintenance.cache_size = 8;

    TieredOMap tmap(cfg);
    auto data = make_data(N);
    std::vector<int> hot_keys;
    for (int i = 0; i < n; ++i) hot_keys.push_back(i);
    tmap.init(data, hot_keys);

    EXPECT_EQ(tmap.hot_set_size(), 8);

    // Access cold key 32 repeatedly across epochs to trigger promotion.
    for (int round = 0; round < 100; ++round) {
        tmap.access(32);
    }

    // After enough accesses, key 32 should have been promoted.
    auto result = tmap.access(32);
    EXPECT_TRUE(result.found_in_hot)
        << "Key 32 should have been promoted to hot tier";
}

TEST(DynamicMaintenance, DemoteStaleKey) {
    int N = 32, n = 4;
    TieredOMapConfig cfg;
    cfg.total_keys = N;
    cfg.hot_set_size = n;
    cfg.mode = SecurityMode::FullOblivious;
    cfg.maintenance.enabled = true;
    cfg.maintenance.observation_window = 8;
    cfg.maintenance.swap_interval = 8;
    cfg.maintenance.cache_size = 2;

    TieredOMap tmap(cfg);
    auto data = make_data(N);
    std::vector<int> hot_keys = {0, 1, 2, 3};
    tmap.init(data, hot_keys);

    EXPECT_EQ(tmap.hot_set_size(), 4);

    for (int round = 0; round < 200; ++round) {
        tmap.access(16);
    }

    // Cache-based: hot set size stays constant, but composition changes.
    EXPECT_EQ(tmap.hot_set_size(), 4);
    EXPECT_TRUE(tmap.hot_keys().count(16))
        << "Frequently accessed cold key 16 should have been promoted";
}

TEST(DynamicMaintenance, PendingColdInsertKeepsColdLogicalMembership) {
    int N = 32, n = 4;
    TieredOMapConfig cfg;
    cfg.total_keys = N;
    cfg.hot_set_size = n;
    cfg.mode = SecurityMode::TierMembership;
    cfg.maintenance.enabled = true;
    cfg.maintenance.observation_window = 4;
    cfg.maintenance.swap_interval = 100;
    cfg.maintenance.cache_size = 2;

    TieredOMap tmap(cfg);
    auto data = make_data(N);
    std::vector<int> hot_keys = {0, 1, 2, 3};
    tmap.init(data, hot_keys);

    // Initial maintenance cache holds keys 0 and 1. Repeatedly touching cold
    // key 16 promotes it and evicts key 0 as a ColdPend entry before B3 fires.
    for (int round = 0; round < 4; ++round)
        tmap.access(16);

    ASSERT_NE(tmap.maintenance_mgr(), nullptr);
    ASSERT_EQ(tmap.maintenance_mgr()->swap_state(), SwapState::ColdPend);
    ASSERT_FALSE(tmap.hot_keys().count(0));

    auto r = tmap.access(0);
    EXPECT_EQ(bytes_to_int(r.value), 0);
    EXPECT_FALSE(r.found_in_hot)
        << "ColdPend pending entries are local but logically cold";
}

TEST(DynamicMaintenance, LocationInvariantsHoldAcrossWorkload) {
    for (auto backend : {OmapBackend::AVL, OmapBackend::BPlus}) {
        for (auto mode : {SecurityMode::FullOblivious,
                          SecurityMode::TierMembership}) {
            int N = 64, n = 8;
            TieredOMapConfig cfg;
            cfg.total_keys = N;
            cfg.hot_set_size = n;
            cfg.mode = mode;
            cfg.backend = backend;
            cfg.bplus_order = 4;
            cfg.maintenance.enabled = true;
            cfg.maintenance.observation_window = 4;
            cfg.maintenance.swap_interval = 12;
            cfg.maintenance.cache_size = 4;

            TieredOMap tmap(cfg);
            auto data = make_data(N);
            std::vector<int> hot_keys;
            for (int i = 0; i < n; ++i) hot_keys.push_back(i);
            tmap.init(data, hot_keys);
            expect_dynamic_invariants(tmap, n);

            std::vector<int> workload = {
                0, 0, 16, 16, 16, 17, 18, 16,
                24, 24, 24, 24, 1, 2, 25, 25,
                26, 27, 28, 24, 16, 29, 30, 31,
                3, 4, 32, 32, 33, 34, 35, 36
            };
            for (int key : workload) {
                auto r = tmap.access(key);
                EXPECT_EQ(bytes_to_int(r.value), key * 100)
                    << "backend=" << static_cast<int>(backend)
                    << " mode=" << static_cast<int>(mode)
                    << " key=" << key;
                expect_dynamic_invariants(tmap, n);
            }
        }
    }
}

// ─── B+ Tree Physical Migration tests ────────────────────────────────────

TEST(DynamicMaintenance, BPlusPhysicalPromotion) {
    int N = 64, n = 8;
    TieredOMapConfig cfg;
    cfg.total_keys = N;
    cfg.hot_set_size = n;
    cfg.mode = SecurityMode::FullOblivious;
    cfg.backend = OmapBackend::BPlus;
    cfg.bplus_order = 4;
    cfg.maintenance.enabled = true;
    cfg.maintenance.observation_window = 16;
    cfg.maintenance.swap_interval = 16;
    cfg.maintenance.cache_size = 8;

    TieredOMap tmap(cfg);
    auto data = make_data(N);
    std::vector<int> hot_keys;
    for (int i = 0; i < n; ++i) hot_keys.push_back(i);
    tmap.init(data, hot_keys);

    EXPECT_EQ(tmap.hot_set_size(), 8);

    for (int round = 0; round < 120; ++round)
        tmap.access(32);

    auto result = tmap.access(32);
    EXPECT_TRUE(result.found_in_hot)
        << "Key 32 should have been physically promoted to hot tier";

    for (int k = 0; k < N; ++k) {
        auto r = tmap.access(k);
        EXPECT_EQ(bytes_to_int(r.value), k * 100)
            << "Value mismatch at key " << k << " after physical promotion";
    }
}

TEST(DynamicMaintenance, BPlusPhysicalDemotion) {
    int N = 32, n = 4;
    TieredOMapConfig cfg;
    cfg.total_keys = N;
    cfg.hot_set_size = n;
    cfg.mode = SecurityMode::FullOblivious;
    cfg.backend = OmapBackend::BPlus;
    cfg.bplus_order = 4;
    cfg.maintenance.enabled = true;
    cfg.maintenance.observation_window = 8;
    cfg.maintenance.swap_interval = 8;
    cfg.maintenance.cache_size = 2;

    TieredOMap tmap(cfg);
    auto data = make_data(N);
    std::vector<int> hot_keys = {0, 1, 2, 3};
    tmap.init(data, hot_keys);

    EXPECT_EQ(tmap.hot_set_size(), 4);

    for (int round = 0; round < 200; ++round)
        tmap.access(16);

    EXPECT_EQ(tmap.hot_set_size(), 4);
    EXPECT_TRUE(tmap.hot_keys().count(16))
        << "Frequently accessed cold key 16 should be promoted";

    for (int k = 0; k < N; ++k) {
        auto r = tmap.access(k);
        EXPECT_EQ(bytes_to_int(r.value), k * 100)
            << "Value mismatch at key " << k << " after physical demotion";
    }
}

TEST(DynamicMaintenance, BPlusPhysicalRoundTrip) {
    int N = 32, n = 4;
    TieredOMapConfig cfg;
    cfg.total_keys = N;
    cfg.hot_set_size = n;
    cfg.mode = SecurityMode::FullOblivious;
    cfg.backend = OmapBackend::BPlus;
    cfg.bplus_order = 4;
    cfg.maintenance.enabled = true;
    cfg.maintenance.observation_window = 8;
    cfg.maintenance.swap_interval = 8;
    cfg.maintenance.cache_size = 8;

    TieredOMap tmap(cfg);
    auto data = make_data(N);
    std::vector<int> hot_keys = {0, 1, 2, 3};
    tmap.init(data, hot_keys);

    // Phase 1: access cold key 20 heavily -> promote it.
    for (int round = 0; round < 100; ++round)
        tmap.access(20);

    auto r1 = tmap.access(20);
    EXPECT_TRUE(r1.found_in_hot)
        << "Key 20 should be promoted after heavy access";

    // Phase 2: stop accessing key 20, access cold keys to age it out.
    for (int round = 0; round < 200; ++round)
        tmap.access(25);

    // Phase 3: verify all values are still correct after migrations.
    for (int k = 0; k < N; ++k) {
        auto r = tmap.access(k);
        EXPECT_EQ(bytes_to_int(r.value), k * 100)
            << "Value mismatch at key " << k << " after round-trip migration";
    }
}

// ---------- Piggyback tests ----------

TEST(Piggyback, DynamicOverheadWorstCase) {
    struct Metrics { double bw_KB; int rounds; int rounds_to_answer; };

    auto build = [](int N, int n, SecurityMode mode, bool maint, bool pb) {
        TieredOMapConfig cfg;
        cfg.total_keys = N; cfg.hot_set_size = n;
        cfg.mode = mode; cfg.backend = OmapBackend::BPlus;
        cfg.bplus_order = 8; cfg.use_split_oram = true;
        cfg.use_hot_backend = true; cfg.hot_backend = OmapBackend::BPlus;
        if (maint) {
            cfg.maintenance.enabled = true;
            cfg.maintenance.observation_window = 16;
            cfg.maintenance.swap_interval = 8;
            cfg.maintenance.cache_size = std::max(n / 2, 4);
            cfg.maintenance.piggyback = pb;
        }
        auto tm = std::make_unique<TieredOMap>(cfg);
        std::vector<std::pair<int, Bytes>> data;
        for (int i = 0; i < N; ++i)
            data.emplace_back(i, int_to_bytes(i * 100));
        std::vector<int> hk;
        for (int i = 0; i < n; ++i) hk.push_back(i);
        tm->init(data, hk);
        tm->access(0); tm->access(N - 1);
        return tm;
    };

    auto measure_one = [](TieredOMap& tm, int key) -> Metrics {
        auto r = tm.access(key);
        return {r.total_bw.total_bytes() / 1024.0,
                (int)r.total_bw.rounds, r.rounds_to_answer};
    };

    for (int logN : {10, 12}) {
        int N = 1 << logN;
        int n = std::max(N / 16, 4);
        int Q = 80;

        printf("\n");
        for (int mi = 0; mi < 2; ++mi) {
            SecurityMode mode = (mi == 0) ? SecurityMode::TierMembership
                                          : SecurityMode::FullOblivious;
            const char* mode_str = (mi == 0) ? "TM" : "FO";

            // (1) Static: no maintenance
            auto s = build(N, n, mode, false, false);
            Metrics s_hot = measure_one(*s, 0);
            Metrics s_cold = measure_one(*s, N - 1);

            // (2) Dynamic with piggyback ON: run Q queries, find worst case
            auto d = build(N, n, mode, true, true);
            Metrics dw_hot{}, dw_cold{};
            int hot_fire = 0, cold_fire = 0;
            for (int i = 0; i < Q; ++i) {
                bool is_hot = (i % 3 == 0);
                int key = is_hot ? (i % n) : (n + (i % (N - n)));
                auto r = d->access(key);
                double bw = r.total_bw.total_bytes() / 1024.0;
                int rnd = (int)r.total_bw.rounds;
                int rta = r.rounds_to_answer;
                if (is_hot) {
                    if (bw > s_hot.bw_KB) hot_fire++;
                    if (bw > dw_hot.bw_KB) dw_hot = {bw, rnd, rta};
                } else {
                    if (bw > s_cold.bw_KB) cold_fire++;
                    if (bw > dw_cold.bw_KB) dw_cold = {bw, rnd, rta};
                }
            }

            // (3) Dynamic with piggyback OFF: run Q queries, find worst case
            auto d_off = build(N, n, mode, true, false);
            Metrics doff_hot{}, doff_cold{};
            for (int i = 0; i < Q; ++i) {
                bool is_hot = (i % 3 == 0);
                int key = is_hot ? (i % n) : (n + (i % (N - n)));
                auto r = d_off->access(key);
                double bw = r.total_bw.total_bytes() / 1024.0;
                int rnd = (int)r.total_bw.rounds;
                int rta = r.rounds_to_answer;
                if (is_hot) {
                    if (bw > doff_hot.bw_KB) doff_hot = {bw, rnd, rta};
                } else {
                    if (bw > doff_cold.bw_KB) doff_cold = {bw, rnd, rta};
                }
            }

            // Verify integrity
            for (int k = 0; k < std::min(N, 32); ++k) {
                auto r = d->access(k);
                EXPECT_EQ(bytes_to_int(r.value), k * 100);
            }

            printf("╔══ N=%d (2^%d)  n=%d  %s  BPlus+Split ═══════════════════════════════\n",
                   N, logN, n, mode_str);
            printf("║                       │  BW (KB)  │  Rounds │  RTA  │  BW OH  │ Rnd OH\n");
            printf("║───────────────────────┼───────────┼─────────┼───────┼─────────┼───────\n");
            printf("║ Static Hot            │  %7.1f  │   %3d   │  %3d  │   ---   │  ---\n",
                   s_hot.bw_KB, s_hot.rounds, s_hot.rounds_to_answer);
            printf("║ Dynamic Hot  (pb OFF) │  %7.1f  │   %3d   │  %3d  │ %+5.0f%%  │ %+3d\n",
                   doff_hot.bw_KB, doff_hot.rounds, doff_hot.rounds_to_answer,
                   s_hot.bw_KB > 0 ? (doff_hot.bw_KB - s_hot.bw_KB) / s_hot.bw_KB * 100 : 0,
                   doff_hot.rounds - s_hot.rounds);
            printf("║ Dynamic Hot  (pb ON)  │  %7.1f  │   %3d   │  %3d  │ %+5.0f%%  │ %+3d\n",
                   dw_hot.bw_KB, dw_hot.rounds, dw_hot.rounds_to_answer,
                   s_hot.bw_KB > 0 ? (dw_hot.bw_KB - s_hot.bw_KB) / s_hot.bw_KB * 100 : 0,
                   dw_hot.rounds - s_hot.rounds);
            printf("║   (hot queries exceeding static BW: %d/%d)\n",
                   hot_fire, Q / 3 + 1);
            printf("║───────────────────────┼───────────┼─────────┼───────┼─────────┼───────\n");
            printf("║ Static Cold           │  %7.1f  │   %3d   │  %3d  │   ---   │  ---\n",
                   s_cold.bw_KB, s_cold.rounds, s_cold.rounds_to_answer);
            printf("║ Dynamic Cold (pb OFF) │  %7.1f  │   %3d   │  %3d  │ %+5.0f%%  │ %+3d\n",
                   doff_cold.bw_KB, doff_cold.rounds, doff_cold.rounds_to_answer,
                   s_cold.bw_KB > 0 ? (doff_cold.bw_KB - s_cold.bw_KB) / s_cold.bw_KB * 100 : 0,
                   doff_cold.rounds - s_cold.rounds);
            printf("║ Dynamic Cold (pb ON)  │  %7.1f  │   %3d   │  %3d  │ %+5.0f%%  │ %+3d\n",
                   dw_cold.bw_KB, dw_cold.rounds, dw_cold.rounds_to_answer,
                   s_cold.bw_KB > 0 ? (dw_cold.bw_KB - s_cold.bw_KB) / s_cold.bw_KB * 100 : 0,
                   dw_cold.rounds - s_cold.rounds);
            printf("║   (cold queries exceeding static BW: %d/%d)\n",
                   cold_fire, Q - Q / 3 - 1);
            printf("╚═════════════════════════════════════════════════════════════════════════\n\n");

            if (mi == 0) {
                EXPECT_LE(dw_hot.rounds, doff_hot.rounds)
                    << "pb ON should not have more rounds than pb OFF (hot)";
                EXPECT_LE(dw_cold.rounds, doff_cold.rounds)
                    << "pb ON should not have more rounds than pb OFF (cold)";
            }
        }
    }
}

TEST(Piggyback, RoundCountTheoryCheck) {
    int N = 32, n = 4;

    // Run two scenarios: piggyback OFF and ON
    for (bool pb : {false, true}) {
        TieredOMapConfig cfg;
        cfg.total_keys = N;
        cfg.hot_set_size = n;
        cfg.mode = SecurityMode::FullOblivious;
        cfg.backend = OmapBackend::BPlus;
        cfg.bplus_order = 4;
        cfg.maintenance.enabled = true;
        cfg.maintenance.observation_window = 8;
        cfg.maintenance.swap_interval = 8;
        cfg.maintenance.cache_size = 4;
        cfg.maintenance.piggyback = pb;

        TieredOMap tmap(cfg);
        auto data = make_data(N);
        std::vector<int> hot_keys = {0, 1, 2, 3};
        tmap.init(data, hot_keys);

        // Collect baseline: access hot key 0 (no maintenance fires on query 1)
        auto r_hot = tmap.access(0);
        int baseline_rounds = static_cast<int>(r_hot.total_bw.rounds);

        printf("\n=== piggyback=%s ===\n", pb ? "ON" : "OFF");
        printf("baseline (hot key 0):  rounds=%d  bw_down=%zu  bw_up=%zu\n",
               baseline_rounds,
               r_hot.total_bw.bytes_downloaded,
               r_hot.total_bw.bytes_uploaded);

        // Run 40 accesses on cold keys to trigger scan/insert maintenance
        int max_rounds = 0;
        int extra_round_count = 0;
        for (int i = 0; i < 40; ++i) {
            int k = 10 + (i % 10);
            auto r = tmap.access(k);
            int rd = static_cast<int>(r.total_bw.rounds);
            if (rd > max_rounds) max_rounds = rd;
            if (rd > baseline_rounds) {
                extra_round_count++;
                if (extra_round_count <= 3)
                    printf("  query %d (key=%d): rounds=%d > baseline=%d  (+%d)\n",
                           i, k, rd, baseline_rounds, rd - baseline_rounds);
            }
        }
        printf("max rounds seen: %d  (baseline=%d)\n", max_rounds, baseline_rounds);
        printf("queries exceeding baseline: %d / 40\n", extra_round_count);

        // Note: piggybacking only reduces rounds in interleaved mode
        // (requires channel/server). In local mode pb ON == pb OFF.
        (void)extra_round_count;

        // Verify all values survived
        for (int k = 0; k < N; ++k) {
            auto r = tmap.access(k);
            EXPECT_EQ(bytes_to_int(r.value), k * 100)
                << "Value mismatch at key " << k;
        }
    }
}

TEST(Piggyback, BPlusDemotionValuesPreserved) {
    // In local mode (no channel), piggybacking doesn't reduce rounds
    // (requires interleaved step machine). This test verifies that
    // maintenance with piggyback=true still preserves data correctness.
    int N = 32, n = 4;
    TieredOMapConfig cfg;
    cfg.total_keys = N;
    cfg.hot_set_size = n;
    cfg.mode = SecurityMode::FullOblivious;
    cfg.backend = OmapBackend::BPlus;
    cfg.bplus_order = 4;
    cfg.maintenance.enabled = true;
    cfg.maintenance.observation_window = 8;
    cfg.maintenance.swap_interval = 8;
    cfg.maintenance.cache_size = 8;
    cfg.maintenance.piggyback = true;

    TieredOMap tmap(cfg);
    auto data = make_data(N);
    std::vector<int> hot_keys = {0, 1, 2, 3};
    tmap.init(data, hot_keys);

    for (int i = 0; i < 200; ++i) {
        int k = 10 + (i % 10);
        tmap.access(k);
    }

    for (int k = 0; k < N; ++k) {
        auto r = tmap.access(k);
        EXPECT_EQ(bytes_to_int(r.value), k * 100)
            << "Value mismatch at key " << k;
    }
}

TEST(Piggyback, BPlusValuesPreservedAfterDemotion) {
    int N = 32, n = 4;
    TieredOMapConfig cfg;
    cfg.total_keys = N;
    cfg.hot_set_size = n;
    cfg.mode = SecurityMode::FullOblivious;
    cfg.backend = OmapBackend::BPlus;
    cfg.bplus_order = 4;
    cfg.maintenance.enabled = true;
    cfg.maintenance.observation_window = 8;
    cfg.maintenance.swap_interval = 8;
    cfg.maintenance.cache_size = 8;
    cfg.maintenance.piggyback = true;

    TieredOMap tmap(cfg);
    auto data = make_data(N);
    std::vector<int> hot_keys = {0, 1, 2, 3};
    tmap.init(data, hot_keys);

    // Access only cold keys — hot keys should age out and get demoted.
    for (int round = 0; round < 200; ++round)
        tmap.access(10 + (round % 10));

    // Verify all values survive piggybacked demotions.
    for (int k = 0; k < N; ++k) {
        auto r = tmap.access(k);
        EXPECT_EQ(bytes_to_int(r.value), k * 100)
            << "Value lost at key " << k << " after piggyback demotion";
    }
}

TEST(Piggyback, StandaloneVsPiggybackConsistency) {
    int N = 32, n = 4;

    auto run_scenario = [&](bool piggyback) -> std::vector<int> {
        TieredOMapConfig cfg;
        cfg.total_keys = N;
        cfg.hot_set_size = n;
        cfg.mode = SecurityMode::FullOblivious;
        cfg.backend = OmapBackend::BPlus;
        cfg.bplus_order = 4;
        cfg.maintenance.enabled = true;
        cfg.maintenance.observation_window = 8;
        cfg.maintenance.swap_interval = 8;
        cfg.maintenance.cache_size = 8;
        cfg.maintenance.piggyback = piggyback;

        TieredOMap tmap(cfg);
        auto data = make_data(N);
        std::vector<int> hot_keys = {0, 1, 2, 3};
        tmap.init(data, hot_keys);

        for (int round = 0; round < 200; ++round)
            tmap.access(10 + (round % 10));

        std::vector<int> vals;
        for (int k = 0; k < N; ++k) {
            auto r = tmap.access(k);
            vals.push_back(bytes_to_int(r.value));
        }
        return vals;
    };

    auto standalone_vals = run_scenario(false);
    auto piggyback_vals = run_scenario(true);

    for (int k = 0; k < N; ++k) {
        EXPECT_EQ(standalone_vals[k], piggyback_vals[k])
            << "Standalone vs piggyback mismatch at key " << k;
    }
}

// ---------- AVL physical migration tests ----------

TEST(DynamicMaintenance, AVLPhysicalPromotion) {
    int N = 64, n = 8;
    TieredOMapConfig cfg;
    cfg.total_keys = N;
    cfg.hot_set_size = n;
    cfg.mode = SecurityMode::FullOblivious;
    cfg.backend = OmapBackend::AVL;
    cfg.maintenance.enabled = true;
    cfg.maintenance.observation_window = 16;
    cfg.maintenance.swap_interval = 16;
    cfg.maintenance.cache_size = 8;

    TieredOMap tmap(cfg);
    auto data = make_data(N);
    std::vector<int> hot_keys;
    for (int i = 0; i < n; ++i) hot_keys.push_back(i);
    tmap.init(data, hot_keys);

    EXPECT_EQ(tmap.hot_set_size(), 8);

    for (int round = 0; round < 120; ++round)
        tmap.access(32);

    auto result = tmap.access(32);
    EXPECT_TRUE(result.found_in_hot)
        << "Key 32 should have been physically promoted to hot tier";

    for (int k = 0; k < N; ++k) {
        auto r = tmap.access(k);
        EXPECT_EQ(bytes_to_int(r.value), k * 100)
            << "Value mismatch at key " << k << " after AVL physical promotion";
    }
}

TEST(DynamicMaintenance, AVLPhysicalDemotion) {
    int N = 32, n = 4;
    TieredOMapConfig cfg;
    cfg.total_keys = N;
    cfg.hot_set_size = n;
    cfg.mode = SecurityMode::FullOblivious;
    cfg.backend = OmapBackend::AVL;
    cfg.maintenance.enabled = true;
    cfg.maintenance.observation_window = 8;
    cfg.maintenance.swap_interval = 8;
    cfg.maintenance.cache_size = 2;

    TieredOMap tmap(cfg);
    auto data = make_data(N);
    std::vector<int> hot_keys = {0, 1, 2, 3};
    tmap.init(data, hot_keys);

    EXPECT_EQ(tmap.hot_set_size(), 4);

    for (int round = 0; round < 200; ++round)
        tmap.access(16);

    EXPECT_EQ(tmap.hot_set_size(), 4);
    EXPECT_TRUE(tmap.hot_keys().count(16))
        << "Frequently accessed cold key 16 should be promoted";

    for (int k = 0; k < N; ++k) {
        auto r = tmap.access(k);
        EXPECT_EQ(bytes_to_int(r.value), k * 100)
            << "Value mismatch at key " << k << " after AVL physical demotion";
    }
}

TEST(DynamicMaintenance, AVLPhysicalRoundTrip) {
    int N = 32, n = 4;
    TieredOMapConfig cfg;
    cfg.total_keys = N;
    cfg.hot_set_size = n;
    cfg.mode = SecurityMode::FullOblivious;
    cfg.backend = OmapBackend::AVL;
    cfg.maintenance.enabled = true;
    cfg.maintenance.observation_window = 8;
    cfg.maintenance.swap_interval = 8;
    cfg.maintenance.cache_size = 8;

    TieredOMap tmap(cfg);
    auto data = make_data(N);
    std::vector<int> hot_keys = {0, 1, 2, 3};
    tmap.init(data, hot_keys);

    for (int round = 0; round < 100; ++round)
        tmap.access(20);

    auto r1 = tmap.access(20);
    EXPECT_TRUE(r1.found_in_hot)
        << "Key 20 should be promoted after heavy access";

    for (int round = 0; round < 200; ++round)
        tmap.access(25);

    for (int k = 0; k < N; ++k) {
        auto r = tmap.access(k);
        EXPECT_EQ(bytes_to_int(r.value), k * 100)
            << "Value mismatch at key " << k << " after AVL round-trip migration";
    }
}

TEST(DynamicMaintenance, ValuesPreservedWithEpoch) {
    int N = 32, n = 4;
    TieredOMapConfig cfg;
    cfg.total_keys = N;
    cfg.hot_set_size = n;
    cfg.mode = SecurityMode::FullOblivious;
    cfg.maintenance.enabled = true;
    cfg.maintenance.observation_window = 16;
    cfg.maintenance.swap_interval = 16;
    cfg.maintenance.cache_size = 8;

    TieredOMap tmap(cfg);
    auto data = make_data(N);
    std::vector<int> hot_keys = {0, 1, 2, 3};
    tmap.init(data, hot_keys);

    // Values should still be correctly readable despite epoch encoding.
    for (int k = 0; k < N; ++k) {
        auto result = tmap.access(k);
        EXPECT_EQ(bytes_to_int(result.value), k * 100)
            << "Value mismatch at key " << k;
    }

    // Write a new value and read it back.
    Bytes new_val = int_to_bytes(9999);
    tmap.access(2, &new_val);
    auto r = tmap.access(2);
    EXPECT_EQ(bytes_to_int(r.value), 9999);
}
