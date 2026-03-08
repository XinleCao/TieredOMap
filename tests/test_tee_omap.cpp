#include "tiered_omap/tee/oblivious.h"
#include "tiered_omap/tee/packed_directory.h"
#include "tiered_omap/tee/enclave_oram.h"
#include "tiered_omap/tee/tee_avl_omap.h"
#include "tiered_omap/tee/tee_omap.h"
#include "tiered_omap/tee/tee_server.h"
#include <gtest/gtest.h>
#include <thread>
#include <unordered_set>

using namespace tiered_omap;
using namespace tiered_omap::tee;

// ── Oblivious primitives ────────────────────────────────────────────────────

TEST(Oblivious, CmovInt) {
    int a = 10, b = 20;
    o_mov_i(false, a, b);
    EXPECT_EQ(a, 10);
    o_mov_i(true, a, b);
    EXPECT_EQ(a, 20);
}

TEST(Oblivious, SwapInt) {
    int a = 1, b = 2;
    o_swap_i(false, a, b);
    EXPECT_EQ(a, 1);
    EXPECT_EQ(b, 2);
    o_swap_i(true, a, b);
    EXPECT_EQ(a, 2);
    EXPECT_EQ(b, 1);
}

TEST(Oblivious, CmovBytes) {
    Bytes a = {1, 2, 3, 4};
    Bytes b = {5, 6, 7, 8};
    o_mov_bytes(false, a, b);
    EXPECT_EQ(a, (Bytes{1, 2, 3, 4}));
    o_mov_bytes(true, a, b);
    EXPECT_EQ(a, (Bytes{5, 6, 7, 8}));
}

// ── Packed Directory ────────────────────────────────────────────────────────

TEST(PackedDirectory, InsertLookup) {
    PackedDirectory dir(16);

    dir.insert(10, 100);
    dir.insert(20, 200);
    dir.insert(30, 300);

    EXPECT_EQ(dir.lookup(10), 100);
    EXPECT_EQ(dir.lookup(20), 200);
    EXPECT_EQ(dir.lookup(30), 300);
    EXPECT_EQ(dir.lookup(99), INVALID_LEAF);
    EXPECT_EQ(dir.size(), 3);
}

TEST(PackedDirectory, UpdateAndRemove) {
    PackedDirectory dir(8);
    dir.insert(5, 50);
    dir.insert(7, 70);

    EXPECT_TRUE(dir.update_pos(5, 55));
    EXPECT_EQ(dir.lookup(5), 55);

    EXPECT_TRUE(dir.remove(5));
    EXPECT_EQ(dir.lookup(5), INVALID_LEAF);
    EXPECT_EQ(dir.size(), 1);
}

TEST(PackedDirectory, LookupAndUpdate) {
    PackedDirectory dir(8);
    dir.insert(42, 100);

    int old = dir.lookup_and_update(42, 999);
    EXPECT_EQ(old, 100);
    EXPECT_EQ(dir.lookup(42), 999);
}

TEST(PackedDirectory, Full) {
    PackedDirectory dir(4);
    EXPECT_TRUE(dir.insert(1, 10));
    EXPECT_TRUE(dir.insert(2, 20));
    EXPECT_TRUE(dir.insert(3, 30));
    EXPECT_TRUE(dir.insert(4, 40));
    EXPECT_FALSE(dir.insert(5, 50));
}

// ── Enclave ORAM ────────────────────────────────────────────────────────────

TEST(EnclaveOram, BasicAccessRW) {
    EnclaveOram oram(64, 32, 4);

    std::vector<std::pair<int, Bytes>> data;
    for (int i = 0; i < 64; ++i)
        data.push_back({i, int_to_bytes(i * 10)});
    oram.init(data);

    for (int i = 0; i < 64; ++i) {
        Bytes val = oram.access(i);
        EXPECT_EQ(bytes_to_int(val), i * 10);
    }

    Bytes new_val = int_to_bytes(999);
    oram.access(7, &new_val);
    Bytes check = oram.access(7);
    EXPECT_EQ(bytes_to_int(check), 999);
}

