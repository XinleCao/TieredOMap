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
    EpochMeta m{5, 3};
    Bytes encoded = encode_with_epoch(val, m);
    auto [decoded_val, decoded_m] = decode_epoch(encoded);
    EXPECT_EQ(bytes_to_int(decoded_val), 42);
    EXPECT_EQ(decoded_m.cnt, 5);
    EXPECT_EQ(decoded_m.ep, 3);
}

TEST(DynamicMaintenance, MaintenanceManagerBasic) {
    MaintenanceConfig mcfg;
    mcfg.observation_window = 10;
    mcfg.swap_interval = 10;
    mcfg.promote_threshold = 3;
    mcfg.demote_threshold = 1;
    mcfg.staleness_windows = 2;
    mcfg.enabled = true;

    MaintenanceManager mgr(mcfg);

    EpochMeta stored{0, 0};
    bool should_promote = false;

    // Access a cold key multiple times within same epoch.
    for (int i = 0; i < 5; ++i) {
        stored = mgr.on_access(100, false, stored, should_promote);
    }
    EXPECT_EQ(stored.cnt, 5);
    EXPECT_EQ(stored.ep, 0);  // still within first observation window (5 < B_obs)

    // The key should have been promoted (prev_freq >= threshold on epoch change).
    // But we need to cross an epoch boundary for prev_freq to be set.
    // Let's fill the epoch.
    EpochMeta dummy{0, 0};
    for (int i = 0; i < 5; ++i) {
        dummy = mgr.on_access(200, true, dummy, should_promote);
    }
    // Now observation epoch should have advanced (10 accesses / B_obs = 1).
    EXPECT_EQ(mgr.obs_epoch(), 1);

    // Access key 100 again in new epoch.
    stored = mgr.on_access(100, false, stored, should_promote);
    // prev_freq should be 5 (from last epoch), which >= promote_threshold=3.
    EXPECT_TRUE(should_promote);
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
    cfg.maintenance.promote_threshold = 3;
    cfg.maintenance.demote_threshold = 1;
    cfg.maintenance.staleness_windows = 2;

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
    cfg.maintenance.promote_threshold = 100;
    cfg.maintenance.demote_threshold = 1;
    cfg.maintenance.staleness_windows = 2;

    TieredOMap tmap(cfg);
    auto data = make_data(N);
    std::vector<int> hot_keys = {0, 1, 2, 3};
    tmap.init(data, hot_keys);

    EXPECT_EQ(tmap.hot_set_size(), 4);

    // Only access cold keys for many epochs so hot keys become stale.
    for (int round = 0; round < 200; ++round) {
        tmap.access(16);
    }

    // At least some hot keys should have been demoted by the scan.
    EXPECT_LT(tmap.hot_set_size(), 4)
        << "Some stale hot keys should have been demoted";
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
    cfg.maintenance.promote_threshold = 3;
    cfg.maintenance.demote_threshold = 1;
    cfg.maintenance.staleness_windows = 2;

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
    cfg.maintenance.promote_threshold = 100;
    cfg.maintenance.demote_threshold = 1;
    cfg.maintenance.staleness_windows = 2;

    TieredOMap tmap(cfg);
    auto data = make_data(N);
    std::vector<int> hot_keys = {0, 1, 2, 3};
    tmap.init(data, hot_keys);

    EXPECT_EQ(tmap.hot_set_size(), 4);

    for (int round = 0; round < 200; ++round)
        tmap.access(16);

    EXPECT_LT(tmap.hot_set_size(), 4)
        << "Stale hot keys should have been physically demoted";

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
    cfg.maintenance.promote_threshold = 3;
    cfg.maintenance.demote_threshold = 1;
    cfg.maintenance.staleness_windows = 2;

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

TEST(Piggyback, BPlusDemotionZeroExtraRounds) {
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
    cfg.maintenance.promote_threshold = 100;
    cfg.maintenance.demote_threshold = 1;
    cfg.maintenance.staleness_windows = 2;
    cfg.maintenance.piggyback = true;

    TieredOMap tmap(cfg);
    auto data = make_data(N);
    std::vector<int> hot_keys = {0, 1, 2, 3};
    tmap.init(data, hot_keys);

    // Run normal accesses to collect baseline round count.
    auto r0 = tmap.access(0);
    int baseline_rounds = static_cast<int>(r0.total_bw.rounds);

    // Keep accessing cold keys to push epoch forward and trigger maintenance.
    // In piggyback mode, maintenance should not add extra rounds.
    for (int i = 0; i < 200; ++i) {
        int k = 10 + (i % 10);
        auto r = tmap.access(k);
        EXPECT_LE(static_cast<int>(r.total_bw.rounds), baseline_rounds)
            << "Piggyback access " << i << " exceeded baseline rounds ("
            << r.total_bw.rounds << " > " << baseline_rounds << ")";
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
    cfg.maintenance.promote_threshold = 100;
    cfg.maintenance.demote_threshold = 1;
    cfg.maintenance.staleness_windows = 2;
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
        cfg.maintenance.promote_threshold = 100;
        cfg.maintenance.demote_threshold = 1;
        cfg.maintenance.staleness_windows = 2;
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
    cfg.maintenance.promote_threshold = 3;
    cfg.maintenance.demote_threshold = 1;
    cfg.maintenance.staleness_windows = 2;

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
    cfg.maintenance.promote_threshold = 100;
    cfg.maintenance.demote_threshold = 1;
    cfg.maintenance.staleness_windows = 2;

    TieredOMap tmap(cfg);
    auto data = make_data(N);
    std::vector<int> hot_keys = {0, 1, 2, 3};
    tmap.init(data, hot_keys);

    EXPECT_EQ(tmap.hot_set_size(), 4);

    for (int round = 0; round < 200; ++round)
        tmap.access(16);

    EXPECT_LT(tmap.hot_set_size(), 4)
        << "Stale hot keys should have been physically demoted";

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
    cfg.maintenance.promote_threshold = 3;
    cfg.maintenance.demote_threshold = 1;
    cfg.maintenance.staleness_windows = 2;

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
    cfg.maintenance.promote_threshold = 100;
    cfg.maintenance.demote_threshold = 1;
    cfg.maintenance.staleness_windows = 100;

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
