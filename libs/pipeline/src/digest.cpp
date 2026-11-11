// SPDX-License-Identifier: GPL-3.0-or-later
#include <cctype>

#include "spectra/pipeline/pipeline.hpp"

namespace spectra {

using nlohmann::json;

namespace {

// Assign a preset array to a per-channel field. Null entries (channels a B&W
// preset omits) leave the existing value, which a single-channel film never reads.
template <size_t N>
void set_array(std::array<double, N>& dst, const json& v) {
    for (size_t i = 0; i < N && i < v.size(); ++i)
        if (!v[i].is_null()) dst[i] = v[i].get<double>();
}

bool set_grain_field(GrainParams& g, const std::string& key, const json& v) {
    if (key == "rms_granularity") set_array(g.rms_granularity, v);
    else if (key == "density_min") set_array(g.density_min, v);
    else if (key == "uniformity") set_array(g.uniformity, v);
    else if (key == "particle_scale_sublayers") set_array(g.particle_scale_sublayers, v);
    else if (key == "micro_structure") set_array(g.micro_structure, v);
    else if (key == "active") g.active = v.get<bool>();
    else if (key == "blur") g.blur = v.get<double>();
    else if (key == "mult_usm_sigma") g.mult_usm_sigma = v.get<double>();
    else if (key == "mult_usm_amount") g.mult_usm_amount = v.get<double>();
    else if (key == "blur_dye_clouds_um") g.blur_dye_clouds_um = v.get<double>();
    else if (key == "micro_sublayers") g.micro_sublayers = v.get<int>();
    else return false;
    return true;
}

bool set_coupler_field(DirCouplersParams& d, const std::string& key, const json& v) {
    if (key == "gamma_samelayer_rgb") set_array(d.gamma_samelayer_rgb, v);
    else if (key == "gamma_interlayer_r_to_gb") set_array(d.gamma_interlayer_r_to_gb, v);
    else if (key == "gamma_interlayer_g_to_rb") set_array(d.gamma_interlayer_g_to_rb, v);
    else if (key == "gamma_interlayer_b_to_rg") set_array(d.gamma_interlayer_b_to_rg, v);
    else if (key == "langmuir_donor_k_rgb") set_array(d.langmuir_donor_k_rgb, v);
    else if (key == "langmuir_receiver_k_rgb") set_array(d.langmuir_receiver_k_rgb, v);
    else if (key == "active") d.active = v.get<bool>();
    else if (key == "amount") d.amount = v.get<double>();
    else if (key == "inhibition_samelayer") d.inhibition_samelayer = v.get<double>();
    else if (key == "inhibition_interlayer") d.inhibition_interlayer = v.get<double>();
    else if (key == "diffusion_size_um") d.diffusion_size_um = v.get<double>();
    else if (key == "diffusion_tail_um") d.diffusion_tail_um = v.get<double>();
    else if (key == "diffusion_tail_weight") d.diffusion_tail_weight = v.get<double>();
    else return false;
    return true;
}

bool is_coupler_field(const std::string& key) {
    DirCouplersParams tmp;
    static const json dummy3 = json::array({0.0, 0.0, 0.0});
    if (key == "active") return true;
    if (key == "amount" || key == "inhibition_samelayer" || key == "inhibition_interlayer" ||
        key == "diffusion_size_um" || key == "diffusion_tail_um" || key == "diffusion_tail_weight")
        return true;
    return key.rfind("gamma_", 0) == 0 || key.rfind("langmuir_", 0) == 0;
}

const json& sub(const json& j, const std::string& key) {
    static const json empty = json::object();
    return j.is_object() && j.contains(key) && j.at(key).is_object() ? j.at(key) : empty;
}

void apply_halation_preset(Params& p, const Profile& film) {
    if (!film.is_film()) return;
    double sigma;
    if (film.use == "still") sigma = 65.0;
    else if (film.use == "cine") sigma = 50.0;
    else return;
    Vec3 strength;
    if (film.antihalation == "strong") strength = {0.015, 0.005, 0.0};
    else if (film.antihalation == "weak") strength = {0.08, 0.02, 0.0};
    else if (film.antihalation == "no") strength = {0.30, 0.10, 0.015};
    else return;
    p.film_render.halation.halation_first_sigma_um = {sigma, sigma, sigma};
    p.film_render.halation.halation_strength = strength;
}

void apply_grain_preset(Params& p, const Profile& film, const json& presets) {
    if (!presets.contains(film.stock)) return;
    const json& preset = presets.at(film.stock);
    const json& defaults = sub(sub(sub(presets, "defaults"), film.is_bw() ? "bw" : "color"),
                               film.is_positive() ? "positive" : "negative");
    GrainParams& g = p.film_render.grain;
    for (auto it = defaults.begin(); it != defaults.end(); ++it)
        set_grain_field(g, it.key(), preset.contains(it.key()) ? preset.at(it.key()) : it.value());
    for (auto it = preset.begin(); it != preset.end(); ++it) set_grain_field(g, it.key(), it.value());
}

void apply_couplers_preset(Params& p, const Profile& film, const json& presets) {
    bool bw = film.is_bw(), positive = film.is_positive();
    const json& all_defaults =
        sub(sub(sub(presets, "defaults"), bw ? "bw" : "color"), positive ? "positive" : "negative");
    json defaults = json::object();
    for (auto it = all_defaults.begin(); it != all_defaults.end(); ++it)
        if (is_coupler_field(it.key()) && !it.value().is_object()) defaults[it.key()] = it.value();
    if (!bw && !positive && film.is_cine()) {
        std::string ref = film.reference_illuminant;
        bool tungsten = !ref.empty() && std::toupper(static_cast<unsigned char>(ref[0])) == 'T';
        const json& branch = sub(sub(all_defaults, "cine"), tungsten ? "tungsten" : "daylight");
        for (auto it = branch.begin(); it != branch.end(); ++it)
            if (is_coupler_field(it.key())) defaults[it.key()] = it.value();
    }
    const json& preset = sub(presets, film.stock);
    DirCouplersParams& d = p.film_render.dir_couplers;
    for (auto it = defaults.begin(); it != defaults.end(); ++it)
        set_coupler_field(d, it.key(), preset.contains(it.key()) ? preset.at(it.key()) : it.value());
    for (auto it = preset.begin(); it != preset.end(); ++it) set_coupler_field(d, it.key(), it.value());
}

}  // namespace

Params digest_params(const Pack& pack, Params p, bool apply_stocks_specifics) {
    const json& meta = pack.meta();
    Profile film = load_profile(pack, p.film);
    Profile print = load_profile(pack, p.print);

    if (p.settings.neutral_print_filters_from_database) {
        const json& db = meta.at("neutral_print_filters");
        const json& entry = sub(sub(db, print.stock), p.enlarger.illuminant);
        double c = 0.0, m = 50.0, y = 50.0;
        if (entry.contains(film.stock) && entry.at(film.stock).is_array()) {
            const json& f = entry.at(film.stock);
            c = f[0].get<double>();
            m = f[1].get<double>();
            y = f[2].get<double>();
        }
        p.enlarger.c_filter_neutral = c;
        p.enlarger.m_filter_neutral = m;
        p.enlarger.y_filter_neutral = y;
    }

    if (p.settings.preview_mode) {
        p.enlarger.lens_blur = 0.0;
        p.film_render.dir_couplers.diffusion_size_um = 0.0;
        p.film_render.grain.active = false;
        p.film_render.grain.rms_granularity = {0.0, 0.0, 0.0};
        p.film_render.grain.blur = 0.0;
        p.film_render.grain.mult_usm_amount = 0.0;
        p.print_render.glare.blur = 0.0;
        p.camera.lens_blur_um = 0.0;
        p.scanner.lens_blur = 0.0;
        p.scanner.unsharp_mask = {0.0, 0.0};
    }

    if (apply_stocks_specifics) {
        apply_halation_preset(p, film);
        apply_grain_preset(p, film, meta.at("grain_presets"));
        apply_couplers_preset(p, film, meta.at("coupler_presets"));
    }

    if (p.debug.lut_mode) {
        p.debug.deactivate_spatial_effects = true;
        p.debug.deactivate_stochastic_effects = true;
        p.camera.auto_exposure = false;
        p.scanner.white_correction = false;
        p.scanner.black_correction = false;
    }
    if (p.debug.deactivate_spatial_effects) {
        p.film_render.halation.scatter_core_um = {0.0, 0.0, 0.0};
        p.film_render.halation.scatter_tail_um = {0.0, 0.0, 0.0};
        p.film_render.halation.halation_first_sigma_um = {0.0, 0.0, 0.0};
        p.film_render.dir_couplers.diffusion_size_um = 0;
        p.film_render.grain.blur = 0.0;
        p.film_render.grain.blur_dye_clouds_um = 0.0;
        p.film_render.grain.mult_usm_amount = 0.0;
        p.print_render.glare.blur = 0;
        p.camera.lens_blur_um = 0.0;
        p.enlarger.lens_blur = 0.0;
        p.enlarger.diffusion_filter.active = false;
        p.camera.diffusion_filter.active = false;
        p.scanner.lens_blur = 0.0;
        p.scanner.unsharp_mask = {0.0, 0.0};
    }
    if (p.debug.deactivate_stochastic_effects) {
        p.film_render.grain.active = false;
        p.print_render.glare.active = false;
    }
    return p;
}

}  // namespace spectra
