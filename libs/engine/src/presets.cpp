// SPDX-License-Identifier: GPL-3.0-or-later
#include "spectra/engine/engine.hpp"

namespace spectra::engine {

using nlohmann::json;

namespace {

json diff(const json& a, const json& base) {
    if (a.is_object() && base.is_object()) {
        json out = json::object();
        for (auto it = a.begin(); it != a.end(); ++it) {
            if (!base.contains(it.key())) {
                out[it.key()] = it.value();
                continue;
            }
            json d = diff(it.value(), base.at(it.key()));
            if (!d.is_null()) out[it.key()] = d;
        }
        return out.empty() ? json() : out;
    }
    return a == base ? json() : a;
}

}  // namespace

json params_diff(const Params& p) {
    json d = diff(params_to_json(p), params_to_json(Params{}));
    return d.is_null() ? json::object() : d;
}

Params params_from_diff(const json& d) {
    Params p;
    if (d.is_object()) apply_params_patch(p, d);
    return p;
}

void apply_stock_specifics(const Pack& pack, Params& p) {
    Params d = digest_params(pack, p, true);
    p.film_render.grain = d.film_render.grain;
    p.film_render.halation = d.film_render.halation;
    p.film_render.dir_couplers = d.film_render.dir_couplers;
    p.io.scan_film = load_profile(pack, p.film).is_positive();
}

Params default_user_params(const Pack& pack, const std::string& film, const std::string& print) {
    Params p;
    p.film = film;
    p.print = print;
    apply_stock_specifics(pack, p);
    return p;
}

Params params_from_preset(const Pack& pack, const json& d) {
    Params p;
    if (d.is_object()) {
        if (d.contains("film")) p.film = d.at("film").get<std::string>();
        if (d.contains("print")) p.print = d.at("print").get<std::string>();
    }
    apply_stock_specifics(pack, p);
    if (d.is_object()) apply_params_patch(p, d);
    return p;
}

json preset_to_json(const Preset& p) {
    return json{{"format", "spectra-preset"}, {"version", 1}, {"name", p.name}, {"description", p.description},
                {"params", p.diff}};
}

Preset preset_from_json(const json& j) {
    if (j.value("format", "") != "spectra-preset") throw std::runtime_error("not a preset file");
    Preset p;
    p.name = j.value("name", "");
    p.description = j.value("description", "");
    p.diff = j.value("params", json::object());
    params_from_diff(p.diff);  // validates the patch
    return p;
}

// Starting points only: stock pairings with the reference's own defaults.
// Nothing here changes a model parameter away from the stock's profile.
std::vector<Preset> builtin_presets() {
    auto pair = [](const char* name, const char* desc, const char* film, const char* print, bool scan_film = false) {
        Preset p;
        p.name = name;
        p.description = desc;
        p.diff = json{{"film", film}, {"print", print}};
        if (scan_film) p.diff["io"]["scan_film"] = true;
        return p;
    };
    return {
        pair("Portra 400 on Portra Endura", "Colour negative printed on paper (the default).", "kodak_portra_400",
             "kodak_portra_endura"),
        pair("Gold 200 on Ektacolor Edge", "Consumer colour negative on consumer paper.", "kodak_gold_200",
             "kodak_ektacolor_edge"),
        pair("Ektar 100 on Supra Endura", "Fine-grained, saturated negative.", "kodak_ektar_100",
             "kodak_supra_endura"),
        pair("Fujifilm Pro 400H on Crystal Archive", "Fujifilm negative on Fujifilm paper.", "fujifilm_pro_400h",
             "fujifilm_crystal_archive_typeii"),
        pair("Vision3 500T on 2383", "Motion-picture negative on print film.", "kodak_vision3_500t", "kodak_2383"),
        pair("Vision3 250D on 2383", "Daylight motion-picture negative on print film.", "kodak_vision3_250d",
             "kodak_2383"),
        pair("Velvia 100 (slide)", "Reversal film, scanned directly.", "fujifilm_velvia_100", "kodak_portra_endura",
             true),
        pair("Ektachrome 100 (slide)", "Reversal film, scanned directly.", "kodak_ektachrome_100",
             "kodak_portra_endura", true),
        pair("Kodachrome 64 (slide)", "Reversal film, scanned directly.", "kodak_kodachrome_64",
             "kodak_portra_endura", true),
    };
}

}  // namespace spectra::engine