TEST(EnclaveOram, DummyAccess) {
    EnclaveOram oram(32, 16, 4);
    std::vector<std::pair<int, Bytes>> data;
    for (int i = 0; i < 32; ++i)
        data.push_back({i, int_to_bytes(i)});
    oram.init(data);

    for (int i = 0; i < 20; ++i)
        EXPECT_NO_THROW(oram.dummy_access());
}

TEST(EnclaveOram, PageTracking) {
    EnclaveOram oram(128, 64, 4);
    std::vector<std::pair<int, Bytes>> data;
    for (int i = 0; i < 128; ++i)
        data.push_back({i, Bytes(64, static_cast<uint8_t>(i))});
    oram.init(data);

    oram.access(0);
    EXPECT_GT(oram.last_stats().pages_touched, 0u);
}

// ── TEE AVL OMAP (cold tier) ────────────────────────────────────────────────

TEST(TeeAvlOmap, SearchInsertRemove) {
    TeeAvlOmap avl(128, 32, 4, 0);

    std::vector<std::pair<int, Bytes>> data;
    for (int i = 0; i < 64; ++i)
        data.push_back({i, int_to_bytes(i * 11)});
    avl.init(data);

    for (int i = 0; i < 64; ++i) {
        Bytes val = avl.search(i);
        EXPECT_EQ(bytes_to_int(val), i * 11) << "key=" << i;
    }

    // Insert new keys.
    for (int i = 64; i < 80; ++i)
        avl.insert(i, int_to_bytes(i * 11));

    // Verify all keys.
    for (int i = 0; i < 80; ++i)
        EXPECT_EQ(bytes_to_int(avl.search(i)), i * 11) << "key=" << i;

    // Remove some keys.
    for (int i = 0; i < 16; ++i)
        avl.remove(i);

    // Verify removed keys are gone and others remain.
    for (int i = 0; i < 16; ++i)
        EXPECT_TRUE(avl.search(i).empty()) << "key=" << i;
    for (int i = 16; i < 80; ++i)
        EXPECT_EQ(bytes_to_int(avl.search(i)), i * 11) << "key=" << i;
}

TEST(TeeAvlOmap, SplitOram) {
    int split_d = 3;
    TeeAvlOmap avl(128, 16, 4, split_d);

    std::vector<std::pair<int, Bytes>> data;
    for (int i = 0; i < 64; ++i)
        data.push_back({i, int_to_bytes(i)});
    avl.init(data);

    EXPECT_TRUE(avl.is_split());

    for (int i = 0; i < 64; ++i) {
        Bytes val = avl.search(i);
        EXPECT_EQ(bytes_to_int(val), i) << "key=" << i;
    }

    EXPECT_GE(avl.last_stats().lower_pages, 0u);
}

TEST(TeeAvlOmap, Update) {
    TeeAvlOmap avl(32, 16, 4, 0);

    std::vector<std::pair<int, Bytes>> data;
    for (int i = 0; i < 16; ++i)
        data.push_back({i, int_to_bytes(i)});
    avl.init(data);

    Bytes new_val = int_to_bytes(999);
    avl.search(7, &new_val);
    EXPECT_EQ(bytes_to_int(avl.search(7)), 999);
}

// ── TEE OMAP ────────────────────────────────────────────────────────────────

TEST(TeeOmap, HotColdAccess) {
    TeeOmapConfig cfg;
    cfg.total_keys = 128;
    cfg.hot_set_size = 16;
    cfg.value_size = 32;
    cfg.bucket_size = 4;
    cfg.mode = TeeSecurityMode::FullOblivious;
    cfg.use_split_oram = true;

    TeeOmap omap(cfg);

    std::vector<std::pair<int, Bytes>> data;
    for (int i = 0; i < 128; ++i)
        data.push_back({i, int_to_bytes(i * 100)});

    std::vector<int> hot_keys;
    for (int i = 0; i < 16; ++i)
        hot_keys.push_back(i);

    omap.init(data, hot_keys);

    // Hot key access.
    auto r1 = omap.access(5);
    EXPECT_TRUE(r1.found_in_hot);
    EXPECT_EQ(bytes_to_int(r1.value), 500);

    // Cold key access.
    auto r2 = omap.access(50);
    EXPECT_FALSE(r2.found_in_hot);
    EXPECT_EQ(bytes_to_int(r2.value), 5000);
}

