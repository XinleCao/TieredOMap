#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

namespace tiered_omap {
namespace tee {

// Oblivious conditional move: dst = cond ? src : dst
// On x86 the compiler emits CMOV; the inline‐asm fallback guarantees it.
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
    // Portable branchless fallback (compiler may still branch; acceptable
    // for simulation / correctness testing on ARM / non‐SGX).
    auto mask = -static_cast<std::make_signed_t<T>>(cond);
    dst = (src & mask) | (dst & ~mask);
#endif
}

// Specialisation for int (most common case in our ORAM code).
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

// Oblivious conditional swap: if cond, swap a and b.
inline void o_swap_i(bool cond, int& a, int& b) {
    int diff = a ^ b;
    int mask = -static_cast<int>(cond);
    diff &= mask;
    a ^= diff;
    b ^= diff;
}

// Oblivious byte‐array conditional move (for Bytes / value buffers).
// Both dst and src must have the same size.
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

}  // namespace tee
}  // namespace tiered_omap
