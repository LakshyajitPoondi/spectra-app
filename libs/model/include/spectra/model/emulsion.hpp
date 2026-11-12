// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>

#include "spectra/core/image.hpp"
#include "spectra/model/curves.hpp"
#include "spectra/model/params.hpp"

namespace spectra {

// ---- DIR couplers -----------------------------------------------------------

// compute_dir_couplers_matrix: [n_ch x n_ch], donor row, receiver column.
Vec dir_couplers_matrix(const DirCouplersParams& p, int n_ch);

// Tables shared by the curve inversion and the per-pixel correction.
struct DirCouplersTables {
    int n_ch = 0;
    bool positive = false;
    Vec matrix;          // amount-scaled [n_ch x n_ch]
    bool has_donor_k = false;
    Vec donor_k, d_ref;  // Langmuir donor (negative)
    bool has_receiver = false;
    Vec receiver_kr, receiver_c_ref;  // receiver saturation (positive)
    Vec density_max;     // per channel
    Vec curves_before;   // density curves before couplers [n_le x n_ch]
};
DirCouplersTables build_dir_couplers_tables(const DirCouplersParams& p, const Vec& log_exposure,
                                            const Vec& normalized_curves, int n_ch,
                                            const std::string& profile_type);

// apply_density_correction_dir_couplers.
Image apply_dir_couplers(const Image& density_cmy, const Image& log_raw, double pixel_size_um,
                         const Vec& log_exposure, const Vec& normalized_curves,
                         const DirCouplersParams& p, const std::string& profile_type,
                         double exposure_axis_gamma = 1.0);

// ---- Grain ------------------------------------------------------------------

struct GrainLayerParams {
    int n_ch = 0, n_sub = 0;
    Vec density_min;         // [n_ch]
    Vec density_min_layers;  // [n_sub x n_ch]
    Vec density_max_layers;  // [n_sub x n_ch], floor included
    Vec n_particles;         // [n_sub x n_ch] grains per pixel
    Vec uniformity;          // [n_ch]
};
// _layer_grain_params with the exact curve-based peak.
GrainLayerParams grain_layer_params(const GrainParams& g, const Curves& normalized_curves,
                                    double pixel_size_um);

struct GrainOptions {
    uint64_t seed = 0;
    bool use_fast_stats = false;
    // Scale applied to the pixel-unit parameters (blur, USM sigma, dye-cloud
    // blur). 1 reproduces the Python reference.
    double pixel_param_scale = 1.0;
};

// apply_grain: per channel and sub-layer compound Poisson-Binomial sampling,
// dye-cloud blur, micro-structure clumping, blur, multiplicative unsharp mask.
// `curves` are the normalised film curves including their sub-layers.
Image apply_grain(const Image& density_cmy, double pixel_size_um, const GrainParams& g,
                  const Curves& normalized_curves, const std::string& profile_type,
                  const GrainOptions& opt);

// Samplers (exposed for the statistical tests).
// Exact: Poisson by inversion / PTRS, binomial by inversion / BTRS.
// Fast: the approximations of spektrafilm.utils.fast_stats.
class PixelRng;
int sample_poisson(PixelRng& rng, double lambda, bool fast);
int sample_binomial(PixelRng& rng, int n, double p, bool fast);

// layer_particle_model without the dye-cloud blur: one emulsion sub-layer sampled
// at the given (floor-inclusive) density field. Compound Poisson-Binomial:
// N_s ~ Poisson(N / saturation) sensitised grains, each developed with
// probability D / Dmax. `density` and `out` may alias.
void sample_grain_layer(const double* density, double* out, int h, int w, double density_max,
                        double n_particles, double uniformity, uint64_t seed, uint32_t stream,
                        bool fast);

// ---- Glare ------------------------------------------------------------------

// add_glare: adds a blurred log-normal random amount of the illuminant.
void add_glare(Image& xyz, const Vec3& illuminant_xyz, const GlareParams& g, uint64_t seed,
               double pixel_param_scale = 1.0);

}  // namespace spectra
