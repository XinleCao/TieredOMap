#include "tiered_omap/network/network_storage.h"
#include "tiered_omap/network/storage_server.h"
#include "tiered_omap/omap/avl_omap.h"
#include "tiered_omap/omap/bplus_omap.h"
#include "tiered_omap/omap/da_ost_omap.h"
#include "tiered_omap/tiered_omap.h"
#include <gtest/gtest.h>
#include <thread>

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
