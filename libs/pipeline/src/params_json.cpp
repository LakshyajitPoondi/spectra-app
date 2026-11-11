// SPDX-License-Identifier: GPL-3.0-or-later
#include "spectra/pipeline/pipeline.hpp"
#include <optional>
#include <stdexcept>
#include <string>

namespace nlohmann {
template <typename T>
struct adl_serializer<std::optional<T>> {
    static void to_json(json& j, const std::optional<T>& o) {
        if (o)
            j = *o;
        else
            j = nullptr;
    }
    static void from_json(const json& j, std::optional<T>& o) {
        if (j.is_null())
            o = std::nullopt;
        else
            o = j.get<T>();
    }
};
}  // namespace nlohmann

namespace spectra {

using nlohmann::json;

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ChemistryParams, development_time, active, gamma_factor,
                                   gamma_factor_fast, gamma_factor_slow, gamma_factor_red,
                                   gamma_factor_green, gamma_factor_blue, developer_exhaustion)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(DiffusionFilterParams, active, filter_family, strength,
                                   spatial_scale, halo_warmth, core_intensity, core_size,
                                   halo_intensity, halo_size, bloom_intensity, bloom_size)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(CameraParams, exposure_compensation_ev, auto_exposure,
                                   auto_exposure_method, lens_blur_um, film_format_mm, color_filter,
                                   diffusion_filter)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(EnlargerParams, illuminant, print_exposure,
                                   print_exposure_compensation, normalize_print_exposure,
                                   y_filter_shift, m_filter_shift, y_filter_neutral,
                                   m_filter_neutral, c_filter_neutral, lens_blur, diffusion_filter,
                                   preflash_exposure, preflash_y_filter_shift,
                                   preflash_m_filter_shift)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ScannerParams, lens_blur, white_correction, black_correction,
                                   white_level, black_level, unsharp_mask)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(GrainParams, active, rms_granularity, density_min, uniformity,
                                   particle_scale_sublayers, blur, mult_usm_sigma, mult_usm_amount,
                                   blur_dye_clouds_um, micro_structure, micro_sublayers)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(HalationParams, active, scatter_amount, scatter_spatial_scale,
                                   halation_amount, halation_spatial_scale, scatter_core_um,
                                   scatter_tail_um, scatter_tail_weight, boost_ev, boost_range,
                                   protect_ev, halation_strength, halation_first_sigma_um,
                                   halation_n_bounces, halation_bounce_decay, halation_renormalize)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(DirCouplersParams, active, amount, inhibition_samelayer,
                                   inhibition_interlayer, gamma_samelayer_rgb,
                                   gamma_interlayer_r_to_gb, gamma_interlayer_g_to_rb,
                                   gamma_interlayer_b_to_rg, langmuir_donor_k_rgb,
                                   langmuir_receiver_k_rgb, diffusion_size_um, diffusion_tail_um,
                                   diffusion_tail_weight)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(GlareParams, active, percent, roughness, blur)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(BaseParams, active, scale, cyan, magenta, yellow)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(FilmRenderingParams, chemistry, grain, halation, dir_couplers,
                                   glare, base)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(PrintRenderingParams, glare, chemistry)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(InputGamutCompressSpec, active, algorithm, knee)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(OutputGamutCompressSpec, algorithm, knee, lightness_compression)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(IOParams, input_color_space, input_cctf_decoding,
                                   output_color_space, output_cctf_encoding, input_gamut_compress,
                                   output_gamut_compress, crop, crop_center, crop_size,
                                   upscale_factor, scan_film)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(DebugParams, deactivate_spatial_effects,
                                   deactivate_stochastic_effects, print_timings, lut_mode)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(SettingsParams, rgb_to_raw_method,
                                   apply_hanatos2025_adaptation_window,
                                   apply_hanatos2025_adaptation_surface, spectral_gaussian_blur,
                                   use_enlarger_lut, use_scanner_lut, lut_resolution, use_fast_stats,
                                   preview_max_size, preview_mode,
                                   neutral_print_filters_from_database)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Params, film, print, film_render, print_render, camera, enlarger,
                                   scanner, io, debug, settings)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(RenderOptions, match_python, seed, reference_pixel_um,
                                   autoexposure_ev, boost_max_raw, full_long_edge_px)

namespace {
void deep_update(json& target, const json& patch, const std::string& path) {
    if (!patch.is_object()) {
        target = patch;
        return;
    }
    for (auto it = patch.begin(); it != patch.end(); ++it) {
        if (!target.is_object() || !target.contains(it.key()))
            throw std::runtime_error("unknown parameter '" + path + it.key() + "'");
        deep_update(target[it.key()], it.value(), path + it.key() + ".");
    }
}
}  // namespace

json params_to_json(const Params& p) { return json(p); }
Params params_from_json(const json& j) { return j.get<Params>(); }

void apply_params_patch(Params& p, const json& patch) {
    json j = p;
    deep_update(j, patch, "");
    p = j.get<Params>();
}

json render_options_to_json(const RenderOptions& o) { return json(o); }

void apply_render_options_patch(RenderOptions& o, const json& patch) {
    json j = o;
    deep_update(j, patch, "options.");
    o = j.get<RenderOptions>();
}

}  // namespace spectra
