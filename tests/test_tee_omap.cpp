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
    auto leaves = oram.init(data);

    for (int i = 0; i < 64; ++i) {
        int nl = oram.random_leaf();
        Bytes val = oram.access(i, leaves[i], nl);
        EXPECT_EQ(bytes_to_int(val), i * 10);
        leaves[i] = nl;
    }

    int nl = oram.random_leaf();
    Bytes new_val = int_to_bytes(999);
    oram.access(7, leaves[7], nl, &new_val);
    leaves[7] = nl;

    nl = oram.random_leaf();
    Bytes check = oram.access(7, leaves[7], nl);
    leaves[7] = nl;
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
    auto leaves = oram.init(data);

    int nl = oram.random_leaf();
    oram.access(0, leaves[0], nl);
    leaves[0] = nl;
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

TEST(TeeAvlOmap, SearchThenRemove) {
    TeeAvlOmap avl(32, 16, 4, 0);

    std::vector<std::pair<int, Bytes>> data;
    for (int i = 0; i < 24; ++i)
        data.push_back({i, int_to_bytes(i * 10)});
    avl.init(data);

    // Mimic promote: search then remove
    Bytes val = avl.search(12);
    EXPECT_EQ(bytes_to_int(val), 120);
    avl.remove(12);

    // Verify all other keys still accessible.
    for (int i = 0; i < 24; ++i) {
        Bytes r = avl.search(i);
        if (i == 12)
            EXPECT_TRUE(r.empty()) << "key=" << i;
        else
            EXPECT_EQ(bytes_to_int(r), i * 10) << "key=" << i;
    }
}

TEST(TeeAvlOmap, RemoveKey5) {
    TeeAvlOmap avl(32, 16, 4, 0);

    std::vector<std::pair<int, Bytes>> data;
    for (int i = 0; i < 24; ++i)
        data.push_back({i, int_to_bytes(i * 10)});
    avl.init(data);

    // Single remove without prior search.
    avl.remove(5);

    for (int i = 0; i < 24; ++i) {
        Bytes r = avl.search(i);
        if (i == 5)
            EXPECT_TRUE(r.empty()) << "key=" << i;
        else
            EXPECT_EQ(bytes_to_int(r), i * 10) << "key=" << i;
    }
}

