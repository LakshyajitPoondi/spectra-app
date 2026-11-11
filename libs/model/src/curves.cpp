// SPDX-License-Identifier: GPL-3.0-or-later
#include "spectra/model/curves.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <stdexcept>

#include "spectra/core/parallel.hpp"

namespace spectra {

namespace {

constexpr double kSeptK = 5.8013;
constexpr double kSigmaFloor = 0.05;
const double kGumbelLocation = -std::log(std::log(2.0));
const double kGumbelWidth = 0.5 * std::log(2.0) * std::sqrt(2.0 * kPi);

double septic_cdf(double z, double alpha) {
    double u = std::clamp(z / kSeptK + 0.5, 0.0, 1.0);
    double v = u;
    if (alpha != 0.0) {
        double t = 2.0 * u - 1.0;
        v = std::clamp(u + alpha * u * (1.0 - u) * t * t, 0.0, 1.0);
    }
    double v2 = v * v;
    return (v2 * v2) * (35.0 + v * (-84.0 + v * (70.0 - 20.0 * v)));
}

double layer_cdf_values(double z, const std::string& model_type, double alpha) {
    if (model_type == "norm_cdfs") return ndtr(z);
    if (model_type == "sept_norm_cdfs") return septic_cdf(z, alpha);
    throw std::runtime_error("unknown density-curve model_type '" + model_type + "'");
}

double layer_cdf(double z, bool positive, const std::string& model_type, double alpha,
                 double gumbel_mix) {
    double zs = positive ? -z : z;
    double cdf = layer_cdf_values(zs, model_type, alpha);
    if (gumbel_mix > 0.0) {
        double g = std::exp(-std::exp(-(zs / kGumbelWidth + kGumbelLocation)));
        cdf = (1.0 - gumbel_mix) * cdf + gumbel_mix * g;
    }
    return cdf;
}

struct ChannelParams {
    Vec centers, amplitudes, sigmas, alphas, mix;
};

double channel_density_at(double x, const ChannelParams& p, bool positive,
                          const std::string& model_type, const Vec& mix) {
    double total = 0.0;
    for (size_t i = 0; i < p.centers.size(); ++i) {
        double z = (x - p.centers[i]) / p.sigmas[i];
        total += p.amplitudes[i] * layer_cdf(z, positive, model_type, p.alphas[i], mix[i]);
    }
    return total;
}

ChannelParams channel_params(const DensityCurvesModel& m, int row) {
    ChannelParams p;
    int n = m.n_layers;
    p.centers.assign(m.centers.begin() + row * n, m.centers.begin() + (row + 1) * n);
    p.amplitudes.assign(m.amplitudes.begin() + row * n, m.amplitudes.begin() + (row + 1) * n);
    p.sigmas.assign(m.sigmas.begin() + row * n, m.sigmas.begin() + (row + 1) * n);
    if (m.has_alphas())
        p.alphas.assign(m.alphas.begin() + row * n, m.alphas.begin() + (row + 1) * n);
    else
        p.alphas.assign(n, 0.0);
    p.mix.assign(n, 0.0);
    return p;
}

double exhaustion_center_offset(const ChannelParams& p, bool positive,
                                const std::string& model_type) {
    bool all_zero = true;
    for (double m : p.mix)
        if (std::fabs(m) > 1e-8) all_zero = false;
    if (all_zero) return 0.0;
    Vec zero_mix(p.mix.size(), 0.0);
    double target = channel_density_at(0.0, p, positive, model_type, zero_mix);
    auto residual = [&](double offset) {
        ChannelParams q = p;
        for (auto& c : q.centers) c += offset;
        return channel_density_at(0.0, q, positive, model_type, p.mix) - target;
    };
    double r0 = residual(0.0);
    if (std::fabs(r0) <= 1e-12) return 0.0;
    double lo = -0.25, hi = 0.25;
    double r_lo = residual(lo), r_hi = residual(hi);
    for (int it = 0; it < 12; ++it) {
        if (r_lo == 0.0) return lo;
        if (r_hi == 0.0) return hi;
        if (r_lo * r_hi < 0.0) return brentq(residual, lo, hi, 1e-10);
        lo *= 2.0;
        hi *= 2.0;
        r_lo = residual(lo);
        r_hi = residual(hi);
    }
    return 0.0;
}

ChannelParams morph_channel_params(const DensityCurvesModel& m, const ChemistryParams& chem,
                                   int row, bool positive) {
    ChannelParams p = channel_params(m, row);
    int n = m.n_layers;
    std::vector<int> order(n);
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(),
                     [&](int a, int b) { return p.centers[a] < p.centers[b]; });
    int i_fast = order[0], i_mid = order[n / 2], i_slow = order[n - 1];
    const double ch_gamma[3] = {chem.gamma_factor_red, chem.gamma_factor_green,
                                chem.gamma_factor_blue};
    if (row > 2) throw std::runtime_error("chemistry morph supports at most 3 channels");
    double g_fast = chem.gamma_factor * ch_gamma[row] * chem.gamma_factor_fast;
    double g_mid = chem.gamma_factor * ch_gamma[row] * chem.gamma_factor_slow;
    double g_slow = g_mid;
    if (g_fast <= 0.0 || g_mid <= 0.0 || g_slow <= 0.0)
        throw std::runtime_error("effective gamma must remain strictly positive per channel");
    // Sequential updates, exactly as upstream (matters if two indices coincide).
    p.sigmas[i_fast] = std::max(p.sigmas[i_fast] / g_fast, kSigmaFloor);
    p.centers[i_fast] = p.centers[i_fast] / g_fast;
    p.sigmas[i_mid] = std::max(p.sigmas[i_mid] / g_mid, kSigmaFloor);
    p.centers[i_mid] = p.centers[i_mid] / g_mid;
    p.sigmas[i_slow] = std::max(p.sigmas[i_slow] / g_slow, kSigmaFloor);
    p.centers[i_slow] = p.centers[i_slow] / g_slow;
    p.mix.assign(n, chem.developer_exhaustion);
    double offset = exhaustion_center_offset(p, positive, m.model_type);
    for (auto& c : p.centers) c = c + offset;
    return p;
}

void validate_chemistry(const ChemistryParams& c) {
    for (double g : {c.gamma_factor, c.gamma_factor_fast, c.gamma_factor_slow, c.gamma_factor_red,
                     c.gamma_factor_green, c.gamma_factor_blue})
        if (g <= 0.0) throw std::runtime_error("gamma factors must be strictly positive");
    if (!(c.developer_exhaustion >= 0.0 && c.developer_exhaustion <= 1.0))
        throw std::runtime_error("developer_exhaustion must be in [0, 1]");
}

Curves sample(const Vec& le, const std::vector<ChannelParams>& ch, bool positive,
              const std::string& model_type, int n_layers) {
    Curves c;
    c.n_le = static_cast<int>(le.size());
    c.n_ch = static_cast<int>(ch.size());
    c.n_layers = n_layers;
    c.total.assign(static_cast<size_t>(c.n_le) * c.n_ch, 0.0);
    c.layers.assign(static_cast<size_t>(c.n_le) * n_layers * c.n_ch, 0.0);
    for (int k = 0; k < c.n_ch; ++k) {
        const ChannelParams& p = ch[k];
        for (int i = 0; i < c.n_le; ++i) {
            double sum = 0.0;
            for (int l = 0; l < n_layers; ++l) {
                double z = (le[i] - p.centers[l]) / p.sigmas[l];
                double v = p.amplitudes[l] * layer_cdf(z, positive, model_type, p.alphas[l], p.mix[l]);
                c.layers[(static_cast<size_t>(i) * n_layers + l) * c.n_ch + k] = v;
                sum += v;
            }
            c.total[static_cast<size_t>(i) * c.n_ch + k] = sum;
        }
    }
    return c;
}

}  // namespace

