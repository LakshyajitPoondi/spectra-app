// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <functional>
#include <string>

#include "spectra/core/image.hpp"
#include "spectra/core/math.hpp"

namespace spectra {

// measure_raw_autoexposure_ev: EV that brings the metered region of the film
// raw to `target` (1 = mid-grey). Methods: average, median, center_weighted,
// partial, matrix, multi_zone, highlight_weighted. Unknown methods give 0 EV.
double measure_raw_autoexposure_ev(const Image& raw, const std::string& method, double target = 1.0);

// Metering preview of the full frame: skimage.transform.rescale(order=0) to a
// long edge of max_size (Gaussian anti-aliasing + nearest sampling).
Image metering_preview(const Image& img, int max_size = 256);

// skimage.transform.rescale(image, factor, order=3): cubic B-spline resampling
// with mirror boundaries (Gaussian anti-aliasing first when reducing), clipped
// to the input range. Used for io.upscale_factor.
Image rescale_cubic(const Image& img, double factor);

// spektrafilm.utils.crop_resize.crop_image
Image crop_image(const Image& img, const std::array<double, 2>& center,
                 const std::array<double, 2>& size);

// Area-average (box) reduction so the long edge is at most `long_edge` pixels.
// Used to build previews; returns the input when it is already small enough.
Image downsample_area(const Image& img, int long_edge);

// 3-D LUT acceleration of a per-pixel spectral function (utils/lut.py).
// fn maps n_in input values to n_out output values.
using PixelFn = std::function<void(const double* in, double* out)>;

struct Lut3D {
    int steps = 0, n_out = 0;
    Vec3 xmin{}, xmax{};
    Vec v;  // [steps^3 x n_out]
};
Lut3D create_lut_3d(const PixelFn& fn, int n_out, const Vec& xmin, const Vec& xmax, int steps);
// Tetrahedral interpolation (apply_lut_3d default).
Image apply_lut_3d(const Lut3D& lut, const Image& data);
// compute_with_lut_1d: single-channel input, 1024-node table, linear interpolation.
Image compute_with_lut_1d(const Image& data, const PixelFn& fn, int n_out, double xmin, double xmax,
                          int steps = 1024);

}  // namespace spectra
