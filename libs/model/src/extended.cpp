// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from spektrafilm-ofx, Copyright (c) Aedan Diez, GPL-3.0. See extended.hpp.
#include "spectra/model/extended.hpp"

#include <algorithm>
#include <cmath>

namespace spectra::extended {

namespace {
double mix(double a, double b, double t) { return a + (b - a) * t; }
double sigmoid(double u) { return 1.0 / (1.0 + std::exp(-u)); }

double development_activity(double stops) {
    double s = std::clamp(stops, -2.0, 2.0);
    double seconds;
    if (s < 0.0) seconds = mix(180.0, 150.0, std::min(-s, 1.0));
    else if (s <= 1.0) seconds = mix(180.0, 220.0, s);
    else seconds = mix(220.0, 280.0, s - 1.0);
    return std::log(seconds / 180.0);
}

double speed_gain(double stops) {
    if (stops > 0.0) {
        double push_one = mix(0.0, 0.33, std::min(stops, 1.0));
        return stops <= 1.0 ? push_one : mix(0.33, 0.5, std::min(stops - 1.0, 1.0));
    }
    if (stops < 0.0) return mix(0.0, -0.2, std::min(-stops, 1.0));
    return 0.0;
}

double warp_log_raw(double log_raw, int ch, double stops) {
    static const double mid_lin[3] = {0.25, 0.28, 0.31}, sh_lin[3] = {0.32, 0.38, 0.45};
    static const double mid_quad[3] = {0.04, 0.06, 0.08}, sh_quad[3] = {0.08, 0.12, 0.16};
    double a = development_activity(stops), a2 = a * std::fabs(a);
    double toe = 1.0 - sigmoid((log_raw + 2.0) / 0.5);
    double shoulder = sigmoid(log_raw / 0.5);
    double mid = std::max(1.0 - toe - shoulder, 0.0);
    double mid_shift = mid_lin[ch] * a + mid_quad[ch] * a2;
    double sh_shift = sh_lin[ch] * a + sh_quad[ch] * a2;
    return log_raw + mid_shift * mid + sh_shift * shoulder;  // the toe does not move
}
}  // namespace

double film_gamma(double stops) {
    double s = std::clamp(stops, -2.0, 2.0);
    if (s < 0.0) return std::pow(150.0 / 180.0, -s);
    if (s <= 1.0) return std::exp(std::log(220.0 / 180.0) * s);
    return std::exp(std::log(220.0 / 180.0) + std::log(280.0 / 220.0) * (s - 1.0));
}

double print_gamma(double stops) { return std::exp2(std::clamp(stops, -2.0, 2.0) * 0.25); }

void experimental_log_raw(const double* log_raw, int n_ch, double stops, double* out) {
    stops = std::clamp(stops, -2.0, 2.0);
    double shift = (stops - speed_gain(stops)) * 0.3010299956639812;
    double v[3];
    for (int k = 0; k < n_ch; ++k) v[k] = log_raw[k] - shift;
    if (n_ch == 3) {
        // Weak inter-layer coupling of the development change.
        double a = development_activity(stops);
        double mean = (v[0] + v[1] + v[2]) / 3.0;
        double d[3] = {v[0] - mean, v[1] - mean, v[2] - mean};
        const double c = 0.015;
        v[0] += a * (c * d[1] - c * d[2]);
        v[1] += a * (-c * d[0] + c * d[2]);
        v[2] += a * (c * d[0] - c * d[1]);
    }
    for (int k = 0; k < n_ch; ++k) out[k] = warp_log_raw(v[k], n_ch == 3 ? k : 1, stops);
}

double experimental_density_gain(double lookup, int channel, double stops) {
    static const double build_lin[3] = {1.05, 0.95, 1.00}, build_quad[3] = {-0.90, -0.82, -0.86};
    double a = development_activity(stops), a2 = a * std::fabs(a);
    double toe = 1.0 - sigmoid((lookup + 2.0) / 0.5);
    double shoulder = sigmoid(lookup / 0.5);
    double mid = std::max(1.0 - toe - shoulder, 0.0);
    double weight = 0.12 * toe + mid + shoulder;
    double build = build_lin[channel] * a + build_quad[channel] * a2;
    return std::clamp(1.0 + build * weight, 0.35, 2.0);
}

BleachResult bleach_bypass(const double* cmy, int n_ch, double amount, bool print_stage, bool positive,
                           const double* density_max, double leuco) {
    BleachResult r{};
    amount = std::clamp(amount, 0.0, 1.0);
    double layer_shoulder = print_stage ? 0.65 : 0.85;
    double retained = 0.0, black = -INFINITY;
    for (int k = 0; k < n_ch; ++k) {
        double silver = (!print_stage && positive) ? std::max(density_max[k] - cmy[k], 0.0) : std::max(cmy[k], 0.0);
        retained += silver / (silver + layer_shoulder);
        black = std::max(black, cmy[k]);
    }
    retained /= n_ch;
    r.silver = amount * (print_stage ? 0.36 : 0.22) * retained;
    double black_amount = std::clamp(amount * retained, 0.0, 1.0);
    for (int k = 0; k < n_ch; ++k) r.dye[k] = mix(cmy[k], black, black_amount);
    if (!print_stage && !positive && n_ch == 3) {
        // Leuco-cyan dye loss in the unbleached negative.
        double cmax = std::max(density_max[0], 1e-6);
        double drive = std::clamp(std::max(cmy[0], 0.0) / cmax, 0.0, 1.0);
        double coupling = std::clamp(leuco, 0.0, 2.0);
        const double max_loss = 0.30;
        double loss = std::min(amount * coupling * max_loss * drive, max_loss * coupling);
        r.dye[0] = std::max(r.dye[0] - loss, 0.0);
    }
    return r;
}

Vec3 hdr_tone_map(const Vec3& rgb, const HdrOutputParams& p) {
    double ref = std::max(p.reference_white_nits, 1.0);
    double peak = std::max(p.peak_nits, ref + 1.0);
    double scale = ref * std::exp2(p.exposure_ev);
    Vec3 nits{rgb[0] * scale, rgb[1] * scale, rgb[2] * scale};
    double y = 0.2627 * nits[0] + 0.6780 * nits[1] + 0.0593 * nits[2];
    if (!(y > 1e-6) || !std::isfinite(y)) return {0, 0, 0};
    double mapped = y;
    if (p.tone_mapping == "clip") {
        mapped = std::min(y, peak);
    } else if (y > ref) {
        double shoulder = std::max(peak - ref, 1.0);
        mapped = ref + shoulder * (1.0 - std::exp(-(y - ref) / shoulder));
    }
    double g = mapped / y;
    return {nits[0] * g, nits[1] * g, nits[2] * g};
}

double encode_pq(double nits) {
    const double m1 = 2610.0 / 16384.0, m2 = 2523.0 / 32.0, c1 = 3424.0 / 4096.0, c2 = 2413.0 / 128.0,
                 c3 = 2392.0 / 128.0;
    double y = std::pow(std::max(nits, 0.0) / 10000.0, m1);
    return std::pow((c1 + c2 * y) / (1.0 + c3 * y), m2);
}

double encode_hlg_display(double nits, double peak) {
    double gamma = std::max(1.0 + 0.42 * std::log10(std::max(peak, 1.0) / 1000.0), 1e-6);
    double e = std::pow(std::clamp(nits / peak, 0.0, 1.0), 1.0 / gamma);
    const double a = 0.17883277, b = 1.0 - 4.0 * a, c = 0.55991073;
    return e <= 1.0 / 12.0 ? std::sqrt(3.0 * e) : a * std::log(12.0 * e - b) + c;
}

}  // namespace spectra::extended
