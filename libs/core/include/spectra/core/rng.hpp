// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <array>
#include <cmath>
#include <cstdint>

namespace spectra {

// Philox4x32-10 counter-based generator (Salmon et al. 2011). Stateless: the
// output depends only on (key, counter), so results do not depend on tiling,
// threading or evaluation order, and the same function can be written in GLSL
// with 32-bit integer ops (umulExtended) for the GPU backend.
inline std::array<uint32_t, 4> philox4x32(std::array<uint32_t, 4> ctr, std::array<uint32_t, 2> key) {
    constexpr uint32_t M0 = 0xD2511F53u, M1 = 0xCD9E8D57u, W0 = 0x9E3779B9u, W1 = 0xBB67AE85u;
    for (int r = 0; r < 10; ++r) {
        uint64_t p0 = static_cast<uint64_t>(M0) * ctr[0];
        uint64_t p1 = static_cast<uint64_t>(M1) * ctr[2];
        uint32_t hi0 = static_cast<uint32_t>(p0 >> 32), lo0 = static_cast<uint32_t>(p0);
        uint32_t hi1 = static_cast<uint32_t>(p1 >> 32), lo1 = static_cast<uint32_t>(p1);
        ctr = {hi1 ^ ctr[1] ^ key[0], lo1, hi0 ^ ctr[3] ^ key[1], lo0};
        key[0] += W0;
        key[1] += W1;
    }
    return ctr;
}

// A per-pixel random stream: key = (seed, stream id), counter = (x, y, draw, 0).
// Each draw yields four 32-bit words; uniform() hands them out one at a time.
class PixelRng {
public:
    PixelRng(uint64_t seed, uint32_t stream, uint32_t x, uint32_t y)
        : key_{static_cast<uint32_t>(seed), static_cast<uint32_t>(seed >> 32) ^ (stream * 0x9E3779B9u)},
          x_(x), y_(y), stream_(stream) {}

    uint32_t next_u32() {
        if (have_ == 0) {
            buf_ = philox4x32({x_, y_, draw_++, stream_}, key_);
            have_ = 4;
        }
        return buf_[4 - have_--];
    }
    // Uniform in (0, 1), never exactly 0 or 1. 24 bits of resolution, so the
    // value is exact in fp32 and the GPU kernels draw the very same numbers.
    double uniform() { return (static_cast<double>(next_u32() >> 8) + 0.5) * (1.0 / 16777216.0); }
    // Standard normal (Box-Muller; uses two uniforms, returns one value).
    double normal() {
        double u1 = uniform(), u2 = uniform();
        return std::sqrt(-2.0 * std::log(u1)) * std::cos(6.283185307179586 * u2);
    }

private:
    std::array<uint32_t, 2> key_;
    uint32_t x_, y_, stream_;
    uint32_t draw_ = 0;
    std::array<uint32_t, 4> buf_{};
    int have_ = 0;
};

}  // namespace spectra
