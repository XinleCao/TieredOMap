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
