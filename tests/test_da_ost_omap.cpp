#include <gtest/gtest.h>
#include "tiered_omap/omap/da_ost_omap.h"

using namespace tiered_omap;

// ═══════════════════════════════════════════════════════════════════════════
// DAORAM + AVL
// ═══════════════════════════════════════════════════════════════════════════

TEST(DaAvlOmap, InitAndSearch) {
    const int N = 64;
    DaOstOmap omap(N, OdsTreeType::AVL);

    std::vector<std::pair<int, Bytes>> data;
    for (int i = 0; i < N; ++i)
        data.emplace_back(i, int_to_bytes(i * 100));
    omap.init(data);

    for (int i = 0; i < N; ++i)
        EXPECT_EQ(bytes_to_int(omap.search(i)), i * 100);
}

TEST(DaAvlOmap, SearchUpdate) {
    const int N = 32;
    DaOstOmap omap(N, OdsTreeType::AVL);

    std::vector<std::pair<int, Bytes>> data;
    for (int i = 0; i < N; ++i)
        data.emplace_back(i, int_to_bytes(i));
    omap.init(data);

    for (int i = 0; i < N; ++i) {
        Bytes new_val = int_to_bytes(i + 5000);
        omap.search(i, &new_val);
    }

    for (int i = 0; i < N; ++i)
        EXPECT_EQ(bytes_to_int(omap.search(i)), i + 5000);
}

TEST(DaAvlOmap, InsertNewKeys) {
    const int N = 64;
    DaOstOmap omap(N * 2, OdsTreeType::AVL, N * 2);

    std::vector<std::pair<int, Bytes>> data;
    for (int i = 0; i < N; ++i)
        data.emplace_back(i, int_to_bytes(i));
    omap.init(data);

    for (int i = N; i < N + 10; ++i)
        omap.insert(i, int_to_bytes(i * 10));

    for (int i = 0; i < N; ++i)
        EXPECT_EQ(bytes_to_int(omap.search(i)), i);

    for (int i = N; i < N + 10; ++i)
        EXPECT_EQ(bytes_to_int(omap.search(i)), i * 10);
}

TEST(DaAvlOmap, RemoveAndSearch) {
    const int N = 64;
    DaOstOmap omap(N, OdsTreeType::AVL);

    std::vector<std::pair<int, Bytes>> data;
    for (int i = 0; i < N; ++i)
        data.emplace_back(i, int_to_bytes(i));
    omap.init(data);

    omap.remove(0);
    omap.remove(N / 2);
    omap.remove(N - 1);

    for (int i = 0; i < N; ++i) {
        if (i == 0 || i == N / 2 || i == N - 1) continue;
        EXPECT_EQ(bytes_to_int(omap.search(i)), i);
    }
}

TEST(DaAvlOmap, DummyAccess) {
    const int N = 32;
    DaOstOmap omap(N, OdsTreeType::AVL);

    std::vector<std::pair<int, Bytes>> data;
    for (int i = 0; i < N; ++i)
        data.emplace_back(i, int_to_bytes(i));
    omap.init(data);

    for (int i = 0; i < 10; ++i)
        omap.dummy_access();

    for (int i = 0; i < N; ++i)
        EXPECT_EQ(bytes_to_int(omap.search(i)), i);
}

TEST(DaAvlOmap, OperationTypeHiding) {
    const int N = 64;
    DaOstOmap omap(N * 2, OdsTreeType::AVL, N * 2);

    std::vector<std::pair<int, Bytes>> data;
    for (int i = 0; i < N; ++i)
        data.emplace_back(i, int_to_bytes(i));
    omap.init(data);

    omap.search(0);
    uint64_t search_rounds = omap.last_stats().rounds;

    omap.insert(N + 1, int_to_bytes(999));
    uint64_t insert_rounds = omap.last_stats().rounds;

    omap.remove(1);
    uint64_t remove_rounds = omap.last_stats().rounds;

    omap.dummy_access();
    uint64_t dummy_rounds = omap.last_stats().rounds;

    EXPECT_EQ(search_rounds, insert_rounds);
    EXPECT_EQ(search_rounds, remove_rounds);
    EXPECT_EQ(search_rounds, dummy_rounds);
}

