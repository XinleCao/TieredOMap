#include <gtest/gtest.h>
#include "tiered_omap/omap/avl_omap.h"
#include <algorithm>
#include <numeric>
#include <set>

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

TEST(AVLOmap, RemoveLeaf) {
    const int N = 16;
    std::vector<std::pair<int, Bytes>> data;
    for (int i = 0; i < N; ++i)
        data.emplace_back(i, int_to_bytes(i * 100));

    AVLOmap omap(N * 2);
    omap.init(data);

    omap.remove(0);
    omap.remove(N - 1);

    for (int i = 1; i < N - 1; ++i) {
        Bytes val = omap.search(i);
        EXPECT_EQ(bytes_to_int(val), i * 100)
            << "Key " << i << " damaged after leaf remove";
    }
}

TEST(AVLOmap, RemoveTwoChildren) {
    const int N = 32;
    std::vector<std::pair<int, Bytes>> data;
    for (int i = 0; i < N; ++i)
        data.emplace_back(i, int_to_bytes(i * 10));

    AVLOmap omap(N * 2);
    omap.init(data);

    std::set<int> removed;
    for (int k : {15, 7, 23, 3, 11, 19, 27}) {
        omap.remove(k);
        removed.insert(k);

        for (int i = 0; i < N; ++i) {
            if (removed.count(i)) continue;
            Bytes val = omap.search(i);
            EXPECT_EQ(bytes_to_int(val), i * 10)
                << "Key " << i << " damaged after removing " << k;
        }
    }
}

TEST(AVLOmap, InsertAfterRemove) {
    const int N = 16;
    std::vector<std::pair<int, Bytes>> data;
    for (int i = 0; i < N; ++i)
        data.emplace_back(i, int_to_bytes(i));

    AVLOmap omap(N * 2);
    omap.init(data);

    for (int i = 0; i < N; i += 2)
        omap.remove(i);

    for (int i = 0; i < N; i += 2)
        omap.insert(i, int_to_bytes(i + 1000));

    for (int i = 0; i < N; ++i) {
        Bytes val = omap.search(i);
        int expected = (i % 2 == 0) ? i + 1000 : i;
        EXPECT_EQ(bytes_to_int(val), expected);
    }
}

TEST(AVLOmap, OperationTypeHiding) {
    const int N = 16;
    std::vector<std::pair<int, Bytes>> data;
    for (int i = 0; i < N; ++i)
        data.emplace_back(i, int_to_bytes(i));

    AVLOmap omap(N * 2);
    omap.init(data);

    omap.search(0);
    uint64_t search_rounds = omap.last_stats().rounds;

    omap.insert(N, int_to_bytes(N));
    uint64_t insert_rounds = omap.last_stats().rounds;

    omap.remove(N);
    uint64_t remove_rounds = omap.last_stats().rounds;

    omap.dummy_access();
    uint64_t dummy_rounds = omap.last_stats().rounds;

    EXPECT_EQ(search_rounds, insert_rounds)
        << "search=" << search_rounds << " insert=" << insert_rounds;
    EXPECT_EQ(search_rounds, remove_rounds)
        << "search=" << search_rounds << " remove=" << remove_rounds;
    EXPECT_EQ(search_rounds, dummy_rounds)
        << "search=" << search_rounds << " dummy=" << dummy_rounds;
}

TEST(AVLOmap, RemoveAllKeys) {
    const int N = 16;
    std::vector<std::pair<int, Bytes>> data;
    for (int i = 0; i < N; ++i)
        data.emplace_back(i, int_to_bytes(i));

    AVLOmap omap(N * 2);
    omap.init(data);

    for (int i = 0; i < N; ++i)
        omap.remove(i);

    omap.insert(42, int_to_bytes(4200));
    Bytes val = omap.search(42);
    EXPECT_EQ(bytes_to_int(val), 4200);
}