TEST(TeeOmap, EarlyResponse) {
    TeeOmapConfig cfg;
    cfg.total_keys = 64;
    cfg.hot_set_size = 8;
    cfg.value_size = 16;
    cfg.mode = TeeSecurityMode::TierMembership;
    cfg.use_split_oram = true;

    TeeOmap omap(cfg);

    std::vector<std::pair<int, Bytes>> data;
    for (int i = 0; i < 64; ++i)
        data.push_back({i, int_to_bytes(i)});

    std::vector<int> hot_keys;
    for (int i = 0; i < 8; ++i)
        hot_keys.push_back(i);

    omap.init(data, hot_keys);

    bool got_early = false, got_final = false;
    auto result = omap.access(
        3, nullptr,
        [&](const Bytes& val, bool found) {
            got_early = true;
            EXPECT_TRUE(found);
            EXPECT_EQ(bytes_to_int(val), 3);
        },
        [&](const Bytes&, bool) {
            got_final = true;
        });

    EXPECT_TRUE(got_early);
    EXPECT_TRUE(got_final);
    EXPECT_TRUE(result.found_in_hot);
}

TEST(TeeOmap, PageMetrics) {
    TeeOmapConfig cfg;
    cfg.total_keys = 256;
    cfg.hot_set_size = 16;
    cfg.value_size = 64;
    cfg.mode = TeeSecurityMode::TierMembership;
    cfg.use_split_oram = true;

    TeeOmap omap(cfg);

    std::vector<std::pair<int, Bytes>> data;
    for (int i = 0; i < 256; ++i)
        data.push_back({i, int_to_bytes(i)});

    std::vector<int> hk;
    for (int i = 0; i < 16; ++i) hk.push_back(i);
    omap.init(data, hk);

    auto hot_r = omap.access(5);
    auto cold_r = omap.access(100);

    EXPECT_LT(hot_r.total_pages, cold_r.total_pages);
}

// ── TEE Maintenance ─────────────────────────────────────────────────────────

TEST(TeeOmap, MaintenanceConvergence) {
    // Setup: keys 0..7 are initially hot, keys 8..31 are cold.
    // Workload: repeatedly access cold keys 24..31 (promote candidates)
    // and never access hot keys 0..3 (demote candidates).
    // After enough epochs, keys 24..31 should be promoted and 0..3 demoted.

    TeeOmapConfig cfg;
    cfg.total_keys = 32;
    cfg.hot_set_size = 8;
    cfg.value_size = 16;
    cfg.mode = TeeSecurityMode::TierMembership;
    cfg.use_split_oram = false;
    cfg.maintenance.enabled = true;
    cfg.maintenance.epoch_length = 16;
    cfg.maintenance.promote_threshold = 3;
    cfg.maintenance.demote_threshold = 1;
    cfg.maintenance.staleness_epochs = 2;

    TeeOmap omap(cfg);

    std::vector<std::pair<int, Bytes>> data;
    for (int i = 0; i < 32; ++i)
        data.push_back({i, int_to_bytes(i * 10)});
    std::vector<int> hk = {0, 1, 2, 3, 4, 5, 6, 7};
    omap.init(data, hk);

    // Verify initial state.
    EXPECT_EQ(omap.hot_count(), 8);
    EXPECT_TRUE(omap.hot_keys().count(0));
    EXPECT_FALSE(omap.hot_keys().count(24));

    // Run workload: hit cold keys 24..27 heavily (4+ times/epoch to exceed
    // promote_threshold=3), hit hot keys 4..7 to keep them alive.
    // Never access hot keys 0..3 → they go stale and get demoted.
    for (int epoch = 0; epoch < 12; ++epoch) {
        for (int i = 0; i < cfg.maintenance.epoch_length; ++i) {
            int key;
            if (i % 4 < 3)
                key = 24 + (i / 4) % 4;   // cold keys 24..27 (3 out of 4 slots)
            else
                key = 4 + (i / 4) % 4;    // hot keys 4..7
            omap.access(key);
        }
    }

    // After 12 epochs, keys 0..3 should have been demoted (never accessed).
    // Keys 24..27 should have been promoted (heavily accessed).
    int promoted_count = 0;
    for (int k = 24; k < 28; ++k)
        if (omap.hot_keys().count(k)) ++promoted_count;

    int demoted_count = 0;
    for (int k = 0; k < 4; ++k)
        if (!omap.hot_keys().count(k)) ++demoted_count;

    EXPECT_GE(promoted_count, 2) << "Expected at least 2 of keys 24..27 promoted";
    EXPECT_GE(demoted_count, 2) << "Expected at least 2 of keys 0..3 demoted";

    // Verify correctness: all keys should still return correct values.
    for (int i = 0; i < 32; ++i) {
        auto r = omap.access(i);
        EXPECT_EQ(bytes_to_int(r.value), i * 10) << "key=" << i;
    }
}