// ═══════════════════════════════════════════════════════════════════════════
// DAORAM + B+ tree
// ═══════════════════════════════════════════════════════════════════════════

TEST(DaBplusOmap, InitAndSearch) {
    const int N = 64;
    DaOstOmap omap(N, OdsTreeType::BPlus);

    std::vector<std::pair<int, Bytes>> data;
    for (int i = 0; i < N; ++i)
        data.emplace_back(i, int_to_bytes(i * 100));
    omap.init(data);

    for (int i = 0; i < N; ++i)
        EXPECT_EQ(bytes_to_int(omap.search(i)), i * 100);
}

TEST(DaBplusOmap, SearchUpdate) {
    const int N = 32;
    DaOstOmap omap(N, OdsTreeType::BPlus);

    std::vector<std::pair<int, Bytes>> data;
    for (int i = 0; i < N; ++i)
        data.emplace_back(i, int_to_bytes(i));
    omap.init(data);

    for (int i = 0; i < N; ++i) {
        Bytes new_val = int_to_bytes(i + 5000);
        omap.search(i, &new_val);
    }

    for (int i = 0; i < N; ++i)
        EXPECT_EQ(bytes_to_int(omap.search(i)), i + 5000);
}

TEST(DaBplusOmap, InsertNewKeys) {
    const int N = 64;
    DaOstOmap omap(N * 2, OdsTreeType::BPlus, N * 2);

    std::vector<std::pair<int, Bytes>> data;
    for (int i = 0; i < N; ++i)
        data.emplace_back(i, int_to_bytes(i));
    omap.init(data);

    for (int i = N; i < N + 10; ++i)
        omap.insert(i, int_to_bytes(i * 10));

    for (int i = 0; i < N; ++i)
        EXPECT_EQ(bytes_to_int(omap.search(i)), i);

    for (int i = N; i < N + 10; ++i)
        EXPECT_EQ(bytes_to_int(omap.search(i)), i * 10);
}

TEST(DaBplusOmap, RemoveAndSearch) {
    const int N = 64;
    DaOstOmap omap(N, OdsTreeType::BPlus);

    std::vector<std::pair<int, Bytes>> data;
    for (int i = 0; i < N; ++i)
        data.emplace_back(i, int_to_bytes(i));
    omap.init(data);

    omap.remove(0);
    omap.remove(N / 2);
    omap.remove(N - 1);

    for (int i = 0; i < N; ++i) {
        if (i == 0 || i == N / 2 || i == N - 1) continue;
        EXPECT_EQ(bytes_to_int(omap.search(i)), i);
    }
}

TEST(DaBplusOmap, DummyAccess) {
    const int N = 32;
    DaOstOmap omap(N, OdsTreeType::BPlus);

    std::vector<std::pair<int, Bytes>> data;
    for (int i = 0; i < N; ++i)
        data.emplace_back(i, int_to_bytes(i));
    omap.init(data);

    for (int i = 0; i < 10; ++i)
        omap.dummy_access();

    for (int i = 0; i < N; ++i)
        EXPECT_EQ(bytes_to_int(omap.search(i)), i);
}

TEST(DaBplusOmap, OperationTypeHiding) {
    const int N = 64;
    DaOstOmap omap(N * 2, OdsTreeType::BPlus, N * 2);

    std::vector<std::pair<int, Bytes>> data;
    for (int i = 0; i < N; ++i)
        data.emplace_back(i, int_to_bytes(i));
    omap.init(data);

    omap.search(0);
    uint64_t search_rounds = omap.last_stats().rounds;

    omap.insert(N + 1, int_to_bytes(999));
    uint64_t insert_rounds = omap.last_stats().rounds;

    omap.remove(1);
    uint64_t remove_rounds = omap.last_stats().rounds;

    omap.dummy_access();
    uint64_t dummy_rounds = omap.last_stats().rounds;

    EXPECT_EQ(search_rounds, insert_rounds);
    EXPECT_EQ(search_rounds, remove_rounds);
    EXPECT_EQ(search_rounds, dummy_rounds);
}
