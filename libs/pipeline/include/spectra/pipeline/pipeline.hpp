// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <vector>
#include <map>
#include <memory>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "spectra/core/image.hpp"
#include "spectra/data/pack.hpp"
#include "spectra/model/colour.hpp"
#include "spectra/model/curves.hpp"
#include "spectra/model/gamut.hpp"
#include "spectra/model/metering.hpp"
#include "spectra/model/params.hpp"
#include "spectra/model/upsampling.hpp"

namespace spectra {

// ---- Parameters <-> JSON ------------------------------------------------------
nlohmann::json params_to_json(const Params& p);
Params params_from_json(const nlohmann::json& j);
// Recursively overwrite the fields named in `patch` (objects merge, everything
// else, including null, replaces).
void apply_params_patch(Params& p, const nlohmann::json& patch);
nlohmann::json render_options_to_json(const RenderOptions& o);
void apply_render_options_patch(RenderOptions& o, const nlohmann::json& patch);

// ---- Digest -------------------------------------------------------------------
// Port of params_builder.digest_params: neutral print filters from the
// database, preview-mode switches, halation / grain / coupler presets of the
// film stock, and the debug switches.
Params digest_params(const Pack& pack, Params p, bool apply_stocks_specifics = true);

// ---- Pipeline -----------------------------------------------------------------
enum class Tap { RgbIn, RgbPre, LogEFilm, CmyFilm, LogEPrint, CmyPrint, RgbOut };
Tap tap_from_string(const std::string& s);
std::string tap_to_string(Tap t);

// A profile prepared for rendering: curves sampled from the model, the
// development time selected and (for film) the chemistry morph baked in.
struct RuntimeProfile {
    Profile info;
    DensityCurvesModel model;  // one row per channel
    Curves curves;             // film: morphed total + layers; print: fitted total
    Vec base_density;          // [81]
    Vec sensitivity;           // nan_to_num(10^log_sensitivity) [81 x n_ch]
    int n_ch = 3;
};

// Whole-image statistics a render produced. Feed them back through
// RenderOptions to make a preview or a tile match the full render.
struct RenderStats {
    double pixel_size_um = 0.0;
    std::optional<double> autoexposure_ev;
    std::optional<double> boost_max_raw;
};

// CPU reference implementation of SimulationPipeline. Parameters must already
// be digested. The object is immutable after construction except for the
// per-render state kept inside process().
class Pipeline {
public:
    Pipeline(std::shared_ptr<const ColourData> colour, const Params& digested,
             const RenderOptions& options = {});

    Image process(const Image& image, Tap inject = Tap::RgbIn, Tap collect = Tap::RgbOut,
                  RenderStats* stats = nullptr) const;

    const Params& params() const { return p_; }
    const RuntimeProfile& film() const { return film_; }
    const RuntimeProfile& print() const { return print_; }
    const TcLut& tc_lut() const { return tc_lut_; }
    const Vec& density_spectral_midgray() const { return midgray_; }
    // Print curves after the print chemistry morph [n_le x n_ch].
    const Vec& print_curves_morphed() const { return print_curves_morphed_; }
    // Per-pixel spectral functions (exposed for the LUT exporter and tests).
    void film_cmy_to_print_log_raw(const double* cmy, double* out) const;
    void cmy_to_log_xyz(const double* cmy, double* out) const;

private:
    struct State;
    Image rgb_to_film_raw(const Image& rgb, const std::string& color_space, bool decode) const;
    Image preprocess(const Image& image, State& st) const;
    Image filming_expose(const Image& rgb, State& st) const;
    Image filming_develop(const Image& log_raw, State& st) const;
    Image printing_expose(const Image& cmy, State& st) const;
    Image printing_develop(const Image& log_raw, State& st) const;
    Image scan(const Image& density, State& st) const;
    void update_bw_references(bool in_print, State& st) const;
    void correction_function(const State& st, double& m, double& q, bool& active) const;
    double bw_filming_exposure_correction(State& st) const;
    double bw_printing_exposure_correction(State& st) const;
    Vec density_spectral_for(const Image& raw_1x1, bool with_extended) const;
    // Extended features
    bool experimental_push() const;
    void film_stage_dyes(const double* cmy, double* dye, double& silver) const;
    Vec film_dmax_norm_, print_dmax_, midgray_extended_, retime_;
    double film_axis_gamma_ = 1.0;

    std::shared_ptr<const ColourData> cd_;
    Params p_;
    RenderOptions opt_;
    RuntimeProfile film_, print_;
    Vec film_sensitivity_filtered_;  // with the camera colour filter folded in
    TcLut tc_lut_;
    Vec midgray_, midgray_comp_;     // spectral density of mid-grey (81); comp may be empty
    Vec print_curves_morphed_;
    // printing tables
    Vec print_illuminant_;           // filtered enlarger light (81)
    Vec preflash_raw_;               // [n_ch print]
    double exposure_factor_ = 1.0;
    // scanning tables
    Vec scan_channel_density_, scan_base_, scan_illuminant_;
    int scan_n_ch_ = 3;
    double scan_norm_ = 1.0;
    Vec3 scan_illuminant_xyz_{};
    Mat3 xyz_to_rgb_{};
    std::unique_ptr<OutputGamutCompressor> compressor_;
    double black_level_ = 0.0, white_level_ = 1.0;
};

// Convenience: init + digest + render (the equivalent of spektrafilm.simulate).
Image simulate(std::shared_ptr<const ColourData> colour, const Image& image, const Params& params,
               const RenderOptions& options = {});

}  // namespace spectra
