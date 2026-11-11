// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "spectra/core/image.hpp"
#include "spectra/core/math.hpp"

namespace spectra {

// Port of spektrafilm.utils.fast_gaussian_filter: per-channel 2-D Gaussian.
// sigma < 3 uses the separable FIR (kernel radius int(truncate*sigma+0.5),
// scipy 'reflect' boundary); sigma >= 3 uses the Young-van Vliet recursive
// filter with sample-replication boundaries. sigma <= 0 copies the input.
void gaussian_filter_plane(const double* in, double* out, int h, int w, double sigma,
                           double truncate = 3.0);
Image gaussian_filter(const Image& img, const Vec& sigma_per_channel, double truncate = 3.0);
Image gaussian_filter(const Image& img, double sigma, double truncate = 3.0);

// fast_exponential_filter: 3-Gaussian surrogate of a 2-D exponential PSF.
Image exponential_filter(const Image& img, const Vec& decay_per_channel);

// scipy.ndimage.gaussian_filter1d along one axis of a strided 1-D signal.
// mode: 0 = 'reflect' (d c b a | a b c d | d c b a), 1 = 'mirror' (d c b | a b c d | c b a).
void scipy_gaussian_filter1d(const double* in, double* out, int n, int stride, double sigma,
                             int mode, double truncate = 4.0);

Image unsharp_mask(const Image& img, double sigma, double amount);
// apply_multiplicative_unsharp_mask. `channel_sums`, when given, replaces the
// per-channel image sums used for the mass renormalisation (in/out pairs).
Image multiplicative_unsharp_mask(const Image& img, double sigma, double amount);

Vec match_channels(const Vec3& values, int n_ch);

}  // namespace spectra
