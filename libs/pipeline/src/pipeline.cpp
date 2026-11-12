// SPDX-License-Identifier: GPL-3.0-or-later
#include "spectra/pipeline/pipeline.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include <stdexcept>

#include "spectra/core/parallel.hpp"
#include "spectra/model/diffusion.hpp"
#include "spectra/model/emulsion.hpp"
#include "spectra/model/filters.hpp"

namespace spectra {

namespace {

const int NL = kNumWavelengths;
constexpr double kLn10 = 2.302585092994046;

double nan_to_num(double v) {
    if (std::isnan(v)) return 0.0;
    if (std::isinf(v)) return v > 0 ? 1.7976931348623157e308 : -1.7976931348623157e308;
    return v;
}

inline double pow10(double x) { return std::exp(x * kLn10); }

// density_to_light for one wavelength: 10^-D * light, NaN -> 0.
inline double transmitted(double density, double light) {
    double t = pow10(-density) * light;
    return std::isnan(t) ? 0.0 : t;
}

RuntimeProfile make_runtime_profile(const Pack& pack, const std::string& stock,
                                    const ChemistryParams& chem, const Vec& le, bool bake_morph) {
    RuntimeProfile r;
    r.info = load_profile(pack, stock);
    const Profile& p = r.info;
    r.n_ch = p.n_channels();
    int idx = select_development_time_index(p.development_time, p.model.n_rows, chem.development_time);
    if (idx >= 0) {
        r.model = select_model_row(p.model, idx);
        r.base_density.resize(NL);
        for (int l = 0; l < NL; ++l)
            r.base_density[l] = p.base_density_cols > 1 ? p.base_density[l * p.base_density_cols + idx]
                                                         : p.base_density[l];
    } else {
        r.model = p.model;
        if (p.base_density_cols != 1) throw std::runtime_error(stock + ": unexpected base density shape");
        r.base_density = p.base_density;
    }
    if (r.model.n_rows != r.n_ch)
        throw std::runtime_error(stock + ": density model rows do not match the channel count");
    if (bake_morph) {
        r.curves = apply_curves_morph(le, r.model, chem, p.type);
        if (r.model.n_layers <= 1) r.curves.layers.clear();
    } else {
        r.curves = evaluate_density_curves(r.model, le, p.type);
    }
    r.sensitivity.resize(p.log_sensitivity.size());
    for (size_t i = 0; i < r.sensitivity.size(); ++i)
        r.sensitivity[i] = nan_to_num(std::pow(10.0, p.log_sensitivity[i]));
    return r;
}

// color_enlarger with the custom dichroic set: illuminant filtered by CMY
// filters given in Kodak CC units.
Vec color_enlarger(const ColourData& cd, const Vec& light, double c, double m, double y) {
    Vec filters = cd.dichroic("custom");
    double tv[3] = {std::pow(10.0, -(c / 100.0)), std::pow(10.0, -(m / 100.0)), std::pow(10.0, -(y / 100.0))};
    Vec out(NL);
    for (int l = 0; l < NL; ++l) {
        double total = 1.0;
        for (int k = 0; k < 3; ++k) total *= 1 - (1 - filters[l * 3 + k]) * (1 - tv[k]);
        out[l] = light[l] * total;
    }
    return out;
}

// _tuned_base_density / base_density_tuning
Vec tuned_base_density(const ColourData& cd, const Vec& base, const BaseParams& bp) {
    if (!bp.active) return base;
    Vec tuned = base;
    if (!(bp.cyan == 1.0 && bp.magenta == 1.0 && bp.yellow == 1.0)) {
        const Vec& wl = cd.wavelengths();
        Vec peaks = {445, 530, 610};
        Vec ch = np_interp(peaks, wl, base);
        ch[0] *= bp.yellow;
        ch[1] *= bp.magenta;
        ch[2] *= bp.cyan;
        Vec spectral_min = np_interp(wl, peaks, ch);
        double mean_step = (wl.back() - wl.front()) / (wl.size() - 1);
        scipy_gaussian_filter1d(spectral_min.data(), tuned.data(), NL, 1, 20.0 / mean_step, 0, 4.0);
    }
    for (auto& v : tuned) v *= bp.scale;
    return tuned;
}

}  // namespace

Tap tap_from_string(const std::string& s) {
    static const std::map<std::string, Tap> m = {
        {"rgb_in", Tap::RgbIn},         {"rgb_pre", Tap::RgbPre},     {"log_e_film", Tap::LogEFilm},
        {"cmy_film", Tap::CmyFilm},     {"log_e_print", Tap::LogEPrint}, {"cmy_print", Tap::CmyPrint},
        {"rgb_out", Tap::RgbOut}};
    auto it = m.find(s);
    if (it == m.end()) throw std::runtime_error("unknown tap '" + s + "'");
    return it->second;
}

std::string tap_to_string(Tap t) {
    static const char* names[] = {"rgb_in", "rgb_pre", "log_e_film", "cmy_film", "log_e_print", "cmy_print", "rgb_out"};
    return names[static_cast<int>(t)];
}

struct Pipeline::State {
    double pixel_size_um = 0.0;
    bool have_pixel_size = false;
    Image full_frame_preview;
    bool have_preview = false;
    Vec log_raw_print_black, log_raw_print_white;
    double y_black = 0.0, y_white = 0.0;
    bool have_bw_refs = false;
    double pixel_param_scale = 1.0;
    RenderStats stats;
};

Pipeline::Pipeline(std::shared_ptr<const ColourData> colour, const Params& digested,
                   const RenderOptions& options)
    : cd_(std::move(colour)), p_(digested), opt_(options) {
    const Pack& pack = cd_->pack();
    const Vec& le = cd_->log_exposure();
    film_ = make_runtime_profile(pack, p_.film, p_.film_render.chemistry, le, true);
    print_ = make_runtime_profile(pack, p_.print, p_.print_render.chemistry, le, false);

    // Black / white reference levels are given as sRGB-encoded values.
    {
        const ColourSpace& srgb = cd_->space("sRGB");
        Mat3 M = cd_->rgb_to_rgb(srgb, srgb);
        auto remove_cctf = [&](double level) {
            double v = cctf_decode("srgb", level);
            Vec3 o = mat3_apply(M, {v, v, v});
            return (o[0] + o[1] + o[2]) / 3.0;
        };
        black_level_ = remove_cctf(p_.scanner.black_level);
        white_level_ = remove_cctf(p_.scanner.white_level);
    }

    // Film sensitivity with the camera taking filter folded in.
    film_sensitivity_filtered_ = film_.sensitivity;
    Vec filt = cd_->camera_filter(p_.camera.color_filter);
    if (!filt.empty())
        for (int l = 0; l < NL; ++l)
            for (int k = 0; k < film_.n_ch; ++k) film_sensitivity_filtered_[l * film_.n_ch + k] *= filt[l];

    if (p_.settings.rgb_to_raw_method == "hanatos2025") {
        HanatosAdaptation ad;
        ad.window_params = film_.info.window_params;
        ad.surface_params = film_.info.surface_params;
        ad.reference_illuminant = film_.info.reference_illuminant;
        ad.apply_window = p_.settings.apply_hanatos2025_adaptation_window;
        ad.apply_surface = p_.settings.apply_hanatos2025_adaptation_surface;
        ad.spectral_gaussian_blur = p_.settings.spectral_gaussian_blur;
        tc_lut_ = compute_tc_lut(*cd_, film_sensitivity_filtered_, film_.n_ch, ad, p_.io.input_gamut_compress);
    } else if (p_.settings.rgb_to_raw_method != "mallett2019") {
        throw std::runtime_error("unsupported rgb_to_raw method: " + p_.settings.rgb_to_raw_method);
    }

    // Mid-grey spectral density used to balance the print exposure.
    {
        Image mid(1, 1, 3, 0.184);
        Image raw_mid = rgb_to_film_raw(mid, "sRGB", false);
        double scale = 1.0;
        if (p_.camera.auto_exposure)
            scale = std::pow(2.0, measure_raw_autoexposure_ev(raw_mid, p_.camera.auto_exposure_method));
        Image scaled = raw_mid;
        for (auto& v : scaled.v) v *= scale;
        midgray_ = density_spectral_for(scaled);
        if (p_.enlarger.print_exposure_compensation) {
            Image mid_c(1, 1, 3, 0.184 * std::pow(2.0, p_.camera.exposure_compensation_ev));
            Image raw_c = rgb_to_film_raw(mid_c, "sRGB", false);
            for (auto& v : raw_c.v) v *= scale;
            midgray_comp_ = density_spectral_for(raw_c);
        }
    }

    // Printing tables.
    {
        Vec light = cd_->illuminant(p_.enlarger.illuminant);
        const EnlargerParams& e = p_.enlarger;
        print_illuminant_ = color_enlarger(*cd_, light, e.c_filter_neutral, e.m_filter_neutral + e.m_filter_shift,
                                           e.y_filter_neutral + e.y_filter_shift);
        int pc = print_.n_ch;
        auto exposure_factor = [&](const Vec& density_mid) {
            double log_sum = 0;
            for (int m = 0; m < pc; ++m) {
                double raw = 0;
                for (int l = 0; l < NL; ++l)
                    raw += transmitted(density_mid[l], print_illuminant_[l]) * print_.sensitivity[l * pc + m];
                log_sum += std::log(std::fmax(raw, 1e-10));
            }
            return 1.0 / std::exp(log_sum / pc);
        };
        double f_mid = exposure_factor(midgray_);
        double f_comp = midgray_comp_.empty() ? 1.0 : exposure_factor(midgray_comp_);
        if (e.print_exposure_compensation && !e.normalize_print_exposure) exposure_factor_ = f_comp / f_mid;
        else if (e.normalize_print_exposure && e.print_exposure_compensation) exposure_factor_ = f_comp;
        else if (e.normalize_print_exposure && !e.print_exposure_compensation) exposure_factor_ = f_mid;
        else exposure_factor_ = 1.0;
        preflash_raw_.assign(pc, 0.0);
        if (e.preflash_exposure > 0) {
            Vec pre = color_enlarger(*cd_, light, e.c_filter_neutral, e.m_filter_neutral + e.preflash_m_filter_shift,
                                     e.y_filter_neutral + e.preflash_y_filter_shift);
            for (int m = 0; m < pc; ++m) {
                double raw = 0;
                for (int l = 0; l < NL; ++l)
                    raw += transmitted(film_.base_density[l], pre[l]) * print_.sensitivity[l * pc + m];
                preflash_raw_[m] = raw * e.preflash_exposure;
            }
        }
        print_curves_morphed_ = apply_curves_morph(le, print_.model, p_.print_render.chemistry, print_.info.type).total;
    }

    // Scanning tables.
    {
        const RuntimeProfile& medium = p_.io.scan_film ? film_ : print_;
        scan_n_ch_ = medium.n_ch;
        scan_channel_density_ = medium.info.channel_density;
        scan_base_ = tuned_base_density(*cd_, medium.base_density, p_.film_render.base);
        scan_illuminant_ = cd_->illuminant(medium.info.viewing_illuminant);
        const Vec& cmf = cd_->cmfs();
        scan_norm_ = 0;
        for (int l = 0; l < NL; ++l) scan_norm_ += scan_illuminant_[l] * cmf[l * 3 + 1];
        for (int k = 0; k < 3; ++k) {
            double s = 0;
            for (int l = 0; l < NL; ++l) s += scan_illuminant_[l] * cmf[l * 3 + k];
            scan_illuminant_xyz_[k] = s / scan_norm_;
        }
        double sum = scan_illuminant_xyz_[0] + scan_illuminant_xyz_[1] + scan_illuminant_xyz_[2];
        Xy illum_xy{scan_illuminant_xyz_[0] / sum, scan_illuminant_xyz_[1] / sum};
        const ColourSpace& out_cs = cd_->space(p_.io.output_color_space);
        xyz_to_rgb_ = cd_->xyz_to_rgb(out_cs, &illum_xy, "CAT02");
        compressor_ = std::make_unique<OutputGamutCompressor>(*cd_, p_.io.output_gamut_compress,
                                                              p_.io.output_color_space);
    }
}

Image Pipeline::rgb_to_film_raw(const Image& rgb, const std::string& color_space, bool decode) const {
    if (p_.settings.rgb_to_raw_method == "hanatos2025")
        return rgb_to_raw_hanatos2025(*cd_, rgb, color_space, decode, film_.info.reference_illuminant, tc_lut_);
    return rgb_to_raw_mallett2019(*cd_, rgb, film_sensitivity_filtered_, film_.n_ch, color_space, decode,
                                  film_.info.reference_illuminant);
}

Vec Pipeline::density_spectral_for(const Image& raw) const {
    const Vec& le = cd_->log_exposure();
    int n = static_cast<int>(le.size()), c = film_.n_ch;
    Vec out(NL, 0.0);
    Vec cmy(c);
    for (int k = 0; k < c; ++k)
        cmy[k] = fast_interp1(std::log10(raw.v[k] + 1e-10), le.data(), film_.curves.total.data() + k, n, 1, c);
    for (int l = 0; l < NL; ++l) {
        double d = 0;
        for (int k = 0; k < c; ++k) d += cmy[k] * film_.info.channel_density[l * c + k];
        out[l] = d + film_.base_density[l];
    }
    return out;
}

void Pipeline::film_cmy_to_print_log_raw(const double* cmy, double* out) const {
    int fc = film_.n_ch, pc = print_.n_ch;
    double raw[3] = {0, 0, 0};
    const Vec& cdn = film_.info.channel_density;
    for (int l = 0; l < NL; ++l) {
        double d = 0;
        for (int k = 0; k < fc; ++k) d += cmy[k] * cdn[l * fc + k];
        d += film_.base_density[l];
        double t = transmitted(d, print_illuminant_[l]);
        for (int m = 0; m < pc; ++m) raw[m] += t * print_.sensitivity[l * pc + m];
    }
    for (int m = 0; m < pc; ++m) {
        double r = raw[m] * exposure_factor_ + preflash_raw_[m];
        out[m] = std::log10(std::fmax(r, 0.0) + 1e-10);
    }
}

void Pipeline::cmy_to_log_xyz(const double* cmy, double* out) const {
    int c = scan_n_ch_;
    const Vec& cmf = cd_->cmfs();
    double xyz[3] = {0, 0, 0};
    for (int l = 0; l < NL; ++l) {
        double d = 0;
        for (int k = 0; k < c; ++k) d += cmy[k] * scan_channel_density_[l * c + k];
        d += scan_base_[l];
        double t = transmitted(d, scan_illuminant_[l]);
        xyz[0] += t * cmf[l * 3];
        xyz[1] += t * cmf[l * 3 + 1];
        xyz[2] += t * cmf[l * 3 + 2];
    }
    for (int k = 0; k < 3; ++k) out[k] = std::log10(std::fmax(xyz[k] / scan_norm_, 0.0) + 1e-10);
}

Image Pipeline::preprocess(const Image& image, State& st) const {
    Image img(image.h, image.w, 3);
    if (image.c < 3) throw std::runtime_error("input image must have at least 3 channels");
    for (size_t i = 0; i < image.pixels(); ++i)
        for (int k = 0; k < 3; ++k) img.v[i * 3 + k] = image.v[i * image.c + k];
    double long_edge = opt_.full_long_edge_px ? *opt_.full_long_edge_px : std::max(img.h, img.w);
    st.pixel_size_um = p_.camera.film_format_mm * 1000 / long_edge;
    st.have_pixel_size = true;
    st.full_frame_preview = metering_preview(img);
    st.have_preview = true;
    if (p_.io.crop) img = crop_image(img, p_.io.crop_center, p_.io.crop_size);
    if (p_.io.upscale_factor != 1.0)
        throw std::runtime_error("io.upscale_factor != 1 is not implemented in the CPU reference yet");
    return img;
}

void Pipeline::correction_function(const State& st, double& m, double& q, bool& active) const {
    double white = white_level_, black = black_level_;
    bool bc = p_.scanner.black_correction, wc = p_.scanner.white_correction;
    m = 1.0;
    q = 0.0;
    active = bc || wc;
    if (bc && !wc) white = st.y_white;
    if (wc && !bc) black = st.y_black;
    if (active) {
        m = (white - black) / (st.y_white - st.y_black + 1e-10);
        q = black - m * st.y_black;
    }
}

void Pipeline::update_bw_references(bool in_print, State& st) const {
    bool bc = p_.scanner.black_correction, wc = p_.scanner.white_correction;
    if (!bc && !wc) return;
    bool scan_film = p_.io.scan_film;
    if (scan_film && film_.info.type == "negative") return;
    double lx[3];
    if (scan_film && film_.info.type == "positive" && !in_print) {
        Vec cmy_black = film_.curves.column_max();
        Vec cmy_white(cmy_black.size(), 0.0);
        cmy_to_log_xyz(cmy_black.data(), lx);
        st.y_black = std::pow(10.0, lx[1]);
        cmy_to_log_xyz(cmy_white.data(), lx);
        st.y_white = std::pow(10.0, lx[1]);
        st.have_bw_refs = true;
    } else if (!scan_film && print_.info.type == "negative" && in_print) {
        const Vec& le = cd_->log_exposure();
        int n = static_cast<int>(le.size()), c = print_.n_ch;
        Vec cmy(c);
        for (int k = 0; k < c; ++k)
            cmy[k] = fast_interp1(st.log_raw_print_black[k], le.data(), print_.curves.total.data() + k, n, 1, c);
        cmy_to_log_xyz(cmy.data(), lx);
        st.y_black = std::pow(10.0, lx[1]);
        for (int k = 0; k < c; ++k)
            cmy[k] = fast_interp1(st.log_raw_print_white[k], le.data(), print_.curves.total.data() + k, n, 1, c);
        cmy_to_log_xyz(cmy.data(), lx);
        st.y_white = std::pow(10.0, lx[1]);
        st.have_bw_refs = true;
    }
}

namespace {
// Shared by the filming and printing exposure corrections.
double midgray_exposure_shift(const RuntimeProfile& prof, const Vec& le, double midgray_corrected,
                              bool negate) {
    double density_mid = -std::log10(0.184);
    double density_mid_corr = -std::log10(midgray_corrected);
    int n = static_cast<int>(le.size()), c = prof.n_ch;
    Vec av(n);
    for (int i = 0; i < n; ++i) {
        double s = 0;
        int cnt = 0;
        for (int k = 0; k < c; ++k) {
            double v = prof.curves.t(i, k);
            if (!std::isnan(v)) {
                s += v;
                ++cnt;
            }
        }
        av[i] = cnt ? s / cnt : NAN;
    }
    double bs = 0;
    int bn = 0;
    for (double v : prof.base_density)
        if (!std::isnan(v)) {
            bs += v;
            ++bn;
        }
    double dmin_av = bs / bn;
    if (negate) {
        Vec neg(n);
        for (int i = 0; i < n; ++i) neg[i] = -av[i];
        double a = -np_interp(-(density_mid_corr - dmin_av), neg, le);
        double b = -np_interp(-(density_mid - dmin_av), neg, le);
        return std::pow(10.0, a - b);
    }
    double a = np_interp(density_mid_corr - dmin_av, av, le);
    double b = np_interp(density_mid - dmin_av, av, le);
    return std::pow(10.0, a - b);
}
}  // namespace

double Pipeline::bw_filming_exposure_correction(State& st) const {
    if (!p_.scanner.black_correction && !p_.scanner.white_correction) return 1.0;
    if (film_.info.type == "negative") return 1.0;
    if (p_.io.scan_film && film_.info.type == "positive") {
        update_bw_references(false, st);
        double m, q;
        bool active;
        correction_function(st, m, q, active);
        double mid_corr = (0.184 - q) / m;
        return 1.0 / midgray_exposure_shift(film_, cd_->log_exposure(), mid_corr, true);
    }
    return 1.0;
}

double Pipeline::bw_printing_exposure_correction(State& st) const {
    if (!p_.scanner.black_correction && !p_.scanner.white_correction) return 1.0;
    if (print_.info.type == "negative") {
        update_bw_references(true, st);
        double m, q;
        bool active;
        correction_function(st, m, q, active);
        double mid_corr = (0.184 - q) / m;
        return midgray_exposure_shift(print_, cd_->log_exposure(), mid_corr, false);
    }
    return 1.0;  // upstream has no positive-print branch
}

Image Pipeline::filming_expose(const Image& rgb, State& st) const {
    Image raw = rgb_to_film_raw(rgb, p_.io.input_color_space, p_.io.input_cctf_decoding);
    if (p_.camera.auto_exposure) {
        double ev;
        if (opt_.autoexposure_ev) {
            ev = *opt_.autoexposure_ev;
        } else {
            if (!st.have_preview)
                throw std::runtime_error("autoexposure metering preview is unavailable; inject at rgb_in "
                                         "or supply options.autoexposure_ev");
            Image meter = rgb_to_film_raw(st.full_frame_preview, p_.io.input_color_space, p_.io.input_cctf_decoding);
            ev = measure_raw_autoexposure_ev(meter, p_.camera.auto_exposure_method);
        }
        st.stats.autoexposure_ev = ev;
        double s = std::pow(2.0, ev);
        for (auto& v : raw.v) v = v * s;
    }
    {
        double s = std::pow(2.0, p_.camera.exposure_compensation_ev);
        for (auto& v : raw.v) v *= s;
    }
    const HalationParams& h = p_.film_render.halation;
    if (h.boost_ev != 0) {
        double mx = raw.v.empty() ? 0.0 : raw.v[0];
        for (double v : raw.v) mx = std::max(mx, v);
        st.stats.boost_max_raw = opt_.boost_max_raw ? *opt_.boost_max_raw : mx;
    }
    boost_highlights(raw, h.boost_ev, h.boost_range, h.protect_ev, opt_.boost_max_raw);
    bool need_px = p_.camera.diffusion_filter.active || p_.camera.lens_blur_um > 0 || h.active;
    if (need_px && !st.have_pixel_size) throw std::runtime_error("pixel size is unknown");
    raw = apply_diffusion_filter_um(raw, p_.camera.diffusion_filter, st.pixel_size_um);
    raw = apply_gaussian_blur_um(raw, p_.camera.lens_blur_um, st.pixel_size_um);
    apply_halation_um(raw, h, st.pixel_size_um);
    double corr = bw_filming_exposure_correction(st);
    Image log_raw(raw.h, raw.w, raw.c);
    parallel_for(static_cast<long>(raw.v.size()), [&](long a, long b) {
        for (long i = a; i < b; ++i) log_raw.v[i] = std::log10(std::fmax(raw.v[i] * corr, 0.0) + 1e-10);
    }, 4096);
    return log_raw;
}

Image Pipeline::filming_develop(const Image& log_raw, State& st) const {
    const Vec& le = cd_->log_exposure();
    // develop(): curves normalised by their per-channel minimum.
    Curves norm = film_.curves;
    Vec mins = norm.column_min();
    for (int i = 0; i < norm.n_le; ++i)
        for (int k = 0; k < norm.n_ch; ++k) norm.total[static_cast<size_t>(i) * norm.n_ch + k] -= mins[k];
    Image cmy = interpolate_exposure_to_density(log_raw, norm.total, le);
    cmy = apply_dir_couplers(cmy, log_raw, st.pixel_size_um, le, norm.total, p_.film_render.dir_couplers,
                             film_.info.type);
    GrainOptions go;
    go.seed = opt_.seed;
    go.use_fast_stats = p_.settings.use_fast_stats;
    go.pixel_param_scale = st.pixel_param_scale;
    return apply_grain(cmy, st.pixel_size_um, p_.film_render.grain, norm, film_.info.type, go);
}

Image Pipeline::printing_expose(const Image& cmy, State& st) const {
    int fc = film_.n_ch, pc = print_.n_ch;
    if (cmy.c != fc) throw std::runtime_error("film density image has the wrong channel count");
    Vec dmin = match_channels(p_.film_render.grain.density_min, fc);
    Vec data_min(fc), data_max = film_.curves.column_max();
    for (int k = 0; k < fc; ++k) data_min[k] = -dmin[k];
    st.log_raw_print_black.assign(pc, 0.0);
    st.log_raw_print_white.assign(pc, 0.0);
    film_cmy_to_print_log_raw(data_min.data(), st.log_raw_print_black.data());
    film_cmy_to_print_log_raw(data_max.data(), st.log_raw_print_white.data());

    PixelFn fn = [this](const double* in, double* out) { film_cmy_to_print_log_raw(in, out); };
    Image log_raw;
    if (p_.settings.use_enlarger_lut && fc == 1) {
        log_raw = compute_with_lut_1d(cmy, fn, pc, data_min[0], data_max[0]);
    } else if (p_.settings.use_enlarger_lut && fc == 3) {
        Lut3D lut = create_lut_3d(fn, pc, data_min, data_max, p_.settings.lut_resolution);
        log_raw = apply_lut_3d(lut, cmy);
    } else {
        log_raw = Image(cmy.h, cmy.w, pc);
        parallel_for(static_cast<long>(cmy.pixels()), [&](long a, long b) {
            for (long i = a; i < b; ++i)
                film_cmy_to_print_log_raw(&cmy.v[static_cast<size_t>(i) * fc], &log_raw.v[static_cast<size_t>(i) * pc]);
        }, 256);
    }
    double corr = bw_printing_exposure_correction(st);
    Image raw(log_raw.h, log_raw.w, pc);
    for (size_t i = 0; i < raw.v.size(); ++i)
        raw.v[i] = std::pow(10.0, log_raw.v[i]) * p_.enlarger.print_exposure * corr;
    if (p_.enlarger.diffusion_filter.active && !st.have_pixel_size) throw std::runtime_error("pixel size is unknown");
    raw = apply_diffusion_filter_um(raw, p_.enlarger.diffusion_filter, st.pixel_size_um);
    for (auto& v : raw.v) v = std::log10(std::fmax(v, 0.0) + 1e-10);
    return raw;
}

Image Pipeline::printing_develop(const Image& log_raw, State&) const {
    return interpolate_exposure_to_density(log_raw, print_curves_morphed_, cd_->log_exposure());
}

Image Pipeline::scan(const Image& density, State& st) const {
    int c = scan_n_ch_;
    if (density.c != c) throw std::runtime_error("density image has the wrong channel count");
    bool scan_film = p_.io.scan_film;
    Vec dmin, dmax;
    if (scan_film) {
        Vec m = match_channels(p_.film_render.grain.density_min, c);
        dmin.resize(c);
        for (int k = 0; k < c; ++k) dmin[k] = -m[k];
        dmax = film_.curves.column_max();
    } else {
        dmin = print_.curves.column_min();
        dmax = print_.curves.column_max();
    }
    PixelFn fn = [this](const double* in, double* out) { cmy_to_log_xyz(in, out); };
    Image log_xyz;
    if (p_.settings.use_scanner_lut && c == 1) {
        log_xyz = compute_with_lut_1d(density, fn, 3, dmin[0], dmax[0]);
    } else if (p_.settings.use_scanner_lut && c == 3) {
        Lut3D lut = create_lut_3d(fn, 3, dmin, dmax, p_.settings.lut_resolution);
        log_xyz = apply_lut_3d(lut, density);
    } else {
        log_xyz = Image(density.h, density.w, 3);
        parallel_for(static_cast<long>(density.pixels()), [&](long a, long b) {
            for (long i = a; i < b; ++i)
                cmy_to_log_xyz(&density.v[static_cast<size_t>(i) * c], &log_xyz.v[static_cast<size_t>(i) * 3]);
        }, 256);
    }
    Image xyz(log_xyz.h, log_xyz.w, 3);
    for (size_t i = 0; i < xyz.v.size(); ++i) xyz.v[i] = std::pow(10.0, log_xyz.v[i]);

    // black / white correction on Y
    bool bc = p_.scanner.black_correction, wc = p_.scanner.white_correction;
    if ((bc || wc) && !(scan_film && film_.info.type == "negative")) {
        if (!st.have_bw_refs)
            throw std::runtime_error("black/white references are unavailable for this route; run the "
                                     "pipeline from an earlier tap");
        double m, q;
        bool active;
        correction_function(st, m, q, active);
        for (size_t i = 0; i < xyz.pixels(); ++i) {
            double y = xyz.v[i * 3 + 1];
            double yc = std::clamp(m * y + q, 0.0, 1.0);
            double s = yc / (y + 1e-10);
            for (int k = 0; k < 3; ++k) xyz.v[i * 3 + k] *= s;
        }
    }
    if (!scan_film) add_glare(xyz, scan_illuminant_xyz_, p_.print_render.glare, opt_.seed, st.pixel_param_scale);

    Image rgb(xyz.h, xyz.w, 3);
    parallel_for(static_cast<long>(xyz.pixels()), [&](long a, long b) {
        for (long i = a; i < b; ++i) {
            const double* x = &xyz.v[static_cast<size_t>(i) * 3];
            Vec3 o = compressor_->apply(mat3_apply(xyz_to_rgb_, {x[0], x[1], x[2]}));
            double* r = &rgb.v[static_cast<size_t>(i) * 3];
            r[0] = o[0];
            r[1] = o[1];
            r[2] = o[2];
        }
    }, 256);

    double s = st.pixel_param_scale;
    if (p_.scanner.lens_blur * s > 0) rgb = gaussian_filter(rgb, p_.scanner.lens_blur * s);
    double usm_sigma = p_.scanner.unsharp_mask[0] * s, usm_amount = p_.scanner.unsharp_mask[1];
    if (usm_sigma > 0 && usm_amount > 0) rgb = unsharp_mask(rgb, usm_sigma, usm_amount);

    if (p_.io.output_cctf_encoding) {
        const ColourSpace& cs = cd_->space(p_.io.output_color_space);
        Mat3 M = cd_->rgb_to_rgb(cs, cs);
        parallel_for(static_cast<long>(rgb.pixels()), [&](long a, long b) {
            for (long i = a; i < b; ++i) {
                double* r = &rgb.v[static_cast<size_t>(i) * 3];
                Vec3 o = mat3_apply(M, {r[0], r[1], r[2]});
                for (int k = 0; k < 3; ++k) r[k] = cctf_encode(cs.cctf, o[k]);
            }
        }, 1024);
    }
    return rgb;
}

Image Pipeline::process(const Image& image, Tap inject, Tap collect, RenderStats* stats) const {
    if (static_cast<int>(collect) <= static_cast<int>(inject) && collect != inject)
        throw std::runtime_error("no node path reaches tap " + tap_to_string(collect) + " from " +
                                 tap_to_string(inject));
    State st;
    if (inject != Tap::RgbIn) {
        double long_edge = opt_.full_long_edge_px ? *opt_.full_long_edge_px : std::max(image.h, image.w);
        st.pixel_size_um = p_.camera.film_format_mm * 1000 / long_edge;
        st.have_pixel_size = true;
    }
    auto finish_scale = [&] {
        st.pixel_param_scale = opt_.match_python ? 1.0 : opt_.reference_pixel_um / st.pixel_size_um;
    };
    finish_scale();
    Image cur = image;
    Tap at = inject;
    bool scan_film = p_.io.scan_film;
    while (at != collect) {
        switch (at) {
            case Tap::RgbIn:
                cur = preprocess(cur, st);
                finish_scale();
                at = Tap::RgbPre;
                break;
            case Tap::RgbPre:
                cur = filming_expose(cur, st);
                at = Tap::LogEFilm;
                break;
            case Tap::LogEFilm:
                cur = filming_develop(cur, st);
                at = Tap::CmyFilm;
                break;
            case Tap::CmyFilm:
                if (scan_film) {
                    if (collect != Tap::RgbOut)
                        throw std::runtime_error("the scan-film route has no print taps");
                    cur = scan(cur, st);
                    at = Tap::RgbOut;
                } else {
                    cur = printing_expose(cur, st);
                    at = Tap::LogEPrint;
                }
                break;
            case Tap::LogEPrint:
                cur = printing_develop(cur, st);
                at = Tap::CmyPrint;
                break;
            case Tap::CmyPrint:
                cur = scan(cur, st);
                at = Tap::RgbOut;
                break;
            case Tap::RgbOut:
                throw std::runtime_error("cannot run past rgb_out");
        }
    }
    st.stats.pixel_size_um = st.pixel_size_um;
    if (stats) *stats = st.stats;
    return cur;
}

Image simulate(std::shared_ptr<const ColourData> colour, const Image& image, const Params& params,
               const RenderOptions& options) {
    Params digested = digest_params(colour->pack(), params);
    Pipeline pipe(colour, digested, options);
    return pipe.process(image);
}

}  // namespace spectra
