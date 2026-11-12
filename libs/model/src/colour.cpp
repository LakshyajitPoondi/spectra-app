// SPDX-License-Identifier: GPL-3.0-or-later
#include "spectra/model/colour.hpp"

#include <cmath>
#include <string>
#include <stdexcept>

namespace spectra {

namespace {
double spow(double a, double p) {
    if (std::isnan(a)) return a;
    double s = a > 0 ? 1.0 : (a < 0 ? -1.0 : 0.0);
    return s * std::pow(std::fabs(a), p);
}
}  // namespace

double cctf_decode(const std::string& id, double v) {
    if (id == "linear") return v;
    if (id == "srgb") {
        const double thr = 0.0031308 * 12.92;
        return thr >= v ? v / 12.92 : spow((v + 0.055) / 1.055, 2.4);
    }
    if (id == "romm") {
        const double i_max = 255.0;
        const double e_t = std::pow(16.0, 1.8 / (1 - 1.8));
        double xp = v * i_max;
        return xp < 16 * e_t * i_max ? xp / (16 * i_max) : spow(xp / i_max, 1.8);
    }
    if (id == "bt2020") {
        const double a = 1.099, b = 0.018;
        const double thr_exact = a * spow(b, 0.45) - (a - 1);
        return v < thr_exact ? v / 4.5 : spow((v + (a - 1)) / a, 1 / 0.45);
    }
    if (id.rfind("gamma:", 0) == 0) return std::pow(v, std::stod(id.substr(6)));
    throw std::runtime_error("unknown CCTF '" + id + "'");
}

double cctf_encode(const std::string& id, double v) {
    if (id == "linear") return v;
    if (id == "srgb") return v <= 0.0031308 ? v * 12.92 : 1.055 * spow(v, 1 / 2.4) - 0.055;
    if (id == "romm") {
        const double i_max = 255.0;
        const double e_t = std::pow(16.0, 1.8 / (1 - 1.8));
        double xp = e_t > v ? v * 16 * i_max : spow(v, 1 / 1.8) * i_max;
        return xp / i_max;
    }
    if (id == "bt2020") {
        const double a = 1.099, b = 0.018;
        return b > v ? v * 4.5 : a * spow(v, 0.45) - (a - 1);
    }
    if (id.rfind("gamma:", 0) == 0) return std::pow(v, 1.0 / std::stod(id.substr(6)));
    throw std::runtime_error("unknown CCTF '" + id + "'");
}

Vec3 xy_to_XYZ(const Xy& xy) {
    double x = xy[0], y = xy[1];
    if (y == 0) return {0, 0, 0};
    return {x * 1.0 / y, 1.0, (1 - x - y) * 1.0 / y};
}

Mat3 von_kries_matrix(const Vec3& xyz_w, const Vec3& xyz_wr, const Mat3& m) {
    Vec3 rgb_w = mat3_apply(m, xyz_w), rgb_wr = mat3_apply(m, xyz_wr);
    Mat3 d{};
    for (int i = 0; i < 3; ++i) d[i * 3 + i] = rgb_w[i] == 0 ? 0.0 : rgb_wr[i] / rgb_w[i];
    return mat3_mul(mat3_mul(mat3_inv(m), d), m);
}

ColourData::ColourData(std::shared_ptr<const Pack> pack) : pack_(std::move(pack)) {
    wavelengths_ = pack_->vec("spectral/wavelengths");
    if (static_cast<int>(wavelengths_.size()) != kNumWavelengths)
        throw std::runtime_error("data pack: unexpected wavelength grid");
    cmfs_ = pack_->vec("spectral/cmfs_xyz");
    log_exposure_ = pack_->vec("spectral/log_exposure");
    mallett_ = pack_->vec("spectral/mallett2019_basis");
    locus_ = pack_->vec("spectral/locus_xy");
    const auto& meta = pack_->meta();
    planck_c2_ = meta.at("planck").at("c2").get<double>();
    planck_n_ = meta.at("planck").at("n").get<double>();
    for (auto& [name, j] : meta.at("colourspaces").items()) {
        ColourSpace cs;
        cs.name = name;
        cs.rgb_to_xyz = pack_->mat3("colourspaces/" + name + "/rgb_to_xyz");
        cs.xyz_to_rgb = pack_->mat3("colourspaces/" + name + "/xyz_to_rgb");
        Vec w = pack_->vec("colourspaces/" + name + "/whitepoint_xy");
        cs.white_xy = {w[0], w[1]};
        cs.cctf = j.at("cctf").get<std::string>();
        spaces_[name] = cs;
    }
    for (const char* c : {"CAT02", "CAT16"}) cats_[c] = pack_->mat3(std::string("cat/") + c);
    oklab_m1 = pack_->mat3("oklab/m1_xyz_to_lms");
    oklab_m2 = pack_->mat3("oklab/m2_lms_to_lab");
    oklab_m1_inv = pack_->mat3("oklab/m1_lms_to_xyz");
    oklab_m2_inv = pack_->mat3("oklab/m2_lab_to_lms");
}

Vec ColourData::illuminant(const std::string& name) const {
    if (pack_->has("illuminants/" + name)) return pack_->vec("illuminants/" + name);
    if (name.rfind("BB", 0) == 0) {
        double t = std::stod(name.substr(2));
        Vec v(kNumWavelengths);
        double sum = 0;
        for (int i = 0; i < kNumWavelengths; ++i) {
            double l = wavelengths_[i] * 1e-9;
            // Planck's law; the c1 prefactor cancels in the normalisation below.
            v[i] = std::pow(l, -5.0) / std::expm1(planck_c2_ / (planck_n_ * l * t));
            sum += v[i];
        }
        double norm = sum / kNumWavelengths;
        for (auto& x : v) x /= norm;
        return v;
    }
    throw std::runtime_error("unknown illuminant '" + name + "'");
}

Xy ColourData::illuminant_xy(const std::string& name) const {
    Vec illu = illuminant(name);
    double xyz[3] = {0, 0, 0};
    for (int k = 0; k < 3; ++k)
        for (int i = 0; i < kNumWavelengths; ++i) xyz[k] += illu[i] * cmfs_[i * 3 + k];
    double s = xyz[0] + xyz[1] + xyz[2];
    return {xyz[0] / s, xyz[1] / s};
}

const ColourSpace& ColourData::space(const std::string& name) const {
    auto it = spaces_.find(name);
    if (it == spaces_.end()) throw std::runtime_error("unknown colour space '" + name + "'");
    return it->second;
}

const Mat3& ColourData::cat(const std::string& name) const {
    auto it = cats_.find(name);
    if (it == cats_.end()) throw std::runtime_error("unknown adaptation transform '" + name + "'");
    return it->second;
}

Mat3 ColourData::rgb_to_xyz(const ColourSpace& cs, const Xy* illum, const std::string& catn) const {
    Xy target = illum ? *illum : cs.white_xy;
    Mat3 mcat = von_kries_matrix(xy_to_XYZ(cs.white_xy), xy_to_XYZ(target), cat(catn));
    return mat3_mul(mcat, cs.rgb_to_xyz);
}

Mat3 ColourData::xyz_to_rgb(const ColourSpace& cs, const Xy* illum, const std::string& catn) const {
    Xy source = illum ? *illum : cs.white_xy;
    Mat3 mcat = von_kries_matrix(xy_to_XYZ(source), xy_to_XYZ(cs.white_xy), cat(catn));
    return mat3_mul(cs.xyz_to_rgb, mcat);
}

Mat3 ColourData::rgb_to_rgb(const ColourSpace& in, const ColourSpace& out) const {
    Mat3 mcat = von_kries_matrix(xy_to_XYZ(in.white_xy), xy_to_XYZ(out.white_xy), cat("CAT02"));
    return mat3_mul(out.xyz_to_rgb, mat3_mul(mcat, in.rgb_to_xyz));
}

const Vec& ColourData::spectra_lut(int& size) const {
    std::lock_guard<std::mutex> lock(mu_);
    if (spectra_lut_.empty()) {
        PackArray a = pack_->array("upsampling/hanatos2025_spectra_lut");
        spectra_lut_size_ = static_cast<int>(a.shape.at(0));
        spectra_lut_ = std::move(a.data);
    }
    size = spectra_lut_size_;
    return spectra_lut_;
}

Vec ColourData::dichroic(const std::string& brand) const {
    return pack_->vec("filters/dichroic/" + brand);
}

Vec ColourData::camera_filter(const std::string& key) const {
    if (key == "none") return {};
    return pack_->vec("filters/camera/" + key);
}

Vec3 xyz_to_oklab(const ColourData& cd, const Vec3& xyz) {
    Vec3 lms = mat3_apply(cd.oklab_m1, xyz);
    for (auto& v : lms) v = spow(v, 1.0 / 3.0);
    return mat3_apply(cd.oklab_m2, lms);
}

Vec3 oklab_to_xyz(const ColourData& cd, const Vec3& lab) {
    Vec3 lms = mat3_apply(cd.oklab_m2_inv, lab);
    for (auto& v : lms) v = spow(v, 3.0);
    return mat3_apply(cd.oklab_m1_inv, lms);
}

}  // namespace spectra
