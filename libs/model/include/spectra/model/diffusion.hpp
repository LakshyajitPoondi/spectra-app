// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <optional>

#include "spectra/core/image.hpp"
#include "spectra/model/params.hpp"

namespace spectra {

// boost_highlights: reconstructs pre-clip irradiance above a protected level.
// `max_raw_override` replaces the global maximum of the image (whole-image
// statistic) when a preview or tile must match the full render.
void boost_highlights(Image& raw, double boost_ev, double boost_range, double protect_ev,
                      const std::optional<double>& max_raw_override = std::nullopt,
                      double midgray = 0.184);

// apply_halation_um: in-emulsion scatter then back-reflection halation.
void apply_halation_um(Image& raw, const HalationParams& h, double pixel_size_um);

// apply_gaussian_blur_um
Image apply_gaussian_blur_um(const Image& img, double sigma_um, double pixel_size_um);

// Strength -> deflected fraction p_s (0 when the filter does nothing).
double diffusion_strength_to_scatter(double strength, const std::string& family);

// Per-channel PSF [size x size x 3], each channel sum-normalised.
Image diffusion_filter_psf(int size, const DiffusionFilterParams& p, double pixel_size_um);

// apply_diffusion_filter_um. With clamp_radius_to_image (the Python behaviour)
// the kernel radius is capped at half the short image side.
Image apply_diffusion_filter_um(const Image& img, const DiffusionFilterParams& p,
                                double pixel_size_um, bool clamp_radius_to_image = true);

// 'same'-size convolution of one plane with a (2r+1)^2 kernel using numpy
// 'reflect' padding (no edge repeat), via real FFT.
void fft_convolve_reflect(const double* in, double* out, int h, int w, const double* kernel,
                          int radius);

}  // namespace spectra
