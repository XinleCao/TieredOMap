#include <gtest/gtest.h>
#include "tiered_omap/omap/avl_omap.h"
#include <algorithm>
#include <numeric>

using namespace tiered_omap;

TEST(AVLOmap, InitAndSearch) {
    const int N = 32;
    std::vector<std::pair<int, Bytes>> data;
    for (int i = 0; i < N; ++i)
        data.emplace_back(i, int_to_bytes(i * 10));

    AVLOmap omap(N);
    omap.init(data);

    for (int i = 0; i < N; ++i) {
        Bytes val = omap.search(i);
        EXPECT_EQ(bytes_to_int(val), i * 10)
            << "Mismatch at key " << i;
    }
}

TEST(AVLOmap, SearchWithUpdate) {
    const int N = 16;
    std::vector<std::pair<int, Bytes>> data;
    for (int i = 0; i < N; ++i)
        data.emplace_back(i, int_to_bytes(i));

    AVLOmap omap(N);
    omap.init(data);

    for (int i = 0; i < N; ++i) {
        Bytes new_val = int_to_bytes(i + 500);
        omap.search(i, &new_val);
    }

    for (int i = 0; i < N; ++i) {
        Bytes val = omap.search(i);
        EXPECT_EQ(bytes_to_int(val), i + 500);
    }
}

TEST(AVLOmap, Insert) {
    AVLOmap omap(32);
    std::vector<std::pair<int, Bytes>> init_data;
    for (int i = 0; i < 8; ++i)
        init_data.emplace_back(i * 2, int_to_bytes(i * 2));

    omap.init(init_data);

    // Insert odd keys.
    for (int i = 0; i < 8; ++i)
        omap.insert(i * 2 + 1, int_to_bytes(i * 2 + 1));

    // Verify all keys.
    for (int i = 0; i < 16; ++i) {
        Bytes val = omap.search(i);
        EXPECT_EQ(bytes_to_int(val), i);
    }
}

TEST(AVLOmap, DummyAccess) {
    const int N = 16;
    std::vector<std::pair<int, Bytes>> data;
    for (int i = 0; i < N; ++i)
        data.emplace_back(i, int_to_bytes(i));

    AVLOmap omap(N);
    omap.init(data);

    for (int i = 0; i < 10; ++i)
        omap.dummy_access();

    for (int i = 0; i < N; ++i) {
        Bytes val = omap.search(i);
        EXPECT_EQ(bytes_to_int(val), i);
    }
}

TEST(AVLOmap, RepeatedSearches) {
    const int N = 32;
    std::vector<std::pair<int, Bytes>> data;
    for (int i = 0; i < N; ++i)
        data.emplace_back(i, int_to_bytes(i));

    AVLOmap omap(N);
    omap.init(data);

    for (int round = 0; round < 100; ++round) {
        int key = round % N;
        Bytes val = omap.search(key);
        EXPECT_EQ(bytes_to_int(val), key);
    }
}