Vec Curves::column_max() const {
    Vec m(n_ch, -std::numeric_limits<double>::infinity());
    for (int i = 0; i < n_le; ++i)
        for (int k = 0; k < n_ch; ++k) {
            double v = t(i, k);
            if (!std::isnan(v) && v > m[k]) m[k] = v;
        }
    return m;
}

Vec Curves::column_min() const {
    Vec m(n_ch, std::numeric_limits<double>::infinity());
    for (int i = 0; i < n_le; ++i)
        for (int k = 0; k < n_ch; ++k) {
            double v = t(i, k);
            if (!std::isnan(v) && v < m[k]) m[k] = v;
        }
    return m;
}

Curves evaluate_density_curves(const DensityCurvesModel& model, const Vec& le,
                               const std::string& profile_type) {
    std::vector<ChannelParams> ch;
    for (int r = 0; r < model.n_rows; ++r) ch.push_back(channel_params(model, r));
    Curves c = sample(le, ch, profile_type == "positive", model.model_type, model.n_layers);
    if (model.n_layers <= 1) c.layers.clear();
    return c;
}

int select_development_time_index(const Vec& times, int n_columns,
                                  const std::optional<double>& development_time) {
    if (times.empty() || n_columns <= 1) return -1;
    if (!development_time) return (static_cast<int>(times.size()) - 1) / 2;
    int best = 0;
    double bd = std::fabs(times[0] - *development_time);
    for (size_t i = 1; i < times.size(); ++i) {
        double d = std::fabs(times[i] - *development_time);
        if (d < bd) {
            bd = d;
            best = static_cast<int>(i);
        }
    }
    return best;
}

