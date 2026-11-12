// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "spectra/core/image.hpp"
#include "spectra/core/math.hpp"
#include "spectra/data/pack.hpp"

namespace spectra {

inline constexpr int kNumWavelengths = 81;
using Xy = std::array<double, 2>;

struct ColourSpace {
    std::string name;
    Mat3 rgb_to_xyz{}, xyz_to_rgb{};
    Xy white_xy{};
    std::string cctf;  // "linear", "srgb", "romm", "bt2020", "gamma:<exponent>"
};

// Colour component transfer functions with colour-science semantics.
double cctf_decode(const std::string& id, double v);
double cctf_encode(const std::string& id, double v);

// xy chromaticity -> XYZ with Y = 1 (colour.xyY_to_XYZ(xy_to_xyY(xy))).
Vec3 xy_to_XYZ(const Xy& xy);
// Von Kries adaptation matrix from white XYZ_w to XYZ_wr with transform M.
Mat3 von_kries_matrix(const Vec3& xyz_w, const Vec3& xyz_wr, const Mat3& m);

// Shared colorimetric data, loaded once from the pack.
class ColourData {
public:
    explicit ColourData(std::shared_ptr<const Pack> pack);

    const Pack& pack() const { return *pack_; }
    const Vec& wavelengths() const { return wavelengths_; }
    const Vec& cmfs() const { return cmfs_; }  // [81 x 3]
    const Vec& log_exposure() const { return log_exposure_; }
    // Normalised spectral power (mean = 1) of a named illuminant: pack entries,
    // or "BB<temperature>" via Planck's law.
    Vec illuminant(const std::string& name) const;
    Xy illuminant_xy(const std::string& name) const;  // spektrafilm's _illuminant_to_xy
    const ColourSpace& space(const std::string& name) const;
    const Mat3& cat(const std::string& name) const;
    // Matrix for colour.RGB_to_XYZ(rgb, space, illuminant=xy, chromatic_adaptation_transform=cat).
    Mat3 rgb_to_xyz(const ColourSpace& cs, const Xy* illuminant_xy, const std::string& cat) const;
    // Matrix for colour.XYZ_to_RGB(xyz, space, illuminant=xy, chromatic_adaptation_transform=cat).
    Mat3 xyz_to_rgb(const ColourSpace& cs, const Xy* illuminant_xy, const std::string& cat) const;
    // Matrix for colour.RGB_to_RGB(in, out) with CAT02.
    Mat3 rgb_to_rgb(const ColourSpace& in, const ColourSpace& out) const;
    // Hanatos 2025 spectra LUT [n x n x 81] as float64 (decoded on first use).
    const Vec& spectra_lut(int& size) const;
    const Vec& mallett_basis() const { return mallett_; }  // [81 x 3]
    const Vec& locus_xy() const { return locus_; }         // [n x 2], closed polygon
    Vec dichroic(const std::string& brand) const;          // [81 x 3], C M Y columns
    Vec camera_filter(const std::string& key) const;       // [81]; empty for "none"
    Mat3 oklab_m1, oklab_m2, oklab_m1_inv, oklab_m2_inv;

private:
    std::shared_ptr<const Pack> pack_;
    Vec wavelengths_, cmfs_, log_exposure_, mallett_, locus_;
    std::map<std::string, ColourSpace> spaces_;
    std::map<std::string, Mat3> cats_;
    double planck_c2_ = 0.014388, planck_n_ = 1.0;
    mutable std::mutex mu_;
    mutable Vec spectra_lut_;
    mutable int spectra_lut_size_ = 0;
};

// Oklab conversions as in colour-science (signed cube root).
Vec3 xyz_to_oklab(const ColourData& cd, const Vec3& xyz);
Vec3 oklab_to_xyz(const ColourData& cd, const Vec3& lab);

}  // namespace spectra

namespace spectra {

// RAW white balance (port of spektrafilm.utils.raw_file_processor).
// colour.CCT_to_xy: CIE D-series locus for T >= 4000 K, Kang 2002 below.
Xy cct_to_xy(double temperature);
// Von Kries (CAT02) adaptation of linear ACES2065-1 RGB from the white of
// `scene_temperature` to the 6504 K reference, followed by the green "tint"
// multiplier. Returns the 3x3 matrix to apply to RGB (tint included).
Mat3 raw_white_balance_matrix(const ColourData& cd, double scene_temperature, double tint);

}  // namespace spectra
