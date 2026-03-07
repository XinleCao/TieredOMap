#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

namespace tiered_omap {

class ZipfSampler {
public:
    ZipfSampler(int n, double alpha, uint64_t seed = 42) : rng_(seed) {
        cdf_.resize(n);
        double h = 0;
        for (int i = 1; i <= n; ++i) h += 1.0 / std::pow(i, alpha);
        double cum = 0;
        for (int i = 0; i < n; ++i) {
            cum += (1.0 / std::pow(i + 1, alpha)) / h;
            cdf_[i] = cum;
        }
    }

    int sample() {
        double u = std::uniform_real_distribution<>(0, 1)(rng_);
        return static_cast<int>(
            std::lower_bound(cdf_.begin(), cdf_.end(), u) - cdf_.begin());
    }

private:
    std::mt19937_64 rng_;
    std::vector<double> cdf_;
};

class ShiftingZipfSampler {
public:
    ShiftingZipfSampler(int n, double alpha, int shift_offset, uint64_t seed = 42)
        : n_(n), shift_(shift_offset), sampler_(n, alpha, seed) {}

    int sample_phase1() { return sampler_.sample(); }
    int sample_phase2() { return (sampler_.sample() + shift_) % n_; }

private:
    int n_;
    int shift_;
    ZipfSampler sampler_;
};

inline double hot_hit_ratio(int N, int n, double alpha) {
    double h = 0;
    for (int i = 1; i <= N; ++i) h += 1.0 / std::pow(i, alpha);
    double top = 0;
    for (int i = 1; i <= n; ++i) top += 1.0 / std::pow(i, alpha);
    return top / h;
}

}  // namespace tiered_omap
