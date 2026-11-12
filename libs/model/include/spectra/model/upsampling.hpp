// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "spectra/core/image.hpp"
#include "spectra/model/colour.hpp"
#include "spectra/model/params.hpp"

namespace spectra {

struct HanatosAdaptation {
    Vec window_params;   // erf4: (c_uv, sigma_uv, c_ir, sigma_ir); empty when absent
    Vec surface_params;  // [3 x 15]; empty when absent
    double spectral_gaussian_blur = 0.0;
    std::string reference_illuminant = "D55";
    bool apply_window = true;
    bool apply_surface = false;
};

// 2-D table [size x size x n_ch] indexed by "tc" coordinates.
struct TcLut {
    int size = 0, n_ch = 0;
    Vec v;
};

// Triangular <-> square chromaticity coordinates used by the LUT.
Xy tri2quad(const Xy& xy);
Xy quad2tri(const Xy& tc);

// compute_hanatos2025_tc_lut: spectra LUT x (sensitivity x window), surface
// correction and the input gamut compression baked in by remap-resample.
TcLut compute_tc_lut(const ColourData& cd, const Vec& sensitivity, int n_ch,
                     const HanatosAdaptation& adaptation, const InputGamutCompressSpec& gamut);

// Mitchell-Netravali bicubic sample of the table at tc in [0, 1]^2.
void sample_tc_lut(const TcLut& lut, double tc0, double tc1, double* out);

// rgb_to_raw_hanatos2025 / rgb_to_raw_mallett2019.
Image rgb_to_raw_hanatos2025(const ColourData& cd, const Image& rgb, const std::string& color_space,
                             bool apply_cctf_decoding, const std::string& reference_illuminant,
                             const TcLut& lut);
Image rgb_to_raw_mallett2019(const ColourData& cd, const Image& rgb, const Vec& sensitivity,
                             int n_ch, const std::string& color_space, bool apply_cctf_decoding,
                             const std::string& reference_illuminant);

}  // namespace spectra