TEST(TeeOmap, MaintenanceFullOblivious) {
    // Same test but in FullOblivious mode — extra dummy ops should not break things.
    TeeOmapConfig cfg;
    cfg.total_keys = 32;
    cfg.hot_set_size = 8;
    cfg.value_size = 16;
    cfg.mode = TeeSecurityMode::FullOblivious;
    cfg.use_split_oram = false;
    cfg.maintenance.enabled = true;
    cfg.maintenance.epoch_length = 16;
    cfg.maintenance.promote_threshold = 3;
    cfg.maintenance.demote_threshold = 1;
    cfg.maintenance.staleness_epochs = 2;

    TeeOmap omap(cfg);

    std::vector<std::pair<int, Bytes>> data;
    for (int i = 0; i < 32; ++i)
        data.push_back({i, int_to_bytes(i)});
    std::vector<int> hk = {0, 1, 2, 3, 4, 5, 6, 7};
    omap.init(data, hk);

    // Run workload.
    for (int epoch = 0; epoch < 8; ++epoch) {
        for (int i = 0; i < cfg.maintenance.epoch_length; ++i) {
            int key = (i % 2 == 0) ? (24 + i % 8) : (4 + i % 4);
            EXPECT_NO_THROW(omap.access(key));
        }
    }

    // Verify all keys are still accessible.
    for (int i = 0; i < 32; ++i) {
        auto r = omap.access(i);
        EXPECT_EQ(bytes_to_int(r.value), i) << "key=" << i;
    }
}

// ── TEE Server / Client (local loopback) ────────────────────────────────────

TEST(TeeServerClient, RoundTrip) {
    TeeOmapConfig cfg;
    cfg.total_keys = 32;
    cfg.hot_set_size = 8;
    cfg.value_size = 16;
    cfg.mode = TeeSecurityMode::FullOblivious;
    cfg.use_split_oram = false;

    const uint16_t PORT = 19877 + (std::rand() % 1000);

    std::vector<std::pair<int, Bytes>> data;
    for (int i = 0; i < 32; ++i)
        data.push_back({i, int_to_bytes(i * 7)});
    std::vector<int> hk = {0, 1, 2, 3, 4, 5, 6, 7};

    TeeServer server(cfg, PORT);
    server.init(data, hk);

    std::thread srv_thread([&]() { server.serve_one(); });

    // Give server time to start.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    {
        TeeClient client("127.0.0.1", PORT);

        // Read a hot key.
        auto [early1, final1] = client.read(3);
        EXPECT_EQ(early1.phase, 'H');
        EXPECT_TRUE(early1.found);
        EXPECT_EQ(bytes_to_int(early1.value), 21);
        EXPECT_EQ(final1.phase, 'F');

        // Read a cold key.
        auto [early2, final2] = client.read(20);
        EXPECT_EQ(early2.phase, 'H');
        EXPECT_FALSE(early2.found);
        EXPECT_EQ(final2.phase, 'F');
        EXPECT_TRUE(final2.found);
        EXPECT_EQ(bytes_to_int(final2.value), 140);
    }  // client destroyed here → server sees EOF → exits handle_connection

    srv_thread.join();
}
