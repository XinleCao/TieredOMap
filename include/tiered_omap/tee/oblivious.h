#pragma once

#include <cstdint>
#include <cstring>
#include <vector>
#include <algorithm>

namespace tiered_omap {
namespace tee {

// ── Oblivious conditional move: dst = cond ? src : dst ──────────────────────

template <typename T>
inline void o_mov(bool cond, T& dst, const T& src) {
    static_assert(std::is_trivially_copyable_v<T>);
#if defined(__x86_64__) && !defined(OBLIVIOUS_NO_ASM)
    T tmp = src;
    asm volatile(
        "test %1, %1\n\t"
        "cmovnz %2, %0"
        : "+r"(dst)
        : "r"(static_cast<int>(cond)), "r"(tmp)
        : "cc");
#else
    auto mask = -static_cast<std::make_signed_t<T>>(cond);
    dst = (src & mask) | (dst & ~mask);
#endif
}

inline void o_mov_i(bool cond, int& dst, int src) {
#if defined(__x86_64__) && !defined(OBLIVIOUS_NO_ASM)
    asm volatile(
        "test %1, %1\n\t"
        "cmovnz %2, %0"
        : "+r"(dst)
        : "r"(static_cast<int>(cond)), "r"(src)
        : "cc");
#else
    int mask = -static_cast<int>(cond);
    dst = (src & mask) | (dst & ~mask);
#endif
}

inline void o_mov_bool(bool cond, bool& dst, bool src) {
    int d = dst ? 1 : 0;
    int s = src ? 1 : 0;
    o_mov_i(cond, d, s);
    dst = (d != 0);
}

// ── Oblivious conditional swap ──────────────────────────────────────────────

inline void o_swap_i(bool cond, int& a, int& b) {
    int diff = a ^ b;
    int mask = -static_cast<int>(cond);
    diff &= mask;
    a ^= diff;
    b ^= diff;
}

// ── Oblivious byte-array conditional move ───────────────────────────────────

inline void o_mov_bytes(bool cond, uint8_t* dst, const uint8_t* src,
                        size_t len) {
    uint8_t mask = -static_cast<uint8_t>(cond);
    for (size_t i = 0; i < len; ++i)
        dst[i] = (src[i] & mask) | (dst[i] & ~mask);
}

inline void o_mov_bytes(bool cond, std::vector<uint8_t>& dst,
                        const std::vector<uint8_t>& src) {
    if (dst.size() != src.size()) dst.resize(src.size());
    o_mov_bytes(cond, dst.data(), src.data(), dst.size());
}

// ── Oblivious select: returns cond ? a : b without branching ────────────────

inline int o_select_i(bool cond, int a, int b) {
    int result = b;
    o_mov_i(cond, result, a);
    return result;
}

// ── Oblivious min / max ─────────────────────────────────────────────────────

inline int o_max(int a, int b) {
    return o_select_i(static_cast<unsigned>(a - b) >> 31, b, a);
}

inline int o_min(int a, int b) {
    return o_select_i(static_cast<unsigned>(a - b) >> 31, a, b);
}

// ── Oblivious constant-time equality ────────────────────────────────────────
// Returns 1 if a == b, 0 otherwise. No branch.

inline int o_equal(int a, int b) {
    unsigned diff = static_cast<unsigned>(a ^ b);
    // If diff == 0, all bits zero → (diff | -diff) has sign bit 0 → result 1.
    // Otherwise sign bit 1 → result 0.
    return 1 - static_cast<int>((diff | (0u - diff)) >> 31);
}

inline int o_less(int a, int b) {
    // Branchless a < b for non-overflowing int range (our ORAM keys).
    return static_cast<int>(static_cast<unsigned>(a - b) >> 31);
}

// ── Oblivious compaction ────────────────────────────────────────────────────
// Moves all elements where is_valid(elem)==true to the front,
// invalid ones to the back. O(n^2) but oblivious (no data-dependent branch).
// After compaction, resize the vector to keep only valid_count elements.

template <typename T, typename F>
inline int o_compact(std::vector<T>& arr, F is_valid) {
    int n = static_cast<int>(arr.size());
    int valid_count = 0;
    // Count valid elements obliviously.
    for (int i = 0; i < n; ++i) {
        int v = is_valid(arr[i]) ? 1 : 0;
        valid_count += v;
    }
    // Oblivious sort: bubble valid elements to front.
    // Each pass, for adjacent pairs, swap if left is invalid and right is valid.
    for (int pass = 0; pass < n; ++pass) {
        for (int i = 0; i < n - 1; ++i) {
            bool left_inv = !is_valid(arr[i]);
            bool right_val = is_valid(arr[i + 1]);
            bool do_swap = left_inv && right_val;
            // Swap all fields obliviously.
            o_swap_element(do_swap, arr[i], arr[i + 1]);
        }
    }
    return valid_count;
}

// ── Oblivious byte-array swap ───────────────────────────────────────────────

inline void o_swap_bytes(bool cond, uint8_t* a, uint8_t* b, size_t len) {
    uint8_t mask = -static_cast<uint8_t>(cond);
    for (size_t i = 0; i < len; ++i) {
        uint8_t diff = (a[i] ^ b[i]) & mask;
        a[i] ^= diff;
        b[i] ^= diff;
    }
}

inline void o_swap_bytes(bool cond, std::vector<uint8_t>& a,
                         std::vector<uint8_t>& b) {
    size_t len = std::max(a.size(), b.size());
    a.resize(len, 0);
    b.resize(len, 0);
    o_swap_bytes(cond, a.data(), b.data(), len);
}

}  // namespace tee
}  // namespace tiered_omap