DensityCurvesModel select_model_row(const DensityCurvesModel& m, int index) {
    DensityCurvesModel r;
    r.model_type = m.model_type;
    r.n_rows = 1;
    r.n_layers = m.n_layers;
    int n = m.n_layers;
    r.centers.assign(m.centers.begin() + index * n, m.centers.begin() + (index + 1) * n);
    r.amplitudes.assign(m.amplitudes.begin() + index * n, m.amplitudes.begin() + (index + 1) * n);
    r.sigmas.assign(m.sigmas.begin() + index * n, m.sigmas.begin() + (index + 1) * n);
    if (m.has_alphas())
        r.alphas.assign(m.alphas.begin() + index * n, m.alphas.begin() + (index + 1) * n);
    return r;
}

Curves apply_curves_morph(const Vec& le, const DensityCurvesModel& model,
                          const ChemistryParams& chem, const std::string& profile_type) {
    if (model.n_layers == 0) throw std::runtime_error("morph requires a fitted density-curve model");
    bool positive = profile_type == "positive";
    if (chem.active) validate_chemistry(chem);
    std::vector<ChannelParams> ch;
    for (int r = 0; r < model.n_rows; ++r)
        ch.push_back(chem.active ? morph_channel_params(model, chem, r, positive)
                                 : channel_params(model, r));
    return sample(le, ch, positive, model.model_type, model.n_layers);
}

Image interpolate_exposure_to_density(const Image& log_raw, const Vec& curves, const Vec& le) {
    Image out(log_raw.h, log_raw.w, log_raw.c);
    int n = static_cast<int>(le.size()), c = log_raw.c;
    parallel_for(log_raw.h, [&](long y0, long y1) {
        for (long y = y0; y < y1; ++y)
            for (int x = 0; x < log_raw.w; ++x) {
                const double* in = log_raw.px(static_cast<int>(y), x);
                double* o = out.px(static_cast<int>(y), x);
                for (int k = 0; k < c; ++k)
                    o[k] = fast_interp1(in[k], le.data(), curves.data() + k, n, 1, c);
            }
    });
    return out;
}

}  // namespace spectra
