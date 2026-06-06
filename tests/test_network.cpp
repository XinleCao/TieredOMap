#include "tiered_omap/network/network_storage.h"
#include "tiered_omap/network/storage_server.h"
#include "tiered_omap/bench_setup.h"
#include "tiered_omap/omap/avl_omap.h"
#include "tiered_omap/omap/bplus_omap.h"
#include "tiered_omap/omap/da_ost_omap.h"
#include "tiered_omap/tiered_omap.h"
#include "tiered_omap/workload.h"
#include <gtest/gtest.h>
#include <thread>
#include <unordered_set>

using namespace tiered_omap;

static constexpr int TEST_PORT = 19876;

class NetworkTest : public ::testing::Test {
protected:
    void SetUp() override {
        server_ = std::make_unique<StorageServer>(TEST_PORT);
        server_thread_ = std::thread([this] { server_->run(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        channel_ = std::make_shared<TcpChannel>(
            TcpChannel::connect("localhost", TEST_PORT));
        creator_ = make_network_creator(channel_);
    }

    void TearDown() override {
        channel_->close();
        server_->stop();
        if (server_thread_.joinable()) {
            server_thread_.detach();
        }
    }

    std::unique_ptr<StorageServer> server_;
    std::thread server_thread_;
    std::shared_ptr<TcpChannel> channel_;
    StorageCreator creator_;
};

TEST_F(NetworkTest, PathORAM_BasicAccess) {
    PathORAM oram(32, 4, 7, creator_);
    std::unordered_map<int, Bytes> data;
    for (int i = 0; i < 32; ++i) data[i] = int_to_bytes(i * 10);
    oram.init(data);

    for (int i = 0; i < 32; ++i) {
        Bytes val = oram.access(i);
        EXPECT_EQ(bytes_to_int(val), i * 10);
    }
}

TEST_F(NetworkTest, AVLOmap_SearchInsertRemove) {
    AVLOmap omap(64, 4, creator_);
    std::vector<std::pair<int, Bytes>> data;
    for (int i = 0; i < 32; ++i) data.emplace_back(i, int_to_bytes(i));
    omap.init(data);

    for (int i = 0; i < 32; ++i) {
        Bytes val = omap.search(i);
        EXPECT_EQ(bytes_to_int(val), i);
    }

    omap.insert(100, int_to_bytes(999));
    EXPECT_EQ(bytes_to_int(omap.search(100)), 999);

    omap.remove(100);
    EXPECT_TRUE(omap.search(100).empty());
}

TEST_F(NetworkTest, BPlusOmap_SearchInsertRemove) {
    BPlusOmap omap(64, 8, 4, creator_);
    std::vector<std::pair<int, Bytes>> data;
    for (int i = 0; i < 32; ++i) data.emplace_back(i, int_to_bytes(i));
    omap.init(data);

    for (int i = 0; i < 32; ++i)
        EXPECT_EQ(bytes_to_int(omap.search(i)), i);

    omap.insert(200, int_to_bytes(777));
    EXPECT_EQ(bytes_to_int(omap.search(200)), 777);
}

TEST_F(NetworkTest, DaOstOmap_AVL) {
    DaOstOmap omap(64, OdsTreeType::AVL, 0, 4, 8, creator_);
    std::vector<std::pair<int, Bytes>> data;
    for (int i = 0; i < 32; ++i) data.emplace_back(i, int_to_bytes(i));
    omap.init(data);

    for (int i = 0; i < 32; ++i)
        EXPECT_EQ(bytes_to_int(omap.search(i)), i);
}

TEST_F(NetworkTest, TieredOMap_FullOblivious) {
    TieredOMapConfig cfg;
    cfg.total_keys = 64;
    cfg.hot_set_size = 16;
    cfg.mode = SecurityMode::FullOblivious;
    cfg.backend = OmapBackend::AVL;
    cfg.storage_creator = creator_;

    TieredOMap tm(cfg);
    std::vector<std::pair<int, Bytes>> data;
    for (int i = 0; i < 64; ++i) data.emplace_back(i, int_to_bytes(i));
    std::vector<int> hot_keys;
    for (int i = 0; i < 16; ++i) hot_keys.push_back(i);

    tm.init(data, hot_keys);

    for (int i = 0; i < 16; ++i) {
        auto r = tm.access(i);
        EXPECT_EQ(bytes_to_int(r.value), i);
        EXPECT_TRUE(r.found_in_hot);
    }

    auto r = tm.access(32);
    EXPECT_EQ(bytes_to_int(r.value), 32);
    EXPECT_FALSE(r.found_in_hot);
}

TEST_F(NetworkTest, TieredOMap_InterleavedAccess) {
    TieredOMapConfig cfg;
    cfg.total_keys = 64;
    cfg.hot_set_size = 16;
    cfg.mode = SecurityMode::FullOblivious;
    cfg.backend = OmapBackend::AVL;
    cfg.storage_creator = creator_;

    TieredOMap tm(cfg);
    std::vector<std::pair<int, Bytes>> data;
    for (int i = 0; i < 64; ++i) data.emplace_back(i, int_to_bytes(i));
    std::vector<int> hot_keys;
    for (int i = 0; i < 16; ++i) hot_keys.push_back(i);

    tm.init(data, hot_keys);
    tm.set_channel(channel_);

    // Hot key access
    for (int i = 0; i < 16; ++i) {
        auto r = tm.access(i);
        EXPECT_EQ(bytes_to_int(r.value), i);
        EXPECT_TRUE(r.found_in_hot);
    }

    // Cold key access
    for (int i = 16; i < 32; ++i) {
        auto r = tm.access(i);
        EXPECT_EQ(bytes_to_int(r.value), i);
        EXPECT_FALSE(r.found_in_hot);
    }

    // Verify interleaved rounds < sequential rounds
    auto rh = tm.access(0);
    auto hot_rounds = rh.hot_bw.rounds;
    auto cold_rounds = rh.cold_bw.rounds;
    auto total_rounds = rh.total_bw.rounds;
    // OMAP traversals overlap; data ORAM may add one or two rounds after refs
    // are known.
    EXPECT_GE(total_rounds, std::max(hot_rounds, cold_rounds));
    EXPECT_GT(total_rounds, std::max(hot_rounds, cold_rounds));
    EXPECT_LE(rh.rounds_to_answer, static_cast<int>(total_rounds));
    EXPECT_LE(total_rounds, std::max(hot_rounds, cold_rounds) + 2);
    EXPECT_LT(total_rounds, hot_rounds + cold_rounds);
}

TEST_F(NetworkTest, TieredOMap_DynamicInterleavedRoundAccounting) {
    TieredOMapConfig cfg;
    cfg.total_keys = 64;
    cfg.hot_set_size = 8;
    cfg.mode = SecurityMode::FullOblivious;
    cfg.backend = OmapBackend::BPlus;
    cfg.bplus_order = 4;
    cfg.storage_creator = creator_;
    cfg.maintenance.enabled = true;
    cfg.maintenance.observation_window = 1000;
    cfg.maintenance.swap_interval = 1000;
    cfg.maintenance.cache_size = 2;
    cfg.maintenance.piggyback = true;

    TieredOMap tm(cfg);
    std::vector<std::pair<int, Bytes>> data;
    for (int i = 0; i < 64; ++i) data.emplace_back(i, int_to_bytes(i));
    std::vector<int> hot_keys;
    for (int i = 0; i < 8; ++i) hot_keys.push_back(i);

    tm.init(data, hot_keys);
    tm.set_channel(channel_);

    auto hot = tm.access(3);
    EXPECT_TRUE(hot.found_in_hot);
    auto max_omap = std::max(hot.hot_bw.rounds, hot.cold_bw.rounds);
    EXPECT_GT(hot.total_bw.rounds, max_omap)
        << "data ORAM rounds must remain included in dynamic interleaved total";
    EXPECT_LT(hot.total_bw.rounds,
              hot.hot_bw.rounds + hot.cold_bw.rounds + 2);
    EXPECT_LE(hot.rounds_to_answer, static_cast<int>(hot.total_bw.rounds));
}

TEST_F(NetworkTest, TieredOMap_DynamicBPlusPromotionDeletesColdEntry) {
    TieredOMapConfig cfg;
    cfg.total_keys = 64;
    cfg.hot_set_size = 8;
    cfg.mode = SecurityMode::FullOblivious;
    cfg.backend = OmapBackend::BPlus;
    cfg.bplus_order = 4;
    cfg.storage_creator = creator_;
    cfg.maintenance.enabled = true;
    cfg.maintenance.observation_window = 2;
    cfg.maintenance.swap_interval = 64;
    cfg.maintenance.cache_size = 4;
    cfg.maintenance.piggyback = false;

    TieredOMap tm(cfg);
    std::vector<std::pair<int, Bytes>> data;
    for (int i = 0; i < 64; ++i) data.emplace_back(i, int_to_bytes(i));
    std::vector<int> hot_keys;
    for (int i = 0; i < 8; ++i) hot_keys.push_back(i);

    tm.init(data, hot_keys);
    tm.set_channel(channel_);

    auto r0 = tm.access(32);
    EXPECT_FALSE(r0.found_in_hot);
    EXPECT_EQ(bytes_to_int(r0.value), 32);

    auto r1 = tm.access(32);
    EXPECT_EQ(bytes_to_int(r1.value), 32);

    auto st = tm.dynamic_debug_state();
    EXPECT_EQ(st.swap_state, SwapState::ColdPend);
    EXPECT_TRUE(st.hot_keys.count(32));
    EXPECT_TRUE(st.cache_keys.count(32));
    EXPECT_TRUE(tm.cold_omap()->search(32).empty())
        << "B+ step promotion must delete the promoted key from cold OMAP";

    for (int k = 0; k < 64; ++k) {
        auto r = tm.access(k);
        EXPECT_EQ(bytes_to_int(r.value), k)
            << "value mismatch after B+ step promotion at key " << k;
    }
}

TEST_F(NetworkTest, TieredOMap_TierMembershipDaColdInterleaved) {
    TieredOMapConfig cfg;
    cfg.total_keys = 128;
    cfg.hot_set_size = 8;
    cfg.mode = SecurityMode::TierMembership;
    cfg.backend = OmapBackend::DaAvl;
    cfg.use_hot_backend = true;
    cfg.hot_backend = OmapBackend::BPlus;
    cfg.storage_creator = creator_;

    TieredOMap tm(cfg);
    std::vector<std::pair<int, Bytes>> data;
    for (int i = 0; i < 128; ++i) data.emplace_back(i, int_to_bytes(i));
    std::vector<int> hot_keys;
    for (int i = 0; i < 8; ++i) hot_keys.push_back(i);

    tm.init(data, hot_keys);
    tm.set_channel(channel_);

    for (int k = 0; k < 8; ++k) {
        auto r = tm.access(k);
        EXPECT_EQ(bytes_to_int(r.value), k);
        EXPECT_TRUE(r.found_in_hot);
    }

    Bytes updated = int_to_bytes(4242);
    auto w = tm.access(64, &updated);
    EXPECT_FALSE(w.found_in_hot);
    EXPECT_EQ(bytes_to_int(w.value), 64);

    auto r = tm.access(64);
    EXPECT_EQ(bytes_to_int(r.value), 4242);
    EXPECT_FALSE(r.found_in_hot);
}

TEST_F(NetworkTest, TieredOMap_DynamicPiggybackSinglePendingTransition) {
    TieredOMapConfig cfg;
    cfg.total_keys = 32;
    cfg.hot_set_size = 4;
    cfg.mode = SecurityMode::FullOblivious;
    cfg.backend = OmapBackend::AVL;
    cfg.storage_creator = creator_;
    cfg.maintenance.enabled = true;
    cfg.maintenance.piggyback = true;
    cfg.maintenance.observation_window = 2;
    cfg.maintenance.swap_interval = 4;
    cfg.maintenance.cache_size = 2;

    TieredOMap tm(cfg);
    std::vector<std::pair<int, Bytes>> data;
    for (int i = 0; i < 32; ++i) data.emplace_back(i, int_to_bytes(i));
    std::vector<int> hot_keys = {0, 1, 2, 3};

    tm.init(data, hot_keys);
    tm.set_channel(channel_);

    tm.access(0);
    tm.access(0);
    tm.access(16);
    auto r = tm.access(16);
    EXPECT_EQ(bytes_to_int(r.value), 16);

    auto st = tm.dynamic_debug_state();
    EXPECT_EQ(st.swap_state, SwapState::ColdPend);
    EXPECT_TRUE(st.hot_keys.count(16));
    EXPECT_FALSE(st.hot_keys.count(1));
    EXPECT_TRUE(st.phys_hot_keys.count(2))
        << "scan demotion must not also commit in the promotion access";
    EXPECT_TRUE(st.phys_hot_keys.count(3));
    EXPECT_FALSE(st.phys_hot_keys.count(1));
    EXPECT_TRUE(tm.hot_omap()->search(1).empty())
        << "piggyback demotion must delete the key inside the AVL OMAP";

    std::unordered_set<int> effective_hot = st.phys_hot_keys;
    effective_hot.insert(st.cache_keys.begin(), st.cache_keys.end());
    if (st.swap_state == SwapState::HotPend)
        effective_hot.insert(st.pending_insert_key);
    EXPECT_EQ(effective_hot, st.hot_keys);
}

TEST_F(NetworkTest, TieredOMap_DynamicPiggybackZipfWorkloadNoThrow) {
    TieredOMapConfig cfg;
    cfg.total_keys = 1024;
    cfg.hot_set_size = 64;
    cfg.mode = SecurityMode::TierMembership;
    cfg.backend = OmapBackend::BPlus;
    cfg.use_split_oram = true;
    cfg.use_hot_backend = true;
    cfg.hot_backend = OmapBackend::BPlus;
    cfg.bplus_order = 8;
    cfg.storage_creator = creator_;
    cfg.maintenance.enabled = true;
    cfg.maintenance.piggyback = true;
    cfg.maintenance.observation_window = 8;
    cfg.maintenance.swap_interval = 8;
    cfg.maintenance.cache_size = 8;

    TieredOMap tm(cfg);
    std::vector<std::pair<int, Bytes>> data;
    for (int i = 0; i < cfg.total_keys; ++i)
        data.emplace_back(i, int_to_bytes(i));
    std::vector<int> hot_keys;
    for (int i = 0; i < cfg.hot_set_size; ++i)
        hot_keys.push_back(i);

    tm.init(data, hot_keys);
    tm.set_channel(channel_);

    ZipfSampler z(cfg.total_keys, 1.0, 42);
    for (int i = 0; i < 80; ++i) {
        int key = z.sample();
        SCOPED_TRACE("op=" + std::to_string(i) +
                     " key=" + std::to_string(key));
        AccessResult r;
        try {
            r = tm.access(key);
        } catch (const std::exception& e) {
            FAIL() << "op=" << i << " key=" << key
                   << " exception=" << e.what();
        }
        EXPECT_EQ(bytes_to_int(r.value), key);
    }
}

TEST_F(NetworkTest, TieredOMap_DynamicPiggybackAvlLargeHotNoThrow) {
    TieredOMapConfig cfg;
    cfg.total_keys = 4096;
    cfg.hot_set_size = 1024;
    cfg.mode = SecurityMode::FullOblivious;
    cfg.backend = OmapBackend::AVL;
    cfg.use_split_oram = true;
    cfg.storage_creator = creator_;
    cfg.maintenance.enabled = true;
    cfg.maintenance.piggyback = true;
    cfg.maintenance.observation_window = 32;
    cfg.maintenance.swap_interval = 32;
    cfg.maintenance.cache_size = 4;

    TieredOMap tm(cfg);
    std::vector<std::pair<int, Bytes>> data;
    for (int i = 0; i < cfg.total_keys; ++i)
        data.emplace_back(i, int_to_bytes(i));
    std::vector<int> hot_keys;
    for (int i = 0; i < cfg.hot_set_size; ++i)
        hot_keys.push_back(i);

    tm.init(data, hot_keys);
    tm.set_channel(channel_);

    ZipfSampler warm(cfg.total_keys, 1.0, 42);
    for (int i = 0; i < 64; ++i) {
        int key = warm.sample();
        SCOPED_TRACE("warmup op=" + std::to_string(i) +
                     " key=" + std::to_string(key));
        AccessResult r;
        try {
            r = tm.access(key);
        } catch (const std::exception& e) {
            FAIL() << "warmup op=" << i << " key=" << key
                   << " exception=" << e.what();
        }
        EXPECT_EQ(bytes_to_int(r.value), key);
    }

    ZipfSampler z(cfg.total_keys, 1.0, 4242);
    for (int i = 0; i < 40; ++i) {
        int key = z.sample();
        SCOPED_TRACE("measure op=" + std::to_string(i) +
                     " key=" + std::to_string(key));
        AccessResult r;
        try {
            r = tm.access(key);
        } catch (const std::exception& e) {
            FAIL() << "measure op=" << i << " key=" << key
                   << " exception=" << e.what();
        }
        EXPECT_EQ(bytes_to_int(r.value), key);
    }
}

TEST_F(NetworkTest, SetupBenchTieredDynamicRestorePreservesEpochValues) {
    const int N = 128;
    const int n = 16;
    const int value_size = 32;

    Bytes payload;
    ser_int(payload, 1);  // tiered
    ser_int(payload, static_cast<int>(OmapBackend::DaBplus));
    ser_int(payload, N);
    ser_int(payload, n);
    ser_int(payload, 4);
    ser_int(payload, value_size);
    ser_int(payload, 0);  // FO
    ser_int(payload, 1);  // split
    ser_int(payload, 0);  // unused data_count
    ser_int(payload, static_cast<int>(OmapBackend::BPlus));
    ser_int(payload, 1);  // use hot backend
    ser_int(payload, 1);  // reserve index-entry metadata for dynamic maintenance

    channel_->send_msg(MsgType::SETUP_BENCH, payload);
    MsgType resp_type;
    Bytes resp;
    channel_->recv_msg(resp_type, resp);
    ASSERT_EQ(resp_type, MsgType::OK);

    const uint8_t* p = resp.data();
    auto tm = bench_setup::restore_tiered(p, channel_);
    ASSERT_NE(tm, nullptr);
    EXPECT_EQ(tm->config().backend, OmapBackend::DaBplus);
    EXPECT_EQ(tm->config().effective_hot_backend(), OmapBackend::BPlus);

    MaintenanceConfig mc;
    mc.enabled = true;
    mc.observation_window = 8;
    mc.swap_interval = 6;
    mc.cache_size = 4;
    mc.piggyback = true;
    tm->enable_maintenance(mc);

    auto hot_cache = tm->access(3);
    EXPECT_TRUE(hot_cache.found_in_hot);
    EXPECT_EQ(bytes_to_int(hot_cache.value), 3);
    EXPECT_EQ(hot_cache.value.size(), static_cast<size_t>(value_size))
        << "server SETUP_BENCH must reserve epoch metadata before "
        << "client-side dynamic maintenance is enabled";

    auto cold = tm->access(64);
    EXPECT_FALSE(cold.found_in_hot);
    EXPECT_EQ(bytes_to_int(cold.value), 64);
    EXPECT_EQ(cold.value.size(), static_cast<size_t>(value_size));

    ZipfSampler z(N, 1.0, 42);
    for (int i = 0; i < 40; ++i) {
        int key = z.sample();
        auto r = tm->access(key);
        EXPECT_EQ(bytes_to_int(r.value), key);
        EXPECT_EQ(r.value.size(), static_cast<size_t>(value_size))
            << "op=" << i << " key=" << key;
    }
}
