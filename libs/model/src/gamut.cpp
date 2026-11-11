// SPDX-License-Identifier: GPL-3.0-or-later
#include "spectra/model/gamut.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "spectra/core/parallel.hpp"

namespace spectra {

namespace {

constexpr int kNL = 64, kNH = 720, kNBisect = 18;
constexpr double kOklrabK1 = 0.206, kOklrabK2 = 0.03;
const double kOklrabK3 = (1.0 + kOklrabK1) / (1.0 + kOklrabK2);
constexpr double kJzYw = 100.0;
// Safdar 2017 constants as used by colour-science.
constexpr double kJzB = 1.15, kJzG = 0.66, kJzD = -0.56, kJzD0 = 1.6295499532821565e-11;
constexpr double kJzM1 = 0.1593017578125, kJzM2 = 134.03437499999998, kJzC1 = 0.8359375,
                 kJzC2 = 18.8515625, kJzC3 = 18.6875;

double spow(double a, double p) {
    if (std::isnan(a)) return a;
    double s = a > 0 ? 1.0 : (a < 0 ? -1.0 : 0.0);
    return s * std::pow(std::fabs(a), p);
}

double ray_polygon_distance(const Xy& origin, double dx, double dy, const Vec& poly) {
    double t_min = INFINITY;
    int n = static_cast<int>(poly.size() / 2) - 1;
    for (int k = 0; k < n; ++k) {
        double ax = poly[2 * k], ay = poly[2 * k + 1];
        double ex = poly[2 * k + 2] - ax, ey = poly[2 * k + 3] - ay;
        double denom = dx * ey - dy * ex;
        if (!(std::fabs(denom) > 1e-12)) continue;
        double ox = origin[0] - ax, oy = origin[1] - ay;
        double t = (-ox * ey + oy * ex) / denom;
        double s = (-ox * dy + oy * dx) / denom;
        if (t > 1e-9 && s >= 0.0 && s <= 1.0 && t < t_min) t_min = t;
    }
    return t_min;
}

bool point_in_polygon(double x, double y, const Vec& poly) {
    int n = static_cast<int>(poly.size() / 2) - 1;  // closed polygon, last == first
    bool inside = false;
    for (int i = 0, j = n - 1; i < n; j = i++) {
        double xi = poly[2 * i], yi = poly[2 * i + 1], xj = poly[2 * j], yj = poly[2 * j + 1];
        if (((yi > y) != (yj > y)) && (x < (xj - xi) * (y - yi) / (yj - yi) + xi)) inside = !inside;
    }
    return inside;
}

Vec3 xy_to_xyz_unit_y(const Xy& xy) {
    double sy = std::fmax(xy[1], 1e-12);
    return {xy[0] / sy, 1.0, (1.0 - xy[0] - xy[1]) / sy};
}

template <class InGamut>
CMaxTable build_cmax(const Vec& l_grid, double chroma_upper, InGamut&& in_gamut) {
    CMaxTable t;
    t.l_grid = l_grid;
    t.n_l = static_cast<int>(l_grid.size());
    t.n_h = kNH;
    t.table.assign(static_cast<size_t>(t.n_l) * kNH, 0.0);
    Vec h_grid(kNH);
    double step = (kPi - (-kPi)) / kNH;
    for (int i = 0; i < kNH; ++i) h_grid[i] = -kPi + i * step;
    parallel_for(t.n_l, [&](long a, long b) {
        for (long li = a; li < b; ++li)
            for (int hi_ = 0; hi_ < kNH; ++hi_) {
                double lo = 0.0, hi = chroma_upper;
                double ch = std::cos(h_grid[hi_]), sh = std::sin(h_grid[hi_]);
                for (int it = 0; it < kNBisect; ++it) {
                    double mid = (lo + hi) * 0.5;
                    if (in_gamut(Vec3{l_grid[li], mid * ch, mid * sh}))
                        lo = mid;
                    else
                        hi = mid;
                }
                t.table[static_cast<size_t>(li) * kNH + hi_] = lo;
            }
    });
    return t;
}

}  // namespace

double reinhard_knee(double d, double threshold, double limit, double power) {
    if (!(d > threshold)) return d;
    double scale = limit - threshold;
    double x = (d - threshold) / scale;
    double y = x / std::pow(1.0 + std::pow(x, power), 1.0 / power);
    return threshold + scale * y;
}

double CMaxTable::lookup(double L, double h) const {
    double l0 = l_grid.front(), l1 = l_grid.back();
    L = std::clamp(L, l0, l1);
    double h0 = -kPi;
    double h_step = (-kPi + 1 * ((kPi - (-kPi)) / n_h)) - h0;
    double h_idx = (h - h0) / h_step;
    double fl = std::floor(h_idx);
    long hl = static_cast<long>(fl) % n_h;
    if (hl < 0) hl += n_h;
    long hh = (hl + 1) % n_h;
    double h_frac = h_idx - fl;
    double l_idx = (L - l0) / (l1 - l0) * (n_l - 1);
    long ll = std::clamp(static_cast<long>(std::floor(l_idx)), 0L, static_cast<long>(n_l - 2));
    long lh = ll + 1;
    double l_frac = l_idx - ll;
    double v00 = table[ll * n_h + hl], v01 = table[ll * n_h + hh];
    double v10 = table[lh * n_h + hl], v11 = table[lh * n_h + hh];
    return v00 * (1 - l_frac) * (1 - h_frac) + v01 * (1 - l_frac) * h_frac +
           v10 * l_frac * (1 - h_frac) + v11 * l_frac * h_frac;
}

Xy compress_xy(const ColourData& cd, const Xy& xy, const Xy& white, const InputGamutCompressSpec& spec) {
    if (!spec.active) return xy;
    double thr = spec.knee[0], lim = spec.knee[1], pw = spec.knee[2];
    const Vec& locus = cd.locus_xy();
    if (spec.algorithm == "xy") {
        double ddx = xy[0] - white[0], ddy = xy[1] - white[1];
        double dist = std::sqrt(ddx * ddx + ddy * ddy);
        if (dist < 1e-9) return xy;
        double safe = std::fmax(dist, 1e-12);
        double dx = ddx / safe, dy = ddy / safe;
        double boundary = ray_polygon_distance(white, dx, dy, locus);
        double d_norm = dist / std::fmax(boundary, 1e-12);
        double d_c = reinhard_knee(d_norm, thr, lim, pw);
        return {white[0] + dx * (d_c * boundary), white[1] + dy * (d_c * boundary)};
    }
    if (spec.algorithm == "oklch") {
        static std::mutex mu;
        static CMaxTable table;
        {
            std::lock_guard<std::mutex> lock(mu);
            if (table.table.empty())
                table = build_cmax(linspace(0.05, 1.0, kNL), 0.5, [&](const Vec3& lab) {
                    Vec3 xyz = oklab_to_xyz(cd, lab);
                    double total = std::fmax(xyz[0] + xyz[1] + xyz[2], 1e-12);
                    return point_in_polygon(xyz[0] / total, xyz[1] / total, locus);
                });
        }
        Vec3 lab = xyz_to_oklab(cd, xy_to_xyz_unit_y(xy));
        double C = std::hypot(lab[1], lab[2]), h = std::atan2(lab[2], lab[1]);
        double safe_c = std::fmax(table.lookup(lab[0], h), 1e-9);
        double c_new = reinhard_knee(C / safe_c, thr, lim, pw) * safe_c;
        Vec3 xyz = oklab_to_xyz(cd, {lab[0], c_new * std::cos(h), c_new * std::sin(h)});
        double total = std::fmax(xyz[0] + xyz[1] + xyz[2], 1e-12);
        return {xyz[0] / total, xyz[1] / total};
    }
    throw std::runtime_error("unknown input gamut algorithm '" + spec.algorithm + "'");
}

Vec3 OutputGamutCompressor::to_polar_space(const Vec3& xyz) const {
    const std::string& a = spec_.algorithm;
    if (a == "oklch" || a == "oklrab") return xyz_to_oklab(cd_, xyz);
    // jzazbz, absolute XYZ in cd/m2
    double X = xyz[0] * kJzYw, Y = xyz[1] * kJzYw, Z = xyz[2] * kJzYw;
    Vec3 p{kJzB * X - (kJzB - 1) * Z, kJzG * Y - (kJzG - 1) * X, Z};
    Vec3 lms = mat3_apply(jz_xyz_to_lms_, p);
    for (auto& v : lms) {
        double yp = spow(v / 10000.0, kJzM1);
        v = spow((kJzC1 + kJzC2 * yp) / (kJzC3 * yp + 1), kJzM2);
    }
    Vec3 iab = mat3_apply(jz_lmsp_to_iab_, lms);
    double jz = ((1 + kJzD) * iab[0]) / (1 + kJzD * iab[0]) - kJzD0;
    return {jz, iab[1], iab[2]};
}

Vec3 OutputGamutCompressor::from_polar_space(const Vec3& lab) const {
    const std::string& a = spec_.algorithm;
    if (a == "oklch" || a == "oklrab") return oklab_to_xyz(cd_, lab);
    double iz = (lab[0] + kJzD0) / (1 + kJzD - kJzD * (lab[0] + kJzD0));
    Vec3 lms = mat3_apply(jz_iab_to_lmsp_, {iz, lab[1], lab[2]});
    for (auto& v : lms) {
        double vp = spow(v, 1.0 / kJzM2);
        double n = std::isnan(vp) ? vp : std::max(0.0, vp - kJzC1);
        v = 10000.0 * spow(n / (kJzC2 - kJzC3 * vp), 1.0 / kJzM1);
    }
    Vec3 p = mat3_apply(jz_lms_to_xyz_, lms);
    double X = (p[0] + (kJzB - 1) * p[2]) / kJzB;
    double Y = (p[1] + (kJzG - 1) * X) / kJzG;
    return {X / kJzYw, Y / kJzYw, p[2] / kJzYw};
}

OutputGamutCompressor::OutputGamutCompressor(const ColourData& cd,
                                             const OutputGamutCompressSpec& spec,
                                             const std::string& output_color_space)
    : cd_(cd), spec_(spec) {
    const std::string& a = spec.algorithm;
    if (a == "off" || a == "aces_rgc") return;
    if (a == "cam16ucs")
        throw std::runtime_error("output gamut algorithm 'cam16ucs' is not implemented yet");
    if (a != "oklch" && a != "oklrab" && a != "jzazbz")
        throw std::runtime_error("unknown output gamut algorithm '" + a + "'");
    const ColourSpace& cs = cd.space(output_color_space);
    rgb_to_xyz_ = cd.rgb_to_xyz(cs, &cs.white_xy, "CAT02");
    xyz_to_rgb_ = cd.xyz_to_rgb(cs, &cs.white_xy, "CAT02");
    auto in_cube = [&](const Vec3& xyz) {
        Vec3 rgb = mat3_apply(xyz_to_rgb_, xyz);
        for (double v : rgb)
            if (!(v >= -1e-6 && v <= 1.0 + 1e-6)) return false;
        return true;
    };
    if (a == "oklch") {
        table_ = build_cmax(linspace(0.02, 1.0, kNL), 0.5,
                            [&](const Vec3& lab) { return in_cube(oklab_to_xyz(cd, lab)); });
    } else if (a == "oklrab") {
        table_ = build_cmax(linspace(0.02, 1.0, kNL), 0.5, [&](const Vec3& lab) {
            double L = (lab[0] * (lab[0] + kOklrabK1)) / (kOklrabK3 * (lab[0] + kOklrabK2));
            return in_cube(oklab_to_xyz(cd, {L, lab[1], lab[2]}));
        });
    } else {
        jz_xyz_to_lms_ = cd.pack().mat3("jzazbz/xyz_to_lms");
        jz_lms_to_xyz_ = cd.pack().mat3("jzazbz/lms_to_xyz");
        jz_lmsp_to_iab_ = cd.pack().mat3("jzazbz/lms_p_to_izazbz");
        jz_iab_to_lmsp_ = cd.pack().mat3("jzazbz/izazbz_to_lms_p");
        table_ = build_cmax(linspace(0.002, 0.18, kNL), 0.3,
                            [&](const Vec3& jab) { return in_cube(from_polar_space(jab)); });
        l_white_ = to_polar_space(xy_to_xyz_unit_y(cs.white_xy))[0];
    }
}

Vec3 OutputGamutCompressor::apply(const Vec3& rgb) const {
    const std::string& a = spec_.algorithm;
    double thr = spec_.knee[0], lim = spec_.knee[1], pw = spec_.knee[2];
    if (a == "off") return rgb;
    if (a == "aces_rgc") {
        double ach = std::max({rgb[0], rgb[1], rgb[2]});
        if (!(ach > 1e-12)) return rgb;
        Vec3 out;
        for (int k = 0; k < 3; ++k) {
            double d = (ach - rgb[k]) / ach;
            out[k] = ach * (1.0 - reinhard_knee(d, thr, lim, pw));
        }
        return out;
    }
    Vec3 lab = to_polar_space(mat3_apply(rgb_to_xyz_, rgb));
    double L = lab[0];
    if (spec_.lightness_compression) {
        const Vec3& lc = *spec_.lightness_compression;
        L = reinhard_knee(L / l_white_, lc[0], lc[1], lc[2]) * l_white_;
    }
    double l_lookup = L;
    if (a == "oklrab") {
        double t = kOklrabK3 * L - kOklrabK1;
        l_lookup = 0.5 * (t + std::sqrt(t * t + 4.0 * kOklrabK2 * kOklrabK3 * L));
    }
    double C = std::hypot(lab[1], lab[2]), h = std::atan2(lab[2], lab[1]);
    double safe_c = std::fmax(table_.lookup(l_lookup, h), 1e-9);
    double c_new = reinhard_knee(C / safe_c, thr, lim, pw) * safe_c;
    Vec3 xyz = from_polar_space({L, c_new * std::cos(h), c_new * std::sin(h)});
    return mat3_apply(xyz_to_rgb_, xyz);
}

void OutputGamutCompressor::apply(Image& rgb) const {
    if (spec_.algorithm == "off") return;
    if (rgb.c != 3) throw std::runtime_error("output gamut compression expects 3 channels");
    parallel_for(static_cast<long>(rgb.pixels()), [&](long b, long e) {
        for (long i = b; i < e; ++i) {
            double* p = &rgb.v[static_cast<size_t>(i) * 3];
            Vec3 o = apply(Vec3{p[0], p[1], p[2]});
            p[0] = o[0];
            p[1] = o[1];
            p[2] = o[2];
        }
    }, 1024);
}

}  // namespace spectra