TEST(TeeAvlOmap, SearchRemoveKey5) {
    TeeAvlOmap avl(32, 16, 4, 0);

    std::vector<std::pair<int, Bytes>> data;
    for (int i = 0; i < 24; ++i)
        data.push_back({i, int_to_bytes(i * 10)});
    avl.init(data);

    // Search then remove (like promote does).
    Bytes v = avl.search(5);
    EXPECT_EQ(bytes_to_int(v), 50);
    avl.remove(5);

    for (int i = 0; i < 24; ++i) {
        Bytes r = avl.search(i);
        if (i == 5)
            EXPECT_TRUE(r.empty()) << "key=" << i;
        else
            EXPECT_EQ(bytes_to_int(r), i * 10) << "key=" << i;
    }
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

    // Both accesses should touch pages (doubly-oblivious: dummy ops
    // touch the same pages as real ops, so hot ≈ cold).
    EXPECT_GT(hot_r.total_pages, 0u);
    EXPECT_GT(cold_r.total_pages, 0u);
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
    EXPECT_TRUE(omap.is_hot(0));
    EXPECT_FALSE(omap.is_hot(24));

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
        if (omap.is_hot(k)) ++promoted_count;

    int demoted_count = 0;
    for (int k = 0; k < 4; ++k)
        if (!omap.is_hot(k)) ++demoted_count;

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

// ── Double Obliviousness Verification ────────────────────────────────────────
// Verify that different keys produce the same page-touch count, confirming
// the ORAM access pattern doesn't leak which key was accessed.

TEST(DoubleOblivious, EnclaveOramAccessPattern) {
    EnclaveOram oram(64, 32, 4);

    std::vector<std::pair<int, Bytes>> data;
    for (int i = 0; i < 64; ++i)
        data.push_back({i, int_to_bytes(i * 10)});
    auto leaves = oram.init(data);

    int nl = oram.random_leaf();
    oram.access(0, leaves[0], nl);
    leaves[0] = nl;
    uint64_t na_key0 = oram.last_stats().node_accesses;

    nl = oram.random_leaf();
    oram.access(31, leaves[31], nl);
    leaves[31] = nl;
    uint64_t na_key31 = oram.last_stats().node_accesses;

    nl = oram.random_leaf();
    oram.access(63, leaves[63], nl);
    leaves[63] = nl;
    uint64_t na_key63 = oram.last_stats().node_accesses;

    oram.dummy_access();
    uint64_t na_dummy = oram.last_stats().node_accesses;

    EXPECT_EQ(na_key0, na_key31)
        << "Different keys should have identical node access count";
    EXPECT_EQ(na_key31, na_key63);
    EXPECT_EQ(na_key63, na_dummy)
        << "Dummy access should have identical node access count as real access";
    EXPECT_GT(na_key0, 0u);
}

TEST(DoubleOblivious, TeeAvlSearchPattern) {
    TeeAvlOmap avl(64, 16, 4, 0);

    std::vector<std::pair<int, Bytes>> data;
    for (int i = 0; i < 32; ++i)
        data.push_back({i, int_to_bytes(i)});
    avl.init(data);

    // Real search for existing key.
    avl.search(5);
    auto stats_real = avl.last_stats();

    // Real search for different existing key.
    avl.search(28);
    auto stats_other = avl.last_stats();

    // Dummy search.
    avl.dummy_access();
    auto stats_dummy = avl.last_stats();

    EXPECT_EQ(stats_real.total_node_accesses(), stats_other.total_node_accesses())
        << "search(5) and search(28) should have identical node access counts";
    EXPECT_EQ(stats_real.total_node_accesses(), stats_dummy.total_node_accesses())
        << "Real and dummy search should have identical node access counts";
    EXPECT_GT(stats_real.total_node_accesses(), 0u);
}

TEST(DoubleOblivious, TeeOmapFOPattern) {
    TeeOmapConfig cfg;
    cfg.total_keys = 64;
    cfg.hot_set_size = 8;
    cfg.value_size = 16;
    cfg.mode = TeeSecurityMode::FullOblivious;
    cfg.use_split_oram = false;

    TeeOmap omap(cfg);

    std::vector<std::pair<int, Bytes>> data;
    for (int i = 0; i < 64; ++i)
        data.push_back({i, int_to_bytes(i)});
    std::vector<int> hk = {0, 1, 2, 3, 4, 5, 6, 7};
    omap.init(data, hk);

    // In FO mode, hot and cold accesses should have identical page counts.
    auto hot_r = omap.access(3);   // hot key
    auto cold_r = omap.access(50); // cold key

    EXPECT_EQ(hot_r.total_pages, cold_r.total_pages)
        << "FO mode: hot and cold access should have identical page counts";

    // Verify correctness is maintained.
    EXPECT_EQ(bytes_to_int(hot_r.value), 3);
    EXPECT_EQ(bytes_to_int(cold_r.value), 50);
}

TEST(DoubleOblivious, SearchOrDummyEquivalence) {
    TeeAvlOmap avl(32, 16, 4, 0);

    std::vector<std::pair<int, Bytes>> data;
    for (int i = 0; i < 16; ++i)
        data.push_back({i, int_to_bytes(i * 100)});
    avl.init(data);

    // search_or_dummy(real=true) should find the key.
    Bytes r1 = avl.search_or_dummy(7, true);
    EXPECT_EQ(bytes_to_int(r1), 700);

    // search_or_dummy(real=false) returns fixed-size zeros (oblivious).
    Bytes r2 = avl.search_or_dummy(7, false);
    EXPECT_EQ(r2.size(), 16u);
    EXPECT_EQ(r2, Bytes(16, 0));

    // Both should have identical page counts.
    avl.search_or_dummy(7, true);
    auto stats_real2 = avl.last_stats();
    avl.search_or_dummy(7, false);
    auto stats_dummy2 = avl.last_stats();
    EXPECT_EQ(stats_real2.total_node_accesses(), stats_dummy2.total_node_accesses());
}

TEST(DoubleOblivious, AccessOrDummyEquivalence) {
    EnclaveOram oram(32, 16, 4);

    std::vector<std::pair<int, Bytes>> data;
    for (int i = 0; i < 32; ++i)
        data.push_back({i, int_to_bytes(i)});
    auto leaves = oram.init(data);

    int nl = oram.random_leaf();
    Bytes r1 = oram.access_or_dummy(10, leaves[10], nl, true);
    leaves[10] = nl;
    EXPECT_EQ(bytes_to_int(r1), 10);

    int rl = oram.random_leaf();
    Bytes r2 = oram.access_or_dummy(10, rl, rl, false);
    EXPECT_EQ(bytes_to_int(r2), 0);

    nl = oram.random_leaf();
    oram.access_or_dummy(10, leaves[10], nl, true);
    leaves[10] = nl;
    uint64_t pages_real = oram.last_stats().pages_touched;

    rl = oram.random_leaf();
    oram.access_or_dummy(10, rl, rl, false);
    uint64_t pages_dummy = oram.last_stats().pages_touched;
    EXPECT_EQ(pages_real, pages_dummy);
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
