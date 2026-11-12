// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <string>

#include "spectra/core/math.hpp"
#include "spectra/model/curves.hpp"

// C++ mirror of spektrafilm.runtime.params_schema.RuntimePhotoParams.
// Field names, units and defaults match the Python dataclasses one to one.
namespace spectra {

using Vec2 = std::array<double, 2>;

struct DiffusionFilterParams {
    bool active = false;
    std::string filter_family = "black_pro_mist";
    double strength = 0.5;
    double spatial_scale = 1.0;
    double halo_warmth = 0.0;
    double core_intensity = 1.0;
    double core_size = 1.0;
    double halo_intensity = 1.0;
    double halo_size = 1.0;
    double bloom_intensity = 1.0;
    double bloom_size = 1.0;
};

struct CameraParams {
    double exposure_compensation_ev = 0.0;
    bool auto_exposure = true;
    std::string auto_exposure_method = "center_weighted";
    double lens_blur_um = 0.0;
    double film_format_mm = 35.0;
    std::string color_filter = "none";
    DiffusionFilterParams diffusion_filter;
};

struct EnlargerParams {
    std::string illuminant = "TH-KG3";
    double print_exposure = 1.0;
    bool print_exposure_compensation = true;
    bool normalize_print_exposure = true;
    double y_filter_shift = 0.0;
    double m_filter_shift = 0.0;
    double y_filter_neutral = 55;
    double m_filter_neutral = 65;
    double c_filter_neutral = 0;
    double lens_blur = 0.0;
    DiffusionFilterParams diffusion_filter;
    double preflash_exposure = 0.0;
    double preflash_y_filter_shift = 0.0;
    double preflash_m_filter_shift = 0.0;
};

struct ScannerParams {
    double lens_blur = 0.0;
    bool white_correction = false;
    bool black_correction = false;
    double white_level = 0.98;
    double black_level = 0.01;
    Vec2 unsharp_mask{0.7, 0.7};
};

struct GrainParams {
    bool active = true;
    Vec3 rms_granularity{6, 8, 10};
    Vec3 density_min{0.03, 0.03, 0.03};
    Vec3 uniformity{0.97, 0.97, 0.97};
    Vec3 particle_scale_sublayers{1.0, 0.5, 0.25};
    double blur = 0.8;
    double mult_usm_sigma = 0.7;
    double mult_usm_amount = 1.5;
    double blur_dye_clouds_um = 2.0;
    Vec2 micro_structure{0.2, 30};
    int micro_sublayers = 1;
};

struct HalationParams {
    bool active = true;
    double scatter_amount = 1.0;
    double scatter_spatial_scale = 1.0;
    double halation_amount = 1.0;
    double halation_spatial_scale = 1.0;
    Vec3 scatter_core_um{2.2, 2.0, 1.6};
    Vec3 scatter_tail_um{9.3, 9.7, 9.1};
    Vec3 scatter_tail_weight{0.78, 0.65, 0.67};
    double boost_ev = 0.0;
    double boost_range = 0.3;
    double protect_ev = 4.0;
    Vec3 halation_strength{0.05, 0.015, 0.0};
    Vec3 halation_first_sigma_um{65.0, 65.0, 65.0};
    int halation_n_bounces = 3;
    double halation_bounce_decay = 0.5;
    bool halation_renormalize = true;
};

struct DirCouplersParams {
    bool active = true;
    double amount = 1.0;
    double inhibition_samelayer = 1.0;
    double inhibition_interlayer = 1.0;
    Vec3 gamma_samelayer_rgb{0.341, 0.324, 0.273};
    Vec2 gamma_interlayer_r_to_gb{0.355, 0.305};
    Vec2 gamma_interlayer_g_to_rb{0.154, 0.358};
    Vec2 gamma_interlayer_b_to_rg{0.171, 0.225};
    Vec3 langmuir_donor_k_rgb{1.0, 1.0, 1.0};
    Vec3 langmuir_receiver_k_rgb{1.0, 1.0, 1.0};
    double diffusion_size_um = 20.0;
    double diffusion_tail_um = 200.0;
    double diffusion_tail_weight = 0.03;
};

struct GlareParams {
    bool active = true;
    double percent = 0.03;
    double roughness = 0.7;
    double blur = 0.5;
};

struct BaseParams {
    bool active = true;
    double scale = 1.0;
    double cyan = 1.0;
    double magenta = 1.0;
    double yellow = 1.0;
};

struct FilmRenderingParams {
    ChemistryParams chemistry;
    GrainParams grain;
    HalationParams halation;
    DirCouplersParams dir_couplers;
    GlareParams glare;
    BaseParams base;
};

struct PrintRenderingParams {
    GlareParams glare;
    ChemistryParams chemistry;
};

struct InputGamutCompressSpec {
    bool active = true;
    std::string algorithm = "xy";  // "xy" | "oklch"
    Vec3 knee{0.0, 1.0, 6.0};
};

struct OutputGamutCompressSpec {
    std::string algorithm = "oklch";  // off | aces_rgc | oklch | oklrab | jzazbz | cam16ucs
    Vec3 knee{0.0, 1.0, 6.0};
    std::optional<Vec3> lightness_compression = Vec3{0.7, 1.0, 2.2};
};

struct IOParams {
    std::string input_color_space = "ProPhoto RGB";
    bool input_cctf_decoding = false;
    std::string output_color_space = "sRGB";
    bool output_cctf_encoding = true;
    InputGamutCompressSpec input_gamut_compress;
    OutputGamutCompressSpec output_gamut_compress;
    bool crop = false;
    Vec2 crop_center{0.5, 0.5};
    Vec2 crop_size{0.1, 0.1};
    double upscale_factor = 1.0;
    bool scan_film = false;
};

struct DebugParams {
    bool deactivate_spatial_effects = false;
    bool deactivate_stochastic_effects = false;
    bool print_timings = false;
    bool lut_mode = false;
};

struct SettingsParams {
    std::string rgb_to_raw_method = "hanatos2025";
    bool apply_hanatos2025_adaptation_window = true;
    bool apply_hanatos2025_adaptation_surface = false;
    double spectral_gaussian_blur = 0.0;
    bool use_enlarger_lut = false;
    bool use_scanner_lut = false;
    int lut_resolution = 17;
    bool use_fast_stats = false;
    int preview_max_size = 640;
    bool preview_mode = false;
    bool neutral_print_filters_from_database = true;
};

// "Extended" features. They are NOT part of the spektrafilm reference: the
// models are adapted from the spektrafilm-ofx port (Aedan Diez, GPL-3.0).
// Each is separately switchable and off by default, and none is covered by the
// parity tests against Python (they have sanity tests instead).
struct PushPullParams {
    bool active = false;
    // "standard": development time changes the curve gamma (the exposure axis
    // of the film curves is scaled about mid-grey, after the DIR-coupler tables
    // are built). "experimental": per-layer, per-tone-region warp.
    std::string mode = "standard";
    double film_stops = 0.0;   // + push, - pull; clamped to [-2, 2]
    double print_stops = 0.0;  // print / paper development, gamma only
};

struct BleachBypassParams {
    bool active = false;
    double film_amount = 0.0;   // 0..1 fraction of the silver image retained in the film
    double print_amount = 0.0;  // 0..1 fraction retained in the print
    double leuco_cyan_coupling = 1.0;  // negative film only: cyan dye loss multiplier (0..2)
};

struct HdrOutputParams {
    bool active = false;
    std::string transfer = "pq";         // "pq" (SMPTE ST 2084) | "hlg" (ARIB STD-B67)
    double reference_white_nits = 203.0; // luminance of output value 1.0
    double peak_nits = 1000.0;
    double exposure_ev = 0.0;
    std::string tone_mapping = "shoulder";  // "shoulder" (soft roll-off to peak) | "clip"
};

struct ExtendedParams {
    // Re-time the print for mid-grey when a film-side Extended feature is on,
    // as a lab does when printing a pushed or bleach-bypassed negative: mid-grey
    // prints at the same density and colour as with the normal process.
    bool retime_print_to_midgray = true;
    PushPullParams push_pull;
    BleachBypassParams bleach_bypass;
    HdrOutputParams hdr_output;
};

// Options of this app that have no Python counterpart.
struct RenderOptions {
    // true: reproduce the Python reference exactly, including its
    // resolution-dependent behaviour (used by the parity tests).
    // false: pixel-unit parameters are interpreted at a reference pixel pitch
    // and scaled to the actual one, so the look is the same at any resolution.
    bool match_python = false;
    uint64_t seed = 0;                       // seed of all stochastic effects
    double reference_pixel_um = 6.0;         // pitch at which pixel-unit params are defined
    // Whole-image statistics measured on the full image and reused by previews
    // and tiles so that they match the final render.
    std::optional<double> autoexposure_ev;   // overrides the metered EV
    std::optional<double> boost_max_raw;     // overrides max(raw) in the highlight boost
    std::optional<double> full_long_edge_px; // long edge of the full image (for pixel size)
};

struct Params {
    std::string film = "kodak_portra_400";
    std::string print = "kodak_portra_endura";
    FilmRenderingParams film_render;
    PrintRenderingParams print_render;
    CameraParams camera;
    EnlargerParams enlarger;
    ScannerParams scanner;
    IOParams io;
    DebugParams debug;
    SettingsParams settings;
    ExtendedParams extended;
};

}  // namespace spectra
