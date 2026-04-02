#include <gtest/gtest.h>
#include "tiered_omap/omap/bplus_omap.h"

using namespace tiered_omap;

TEST(BPlusOmap, InitAndSearch) {
    const int N = 32;
    std::vector<std::pair<int, Bytes>> data;
    for (int i = 0; i < N; ++i)
        data.emplace_back(i, int_to_bytes(i * 10));

    BPlusOmap omap(N, 4);
    omap.init(data);

    for (int i = 0; i < N; ++i) {
        Bytes val = omap.search(i);
        EXPECT_EQ(bytes_to_int(val), i * 10)
            << "Mismatch at key " << i;
    }
}

TEST(BPlusOmap, SearchWithUpdate) {
    const int N = 16;
    std::vector<std::pair<int, Bytes>> data;
    for (int i = 0; i < N; ++i)
        data.emplace_back(i, int_to_bytes(i));

    BPlusOmap omap(N, 4);
    omap.init(data);

    for (int i = 0; i < N; ++i) {
        Bytes nv = int_to_bytes(i + 500);
        omap.search(i, &nv);
    }

    for (int i = 0; i < N; ++i) {
        Bytes val = omap.search(i);
        EXPECT_EQ(bytes_to_int(val), i + 500);
    }
}

TEST(BPlusOmap, LargerData) {
    const int N = 128;
    std::vector<std::pair<int, Bytes>> data;
    for (int i = 0; i < N; ++i)
        data.emplace_back(i, int_to_bytes(i * 7));

    BPlusOmap omap(N, 8);
    omap.init(data);

    for (int i = 0; i < N; ++i) {
        Bytes val = omap.search(i);
        EXPECT_EQ(bytes_to_int(val), i * 7);
    }
}

TEST(BPlusOmap, RepeatedSearches) {
    const int N = 32;
    std::vector<std::pair<int, Bytes>> data;
    for (int i = 0; i < N; ++i)
        data.emplace_back(i, int_to_bytes(i));

    BPlusOmap omap(N, 4);
    omap.init(data);

    for (int round = 0; round < 50; ++round) {
        int key = round % N;
        Bytes val = omap.search(key);
        EXPECT_EQ(bytes_to_int(val), key);
    }
}

TEST(BPlusOmap, DummyAccess) {
    const int N = 16;
    std::vector<std::pair<int, Bytes>> data;
    for (int i = 0; i < N; ++i)
        data.emplace_back(i, int_to_bytes(i));

    BPlusOmap omap(N, 4);
    omap.init(data);

    for (int i = 0; i < 10; ++i)
        omap.dummy_access();

    for (int i = 0; i < N; ++i) {
        Bytes val = omap.search(i);
        EXPECT_EQ(bytes_to_int(val), i);
    }
}

TEST(BPlusOmap, RemoveBasic) {
    const int N = 32;
    std::vector<std::pair<int, Bytes>> data;
    for (int i = 0; i < N; ++i)
        data.emplace_back(i, int_to_bytes(i * 10));

    BPlusOmap omap(N, 4);
    omap.init(data);

    omap.remove(10);
    omap.remove(20);

    for (int i = 0; i < N; ++i) {
        Bytes val = omap.search(i);
        if (i == 10 || i == 20)
            EXPECT_TRUE(val.empty()) << "Key " << i << " should be removed";
        else
            EXPECT_EQ(bytes_to_int(val), i * 10) << "Key " << i << " mismatch";
    }
}

TEST(BPlusOmap, RemoveMultiple) {
    const int N = 64;
    std::vector<std::pair<int, Bytes>> data;
    for (int i = 0; i < N; ++i)
        data.emplace_back(i, int_to_bytes(i));

    BPlusOmap omap(N, 8);
    omap.init(data);

    for (int i = 0; i < N; i += 3)
        omap.remove(i);

    for (int i = 0; i < N; ++i) {
        Bytes val = omap.search(i);
        if (i % 3 == 0)
            EXPECT_TRUE(val.empty()) << "Key " << i << " should be removed";
        else
            EXPECT_EQ(bytes_to_int(val), i) << "Key " << i << " mismatch";
    }
}

TEST(BPlusOmap, RemoveThenInsert) {
    const int N = 32;
    std::vector<std::pair<int, Bytes>> data;
    for (int i = 0; i < N; ++i)
        data.emplace_back(i, int_to_bytes(i * 100));

    BPlusOmap omap(N + 8, 4);
    omap.init(data);

    omap.remove(5);
    omap.remove(15);

    Bytes v5 = omap.search(5);
    EXPECT_TRUE(v5.empty());

    omap.insert(5, int_to_bytes(555));
    Bytes v5b = omap.search(5);
    EXPECT_EQ(bytes_to_int(v5b), 555);
}

TEST(BPlusOmap, OperationTypeHiding) {
    const int N = 32;
    std::vector<std::pair<int, Bytes>> data;
    for (int i = 0; i < N; ++i)
        data.emplace_back(i, int_to_bytes(i));

    BPlusOmap omap(N + 8, 4);
    omap.init(data);

    omap.reset_stats();
    omap.search(5);
    auto search_bw = omap.last_stats();

    omap.reset_stats();
    omap.insert(100, int_to_bytes(100));
    auto insert_bw = omap.last_stats();

    omap.reset_stats();
    omap.remove(10);
    auto remove_bw = omap.last_stats();

    omap.reset_stats();
    omap.dummy_access();
    auto dummy_bw = omap.last_stats();

    EXPECT_EQ(search_bw.rounds, insert_bw.rounds)
        << "search and insert should have identical round count";
    EXPECT_EQ(search_bw.rounds, remove_bw.rounds)
        << "search and remove should have identical round count";
    EXPECT_EQ(search_bw.rounds, dummy_bw.rounds)
        << "search and dummy should have identical round count";
}

TEST(BPlusOmap, InsertThenSearchSplitVictims) {
    std::vector<std::pair<int, Bytes>> data;
    for (int i = 8; i < 64; ++i)
        data.emplace_back(i, int_to_bytes(i * 100));

    BPlusOmap omap(64, 4, 4);
    omap.init(data);

    for (auto& [k, v] : data) {
        auto r = omap.search(k);
        ASSERT_EQ(bytes_to_int(r), k * 100) << "Pre-insert check failed at " << k;
    }

    omap.insert(0, int_to_bytes(0));

    auto r9 = omap.search(9);
    EXPECT_EQ(bytes_to_int(r9), 900) << "Key 9 lost after insert(0)";
    auto r10 = omap.search(10);
    EXPECT_EQ(bytes_to_int(r10), 1000) << "Key 10 lost after insert(0)";

    for (int k = 8; k < 64; ++k) {
        auto r = omap.search(k);
        EXPECT_EQ(bytes_to_int(r), k * 100) << "Post-insert check failed at " << k;
    }
}
