// SPDX-License-Identifier: GPL-3.0-or-later
// "Extended" features: push / pull development, bleach bypass and HDR output.
// These are not in the spektrafilm reference. The models are adapted from
// spektrafilm-ofx (Copyright (c) Aedan Diez, GPL-3.0): shaders/vulkan/
// SpektraCurveDevelop.comp, SpektraPrintScan.comp, SpektraScannerPost.comp and
// src/SpektraVulkanRenderer.cpp.
#pragma once
#include "spectra/core/math.hpp"
#include "spectra/model/params.hpp"

namespace spectra::extended {

// ---- Push / pull --------------------------------------------------------------
// Curve gamma factor of a film development pushed / pulled by `stops`
// (ECN-2 times: pull 1 = 150 s, normal = 180 s, push 1 = 220 s, push 2 = 280 s).
double film_gamma(double stops);
// Print development: gamma = 2^(stops / 4).
double print_gamma(double stops);
// Experimental mode: log-exposure actually looked up in the film curves, and
// the density gain applied to the result. `n_ch` is 3 (colour) or 1 (B&W).
void experimental_log_raw(const double* log_raw, int n_ch, double stops, double* out);
double experimental_density_gain(double lookup_log_raw, int channel, double stops);

// ---- Bleach bypass ------------------------------------------------------------
struct BleachResult {
    double dye[3];   // dye densities after the bypass
    double silver;   // neutral retained-silver density, added at every wavelength
};
// `print_stage` selects the print constants. For film, `positive` selects the
// reversal silver image (d_max - d) and disables the leuco-cyan loss.
BleachResult bleach_bypass(const double* density_cmy, int n_ch, double amount, bool print_stage,
                           bool positive, const double* density_max, double leuco_cyan_coupling);

// ---- HDR output ---------------------------------------------------------------
// Scene-referred linear RGB (1.0 = reference white) -> display light in nits,
// with a luminance tone map toward the peak.
Vec3 hdr_tone_map(const Vec3& rgb, const HdrOutputParams& p);
double encode_pq(double nits);                      // SMPTE ST 2084 inverse EOTF
double encode_hlg_display(double nits, double peak_nits);  // display nits -> HLG signal

}  // namespace spectra::extended
