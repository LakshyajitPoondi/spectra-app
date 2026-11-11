// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <stdexcept>
#include <vector>

namespace spectra {

// Interleaved H x W x C image of doubles. The CPU reference pipeline works in
// float64 throughout, like the Python reference.
struct Image {
    int h = 0, w = 0, c = 0;
    std::vector<double> v;

    Image() = default;
    Image(int h_, int w_, int c_, double fill = 0.0)
        : h(h_), w(w_), c(c_), v(static_cast<size_t>(h_) * w_ * c_, fill) {}

    size_t pixels() const { return static_cast<size_t>(h) * w; }
    size_t size() const { return v.size(); }
    double& at(int y, int x, int k) { return v[(static_cast<size_t>(y) * w + x) * c + k]; }
    double at(int y, int x, int k) const { return v[(static_cast<size_t>(y) * w + x) * c + k]; }
    double* px(int y, int x) { return &v[(static_cast<size_t>(y) * w + x) * c]; }
    const double* px(int y, int x) const { return &v[(static_cast<size_t>(y) * w + x) * c]; }
    bool same_shape(const Image& o) const { return h == o.h && w == o.w && c == o.c; }
};

// Single-channel plane helpers.
std::vector<double> extract_channel(const Image& img, int k);
void insert_channel(Image& img, int k, const std::vector<double>& plane);

}  // namespace spectra
