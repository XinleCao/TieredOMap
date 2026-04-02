#include <gtest/gtest.h>
#include "tiered_omap/oram/path_oram.h"

using namespace tiered_omap;

TEST(PathORAM, InitAndAccess) {
    const int N = 64;
    PathORAM oram(N, 4);

    std::unordered_map<int, Bytes> data;
    for (int i = 0; i < N; ++i)
        data[i] = int_to_bytes(i * 100);

    oram.init(data);

    for (int i = 0; i < N; ++i) {
        Bytes val = oram.access(i);
        EXPECT_EQ(bytes_to_int(val), i * 100);
    }
}

TEST(PathORAM, WriteAndReadBack) {
    const int N = 32;
    PathORAM oram(N, 4);

    std::unordered_map<int, Bytes> data;
    for (int i = 0; i < N; ++i)
        data[i] = int_to_bytes(i);

    oram.init(data);

    for (int i = 0; i < N; ++i) {
        Bytes new_val = int_to_bytes(i + 1000);
        oram.access(i, &new_val);
    }

    for (int i = 0; i < N; ++i) {
        Bytes val = oram.access(i);
        EXPECT_EQ(bytes_to_int(val), i + 1000);
    }
}

TEST(PathORAM, MultipleAccessesSameKey) {
    const int N = 16;
    PathORAM oram(N, 4);

    std::unordered_map<int, Bytes> data;
    for (int i = 0; i < N; ++i)
        data[i] = int_to_bytes(i);

    oram.init(data);

    for (int round = 0; round < 50; ++round) {
        int key = round % N;
        Bytes val = oram.access(key);
        EXPECT_EQ(bytes_to_int(val), key);
    }
}

TEST(PathORAM, DummyAccess) {
    const int N = 16;
    PathORAM oram(N, 4);

    std::unordered_map<int, Bytes> data;
    for (int i = 0; i < N; ++i)
        data[i] = int_to_bytes(i);

    oram.init(data);

    // Dummy accesses should not corrupt state.
    for (int i = 0; i < 20; ++i)
        oram.dummy_access();

    for (int i = 0; i < N; ++i) {
        Bytes val = oram.access(i);
        EXPECT_EQ(bytes_to_int(val), i);
    }
}

TEST(PathORAM, LargerScale) {
    const int N = 256;
    PathORAM oram(N, 4);

    std::unordered_map<int, Bytes> data;
    for (int i = 0; i < N; ++i)
        data[i] = int_to_bytes(i * 7);

    oram.init(data);

    for (int i = 0; i < N; ++i) {
        Bytes val = oram.access(i);
        EXPECT_EQ(bytes_to_int(val), i * 7);
    }
}
