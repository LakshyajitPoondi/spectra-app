// SPDX-License-Identifier: GPL-3.0-or-later
#include "spectra/data/pack.hpp"

#include <cstring>
#include <algorithm>
#include <string>
#include <vector>
#include <fstream>
#include <stdexcept>

namespace spectra {

using nlohmann::json;

std::shared_ptr<const Pack> Pack::load(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open data pack: " + path);
    char magic[4];
    uint32_t version = 0;
    uint64_t json_len = 0, bin_len = 0;
    f.read(magic, 4);
    f.read(reinterpret_cast<char*>(&version), 4);
    f.read(reinterpret_cast<char*>(&json_len), 8);
    if (!f || std::memcmp(magic, "SFPK", 4) != 0) throw std::runtime_error("not a .sfpk pack: " + path);
    if (version != 1) throw std::runtime_error("unsupported pack version " + std::to_string(version));
    std::string js(json_len, '\0');
    f.read(js.data(), static_cast<std::streamsize>(json_len));
    f.read(reinterpret_cast<char*>(&bin_len), 8);
    auto pack = std::make_shared<Pack>();
    pack->path_ = path;
    pack->bin_.resize(bin_len);
    f.read(reinterpret_cast<char*>(pack->bin_.data()), static_cast<std::streamsize>(bin_len));
    if (!f) throw std::runtime_error("truncated data pack: " + path);
    json doc = json::parse(js);
    pack->meta_ = doc.at("meta");
    for (auto& [name, e] : doc.at("arrays").items()) {
        Entry en;
        en.dtype = e.at("dtype").get<std::string>();
        en.shape = e.at("shape").get<std::vector<size_t>>();
        en.offset = e.at("offset").get<size_t>();
        en.nbytes = e.at("nbytes").get<size_t>();
        en.crc = e.at("crc32").get<uint32_t>();
        if (en.offset + en.nbytes > bin_len) throw std::runtime_error("pack array out of range: " + name);
        pack->entries_[name] = en;
    }
    return pack;
}

bool Pack::has(const std::string& name) const { return entries_.count(name) != 0; }

std::vector<std::string> Pack::array_names() const {
    std::vector<std::string> n;
    for (auto& kv : entries_) n.push_back(kv.first);
    return n;
}

std::string Pack::dtype(const std::string& name) const { return entries_.at(name).dtype; }

std::vector<uint8_t> Pack::raw_bytes(const std::string& name) const {
    const Entry& e = entries_.at(name);
    return std::vector<uint8_t>(bin_.begin() + static_cast<long>(e.offset),
                                bin_.begin() + static_cast<long>(e.offset + e.nbytes));
}

PackArray Pack::array(const std::string& name) const {
    auto it = entries_.find(name);
    if (it == entries_.end()) throw std::runtime_error("data pack has no array '" + name + "'");
    const Entry& e = it->second;
    PackArray a;
    a.shape = e.shape;
    size_t n = 1;
    for (size_t d : e.shape) n *= d;
    a.data.resize(n);
    const uint8_t* p = bin_.data() + e.offset;
    if (e.dtype == "float64") {
        std::memcpy(a.data.data(), p, n * 8);
    } else if (e.dtype == "float32") {
        for (size_t i = 0; i < n; ++i) {
            float v;
            std::memcpy(&v, p + i * 4, 4);
            a.data[i] = v;
        }
    } else if (e.dtype == "float16") {
        for (size_t i = 0; i < n; ++i) {
            uint16_t v;
            std::memcpy(&v, p + i * 2, 2);
            a.data[i] = half_to_double(v);
        }
    } else if (e.dtype == "int32") {
        for (size_t i = 0; i < n; ++i) {
            int32_t v;
            std::memcpy(&v, p + i * 4, 4);
            a.data[i] = v;
        }
    } else {
        throw std::runtime_error("unsupported pack dtype " + e.dtype);
    }
    return a;
}

Mat3 Pack::mat3(const std::string& name) const {
    PackArray a = array(name);
    if (a.data.size() != 9) throw std::runtime_error("array '" + name + "' is not 3x3");
    Mat3 m;
    std::copy(a.data.begin(), a.data.end(), m.begin());
    return m;
}

std::vector<std::string> Pack::verify() const {
    std::vector<std::string> bad;
    for (auto& [name, e] : entries_)
        if (crc32(bin_.data() + e.offset, e.nbytes) != e.crc) bad.push_back(name);
    return bad;
}

std::vector<std::string> list_profiles(const Pack& pack) {
    std::vector<std::string> out;
    for (auto& kv : pack.meta().at("profiles").items()) out.push_back(kv.key());
    return out;
}

Profile load_profile(const Pack& pack, const std::string& stock) {
    const json& profiles = pack.meta().at("profiles");
    if (!profiles.contains(stock)) throw std::runtime_error("unknown profile '" + stock + "'");
    const json& j = profiles.at(stock);
    Profile p;
    p.stock = j.at("stock").get<std::string>();
    p.name = j.value("name", stock);
    p.type = j.at("type").get<std::string>();
    p.support = j.at("support").get<std::string>();
    p.stage = j.at("stage").get<std::string>();
    p.use = j.at("use").get<std::string>();
    p.antihalation = j.at("antihalation").get<std::string>();
    p.channel_model = j.at("channel_model").get<std::string>();
    p.densitometer = j.value("densitometer", "");
    p.reference_illuminant = j.at("reference_illuminant").get<std::string>();
    p.viewing_illuminant = j.at("viewing_illuminant").get<std::string>();
    if (!j.at("target_print").is_null()) p.target_print = j.at("target_print").get<std::string>();
    std::string base = "profiles/" + stock + "/";
    p.log_sensitivity = pack.vec(base + "log_sensitivity");
    p.channel_density = pack.vec(base + "channel_density");
    PackArray bd = pack.array(base + "base_density");
    p.base_density = bd.data;
    p.base_density_cols = bd.shape.size() == 2 ? static_cast<int>(bd.shape[1]) : 1;
    p.log_exposure = pack.vec(base + "log_exposure");
    if (pack.has(base + "development_time")) p.development_time = pack.vec(base + "development_time");
    if (pack.has(base + "hanatos2025_adaptation_window_params"))
        p.window_params = pack.vec(base + "hanatos2025_adaptation_window_params");
    if (pack.has(base + "hanatos2025_adaptation_surface_params"))
        p.surface_params = pack.vec(base + "hanatos2025_adaptation_surface_params");
    PackArray c = pack.array(base + "model_centers");
    p.model.model_type = j.at("model_type").get<std::string>();
    p.model.n_rows = static_cast<int>(c.shape.at(0));
    p.model.n_layers = static_cast<int>(c.shape.at(1));
    p.model.centers = c.data;
    p.model.amplitudes = pack.vec(base + "model_amplitudes");
    p.model.sigmas = pack.vec(base + "model_sigmas");
    if (j.value("model_has_alphas", false)) p.model.alphas = pack.vec(base + "model_alphas");
    return p;
}

}  // namespace spectra
