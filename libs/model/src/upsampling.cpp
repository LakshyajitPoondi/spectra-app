// SPDX-License-Identifier: GPL-3.0-or-later
#include "spectra/model/upsampling.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "spectra/core/parallel.hpp"
#include "spectra/model/filters.hpp"
#include "spectra/model/gamut.hpp"

namespace spectra {

namespace {

double mitchell_weight(double t) {
    const double B = 1.0 / 3.0, C = 1.0 / 3.0;
    double x = std::fabs(t);
    if (x < 1)
        return (1.0 / 6.0) * ((12 - 9 * B - 6 * C) * x * x * x + (-18 + 12 * B + 6 * C) * x * x + (6 - 2 * B));
    if (x < 2)
        return (1.0 / 6.0) * ((-B - 6 * C) * x * x * x + (6 * B + 30 * C) * x * x + (-12 * B - 48 * C) * x + (8 * B + 24 * C));
    return 0.0;
}

inline int safe_index(int idx, int L) {
    if (idx < 0) return -idx;
    if (idx >= L) return 2 * (L - 1) - idx;
    return idx;
}

inline void base_fraction(double coord, int L, int& base, double& frac) {
    if (coord <= 0.0) coord = 0.0;
    double upper = L - 1;
    if (coord >= upper) {
        base = L - 2;
        frac = 1.0;
        return;
    }
    base = static_cast<int>(std::floor(coord));
    frac = coord - base;
}

double nan_to_num(double v) {
    if (std::isnan(v)) return 0.0;
    if (std::isinf(v)) return v > 0 ? 1.7976931348623157e308 : -1.7976931348623157e308;
    return v;
}

}  // namespace

Xy tri2quad(const Xy& tc) {
    double tx = tc[0], ty = tc[1];
    double y = ty / std::fmax(1.0 - tx, 1e-10);
    double x = (1.0 - tx) * (1.0 - tx);
    return {std::clamp(x, 0.0, 1.0), std::clamp(y, 0.0, 1.0)};
}

Xy quad2tri(const Xy& xy) {
    double s = std::sqrt(xy[0]);
    return {1 - s, xy[1] * s};
}

void sample_tc_lut(const TcLut& lut, double tc0, double tc1, double* out) {
    int L = lut.size, C = lut.n_ch;
    double x = tc0 * (L - 1), y = tc1 * (L - 1);
    int xb, yb;
    double xf, yf;
    base_fraction(x, L, xb, xf);
    base_fraction(y, L, yb, yf);
    double wx[4] = {mitchell_weight(xf + 1), mitchell_weight(xf), mitchell_weight(xf - 1), mitchell_weight(xf - 2)};
    double wy[4] = {mitchell_weight(yf + 1), mitchell_weight(yf), mitchell_weight(yf - 1), mitchell_weight(yf - 2)};
    double acc[4] = {0, 0, 0, 0};
    double wsum = 0.0;
    for (int i = 0; i < 4; ++i) {
        int xi = safe_index(xb - 1 + i, L);
        for (int j = 0; j < 4; ++j) {
            int yj = safe_index(yb - 1 + j, L);
            double w = wx[i] * wy[j];
            wsum += w;
            const double* p = &lut.v[(static_cast<size_t>(xi) * L + yj) * C];
            for (int c = 0; c < C; ++c) acc[c] += w * p[c];
        }
    }
    for (int c = 0; c < C; ++c) out[c] = wsum != 0.0 ? acc[c] / wsum : acc[c];
}

TcLut compute_tc_lut(const ColourData& cd, const Vec& sensitivity, int n_ch,
                     const HanatosAdaptation& ad, const InputGamutCompressSpec& gamut) {
    int S = 0;
    const Vec* spectra = &cd.spectra_lut(S);
    const int NL = kNumWavelengths;
    Vec blurred;
    if (ad.spectral_gaussian_blur > 0) {
        blurred.resize(spectra->size());
        parallel_for(static_cast<long>(S) * S, [&](long a, long b) {
            for (long i = a; i < b; ++i)
                scipy_gaussian_filter1d(spectra->data() + static_cast<size_t>(i) * NL,
                                        blurred.data() + static_cast<size_t>(i) * NL, NL, 1,
                                        ad.spectral_gaussian_blur, 0, 4.0);
        }, 256);
        spectra = &blurred;
    }
    // weight[l, m] = sensitivity x normalised window
    Vec weight(sensitivity);
    if (ad.apply_window && !ad.window_params.empty()) {
        if (ad.window_params.size() != 4)
            throw std::runtime_error("only the erf4 adaptation window is supported");
        const Vec& wl = cd.wavelengths();
        double c_uv = ad.window_params[0], s_uv = ad.window_params[1], c_ir = ad.window_params[2],
               s_ir = ad.window_params[3];
        const double sqrt2 = std::sqrt(2.0);
        Vec window(NL);
        for (int l = 0; l < NL; ++l) {
            double e_uv = 0.5 * (1.0 + std::erf((wl[l] - c_uv) / (s_uv * sqrt2)));
            double e_ir = 0.5 * (1.0 - std::erf((wl[l] - c_ir) / (s_ir * sqrt2)));
            window[l] = e_uv * e_ir;
        }
        Vec illum = cd.illuminant(ad.reference_illuminant);
        for (int m = 0; m < n_ch; ++m) {
            double num = 0, den = 0;
            for (int l = 0; l < NL; ++l) {
                num += sensitivity[l * n_ch + m] * illum[l] * window[l];
                den += sensitivity[l * n_ch + m] * illum[l];
            }
            double norm = num / den;
            for (int l = 0; l < NL; ++l) weight[l * n_ch + m] = sensitivity[l * n_ch + m] * (window[l] / norm);
        }
    }
    TcLut lut;
    lut.size = S;
    lut.n_ch = n_ch;
    lut.v.assign(static_cast<size_t>(S) * S * n_ch, 0.0);
    parallel_for(static_cast<long>(S) * S, [&](long a, long b) {
        for (long i = a; i < b; ++i) {
            const double* sp = spectra->data() + static_cast<size_t>(i) * NL;
            for (int m = 0; m < n_ch; ++m) {
                double s = 0;
                for (int l = 0; l < NL; ++l) s += sp[l] * weight[l * n_ch + m];
                lut.v[static_cast<size_t>(i) * n_ch + m] = s;
            }
        }
    }, 256);

    Xy illu_xy = cd.illuminant_xy(ad.reference_illuminant);
    if (ad.apply_surface && !ad.surface_params.empty()) {
        if (n_ch != 3 || ad.surface_params.size() != 45)
            throw std::runtime_error("adaptation surface requires a 3-channel film and 3x15 parameters");
        Xy center = tri2quad(illu_xy);
        Vec base = linspace(0, 1, S);
        for (int i = 0; i < S; ++i)
            for (int j = 0; j < S; ++j) {
                double x = base[i] - center[0], y = base[j] - center[1];
                double x2 = x * x, y2 = y * y, xy = x * y, x3 = x2 * x, y3 = y2 * y;
                for (int m = 0; m < 3; ++m) {
                    const double* c = &ad.surface_params[m * 15];
                    double raw = c[1] * x + c[2] * y + c[3] * x2 + c[4] * y2 + c[5] * xy + c[6] * x3 +
                                 c[7] * y3 + c[8] * (x2 * y) + c[9] * (x * y2) + c[10] * (x2 * x2) +
                                 c[11] * (y2 * y2) + c[12] * (x3 * y) + c[13] * (x2 * y2) + c[14] * (x * y3);
                    double surf = raw / std::sqrt(1.0 + (raw / 2.0) * (raw / 2.0));
                    lut.v[(static_cast<size_t>(i) * S + j) * 3 + m] *= std::pow(2.0, surf);
                }
            }
    }

    if (gamut.active) {
        TcLut out = lut;
        parallel_for(S, [&](long a, long b) {
            for (long i = a; i < b; ++i)
                for (int j = 0; j < S; ++j) {
                    Xy tc{static_cast<double>(i) / (S - 1), static_cast<double>(j) / (S - 1)};
                    Xy xy = compress_xy(cd, quad2tri(tc), illu_xy, gamut);
                    Xy tcc = tri2quad(xy);
                    double ci = tcc[0] * (S - 1), cj = tcc[1] * (S - 1);
                    int i0 = static_cast<int>(std::floor(ci)), j0 = static_cast<int>(std::floor(cj));
                    double fi = ci - i0, fj = cj - j0;
                    int i1 = std::clamp(i0 + 1, 0, S - 1), j1 = std::clamp(j0 + 1, 0, S - 1);
                    i0 = std::clamp(i0, 0, S - 1);
                    j0 = std::clamp(j0, 0, S - 1);
                    for (int m = 0; m < n_ch; ++m) {
                        auto at = [&](int ii, int jj) { return lut.v[(static_cast<size_t>(ii) * S + jj) * n_ch + m]; };
                        double v0 = at(i0, j0) * (1 - fj) + at(i0, j1) * fj;
                        double v1 = at(i1, j0) * (1 - fj) + at(i1, j1) * fj;
                        out.v[(static_cast<size_t>(i) * S + j) * n_ch + m] = v0 * (1 - fi) + v1 * fi;
                    }
                }
        });
        lut = std::move(out);
    }
    return lut;
}

Image rgb_to_raw_hanatos2025(const ColourData& cd, const Image& rgb, const std::string& color_space,
                             bool decode, const std::string& reference_illuminant,
                             const TcLut& lut) {
    if (rgb.c < 3) throw std::runtime_error("rgb_to_raw expects an RGB image");
    const ColourSpace& cs = cd.space(color_space);
    Xy illu = cd.illuminant_xy(reference_illuminant);
    Mat3 M = cd.rgb_to_xyz(cs, &illu, "CAT16");
    Image raw(rgb.h, rgb.w, lut.n_ch);
    parallel_for(rgb.h, [&](long y0, long y1) {
        for (long y = y0; y < y1; ++y)
            for (int x = 0; x < rgb.w; ++x) {
                const double* p = rgb.px(static_cast<int>(y), x);
                Vec3 v{p[0], p[1], p[2]};
                if (decode)
                    for (auto& c : v) c = cctf_decode(cs.cctf, c);
                Vec3 xyz = mat3_apply(M, v);
                double b = xyz[0] + xyz[1] + xyz[2];
                double den = std::fmax(b, 1e-10);
                Xy tc = tri2quad({xyz[0] / den, xyz[1] / den});
                b = nan_to_num(b);
                double* o = raw.px(static_cast<int>(y), x);
                sample_tc_lut(lut, tc[0], tc[1], o);
                for (int c = 0; c < lut.n_ch; ++c) o[c] *= b;
            }
    });
    return raw;
}

Image rgb_to_raw_mallett2019(const ColourData& cd, const Image& rgb, const Vec& sensitivity,
                             int n_ch, const std::string& color_space, bool decode,
                             const std::string& reference_illuminant) {
    if (n_ch < 2) throw std::runtime_error("mallett2019 upsampling needs a colour film");
    const ColourSpace& cs = cd.space(color_space);
    Mat3 M = cd.rgb_to_rgb(cs, cd.space("sRGB"));
    Vec illum = cd.illuminant(reference_illuminant);
    const Vec& basis = cd.mallett_basis();
    Vec A(static_cast<size_t>(3) * n_ch, 0.0), mid(n_ch, 0.0);
    for (int k = 0; k < 3; ++k)
        for (int m = 0; m < n_ch; ++m) {
            double s = 0;
            for (int l = 0; l < kNumWavelengths; ++l)
                s += basis[l * 3 + k] * illum[l] * sensitivity[l * n_ch + m];
            A[k * n_ch + m] = s;
        }
    for (int m = 0; m < n_ch; ++m)
        for (int l = 0; l < kNumWavelengths; ++l) mid[m] += illum[l] * 0.184 * sensitivity[l * n_ch + m];
    Image raw(rgb.h, rgb.w, n_ch);
    for (size_t i = 0; i < rgb.pixels(); ++i) {
        const double* p = &rgb.v[i * rgb.c];
        Vec3 v{p[0], p[1], p[2]};
        if (decode)
            for (auto& c : v) c = cctf_decode(cs.cctf, c);
        Vec3 l = mat3_apply(M, v);
        for (int m = 0; m < n_ch; ++m) {
            double s = l[0] * A[m] + l[1] * A[n_ch + m] + l[2] * A[2 * n_ch + m];
            raw.v[i * n_ch + m] = nan_to_num(s) / mid[1];
        }
    }
    return raw;
}

}  // namespace spectra
