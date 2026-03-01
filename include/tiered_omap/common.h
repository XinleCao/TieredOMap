#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <random>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace tiered_omap {

using Bytes = std::vector<uint8_t>;

constexpr int INVALID_KEY = -1;
constexpr int INVALID_LEAF = -1;

struct Block {
    int key = INVALID_KEY;
    int leaf = INVALID_LEAF;
    Bytes value;

    bool is_dummy() const { return key == INVALID_KEY; }
};

struct BandwidthStats {
    uint64_t bytes_downloaded = 0;
    uint64_t bytes_uploaded = 0;
    uint64_t rounds = 0;

    uint64_t total_bytes() const { return bytes_downloaded + bytes_uploaded; }

    BandwidthStats& operator+=(const BandwidthStats& o) {
        bytes_downloaded += o.bytes_downloaded;
        bytes_uploaded += o.bytes_uploaded;
        rounds += o.rounds;
        return *this;
    }

    void reset() { bytes_downloaded = bytes_uploaded = rounds = 0; }
};

class SecureRandom {
public:
    static int rand_below(int n) {
        if (n <= 0) return 0;
        thread_local std::mt19937 rng(std::random_device{}());
        std::uniform_int_distribution<int> dist(0, n - 1);
        return dist(rng);
    }
};

inline Bytes int_to_bytes(int v) {
    Bytes b(sizeof(int));
    std::memcpy(b.data(), &v, sizeof(int));
    return b;
}

inline int bytes_to_int(const Bytes& b) {
    int v = 0;
    if (b.size() >= sizeof(int))
        std::memcpy(&v, b.data(), sizeof(int));
    return v;
}

inline Bytes pad_bytes(const Bytes& b, size_t target_size) {
    if (b.size() >= target_size) return b;
    Bytes padded = b;
    padded.resize(target_size, 0);
    return padded;
}

inline int ceil_log2(int n) {
    if (n <= 1) return 0;
    return static_cast<int>(std::ceil(std::log2(n)));
}

}  // namespace tiered_omap
