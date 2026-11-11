// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "spectra/core/math.hpp"

namespace spectra {

// One array of the data pack, converted to float64.
struct PackArray {
    std::vector<size_t> shape;
    Vec data;
    size_t dim(size_t i) const { return shape.at(i); }
};

// Reader for the .sfpk data pack written by tools/exporter/export_pack.py.
class Pack {
public:
    static std::shared_ptr<const Pack> load(const std::string& path);

    bool has(const std::string& name) const;
    PackArray array(const std::string& name) const;   // converted to float64
    Vec vec(const std::string& name) const { return array(name).data; }
    Mat3 mat3(const std::string& name) const;
    std::vector<std::string> array_names() const;
    const nlohmann::json& meta() const { return meta_; }
    const std::string& path() const { return path_; }
    // Recompute every array's CRC32 and compare with the manifest. Returns the
    // names that fail (empty when the pack is intact).
    std::vector<std::string> verify() const;
    // Raw bytes of one array as stored (for the round-trip test).
    std::vector<uint8_t> raw_bytes(const std::string& name) const;
    std::string dtype(const std::string& name) const;

private:
    struct Entry {
        std::string dtype;
        std::vector<size_t> shape;
        size_t offset = 0, nbytes = 0;
        uint32_t crc = 0;
    };
    std::string path_;
    std::vector<uint8_t> bin_;
    std::map<std::string, Entry> entries_;
    nlohmann::json meta_;
};

// Profile data as stored on disk (before any runtime processing).
struct DensityCurvesModel {
    std::string model_type = "norm_cdfs";
    int n_rows = 0, n_layers = 0;  // rows = channels, or development times for B&W families
    Vec centers, amplitudes, sigmas, alphas;  // row-major [row][layer]; alphas may be empty
    bool has_alphas() const { return !alphas.empty(); }
};

struct Profile {
    // info
    std::string stock, name, type = "negative", support = "film", stage = "filming",
                                  use = "still", antihalation = "weak", channel_model = "color",
                                  densitometer, reference_illuminant = "D55",
                                  viewing_illuminant = "D50";
    std::optional<std::string> target_print;
    // data
    Vec log_sensitivity;   // [81 x n_ch]
    Vec channel_density;   // [81 x n_ch]
    Vec base_density;      // [81] or [81 x n_dev]
    int base_density_cols = 1;
    Vec log_exposure;      // [256]
    Vec development_time;  // empty for colour
    Vec window_params;     // empty when absent
    Vec surface_params;    // [3 x 15] or empty
    DensityCurvesModel model;

    int n_channels() const { return channel_model == "bw" ? 1 : 3; }
    bool is_positive() const { return type == "positive"; }
    bool is_negative() const { return type == "negative"; }
    bool is_film() const { return support == "film"; }
    bool is_bw() const { return channel_model == "bw"; }
    bool is_cine() const { return use == "cine"; }
};

Profile load_profile(const Pack& pack, const std::string& stock);
std::vector<std::string> list_profiles(const Pack& pack);

}  // namespace spectra
