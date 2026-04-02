#include <gtest/gtest.h>
#include "tiered_omap/oram/da_oram.h"

using namespace tiered_omap;

TEST(DAOram, InitAndAccess) {
    const int N = 64;
    DAOram oram(N);

    std::unordered_map<int, Bytes> data;
    for (int i = 0; i < N; ++i)
        data[i] = int_to_bytes(i * 100);

    oram.init(data);

    for (int i = 0; i < N; ++i) {
        Bytes val = oram.access(i);
        EXPECT_EQ(bytes_to_int(val), i * 100);
    }
}

TEST(DAOram, WriteAndReadBack) {
    const int N = 64;
    DAOram oram(N);

    std::unordered_map<int, Bytes> data;
    for (int i = 0; i < N; ++i)
        data[i] = int_to_bytes(i);

    oram.init(data);

    for (int i = 0; i < N; ++i) {
        Bytes new_val = int_to_bytes(i + 2000);
        oram.access(i, &new_val);
    }

    for (int i = 0; i < N; ++i) {
        Bytes val = oram.access(i);
        EXPECT_EQ(bytes_to_int(val), i + 2000);
    }
}

TEST(DAOram, RepeatedAccess) {
    const int N = 128;
    DAOram oram(N);

    std::unordered_map<int, Bytes> data;
    for (int i = 0; i < N; ++i)
        data[i] = int_to_bytes(i);

    oram.init(data);

    for (int round = 0; round < 200; ++round) {
        int key = round % N;
        Bytes val = oram.access(key);
        EXPECT_EQ(bytes_to_int(val), key);
    }
}

TEST(DAOram, DummyAccess) {
    const int N = 64;
    DAOram oram(N);

    std::unordered_map<int, Bytes> data;
    for (int i = 0; i < N; ++i)
        data[i] = int_to_bytes(i);

    oram.init(data);

    for (int i = 0; i < 20; ++i)
        oram.dummy_access();

    for (int i = 0; i < N; ++i) {
        Bytes val = oram.access(i);
        EXPECT_EQ(bytes_to_int(val), i);
    }
}

TEST(DAOram, ICOverflowAndReset) {
    // Small ic_max to trigger overflow quickly.
    const int N = 64;
    DAOram oram(N, 4, 14, 4, 4);

    std::unordered_map<int, Bytes> data;
    for (int i = 0; i < N; ++i)
        data[i] = int_to_bytes(i * 10);

    oram.init(data);

    // Access key 0 many times to trigger multiple overflows.
    for (int round = 0; round < 20; ++round) {
        Bytes val = oram.access(0);
        EXPECT_EQ(bytes_to_int(val), 0);
    }

    // Verify all other keys still accessible.
    for (int i = 1; i < N; ++i) {
        Bytes val = oram.access(i);
        EXPECT_EQ(bytes_to_int(val), i * 10);
    }
}
