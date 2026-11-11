// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>
#include <stdexcept>

#include "spectra/core/parallel.hpp"
#include "spectra/core/rng.hpp"
#include "spectra/model/emulsion.hpp"
#include "spectra/model/filters.hpp"

namespace spectra {

namespace {

constexpr double kRmsApertureDiameterUm = 48.0;
const double kRmsApertureAreaUm2 = kPi * (kRmsApertureDiameterUm / 2.0) * (kRmsApertureDiameterUm / 2.0);

// Stream identifiers of the per-pixel random streams.
enum : uint32_t { kStreamGrain = 0x100, kStreamClump = 0x200, kStreamGlare = 0x300 };

// log(k!) - Stirling approximation, i.e. the tail correction term.
double stirling_tail(double k) {
    static const double t[10] = {0.0810614667953272,  0.0413406959554092, 0.0276779256849983,
                                 0.02079067210376509, 0.0166446911898211, 0.0138761288230707,
                                 0.0118967099458917,  0.0104112652619720, 0.00925546218271273,
                                 0.00833056343336287};
    if (k <= 9) return t[static_cast<int>(k)];
    double kp1sq = (k + 1) * (k + 1);
    return (1.0 / 12 - (1.0 / 360 - 1.0 / 1260 / kp1sq) / kp1sq) / (k + 1);
}

double log_factorial(double k) {
    // log(k!) = (k + 1/2) log(k + 1) - (k + 1) + log(sqrt(2 pi)) + tail(k)
    return (k + 0.5) * std::log(k + 1.0) - (k + 1.0) + 0.9189385332046727 + stirling_tail(k);
}

int poisson_exact(PixelRng& rng, double lam) {
    if (!(lam > 0.0)) return 0;
    if (lam < 10.0) {
        // Inversion by sequential search.
        for (int attempt = 0; attempt < 8; ++attempt) {
            double u = rng.uniform();
            double p = std::exp(-lam), s = p;
            int k = 0;
            while (u > s && k < 1000) {
                ++k;
                p *= lam / k;
                s += p;
            }
            if (k < 1000) return k;
        }
        return static_cast<int>(lam);
    }
    // PTRS, Hoermann 1993.
    double slam = std::sqrt(lam), loglam = std::log(lam);
    double b = 0.931 + 2.53 * slam;
    double a = -0.059 + 0.02483 * b;
    double invalpha = 1.1239 + 1.1328 / (b - 3.4);
    double vr = 0.9277 - 3.6224 / (b - 2);
    for (;;) {
        double U = rng.uniform() - 0.5, V = rng.uniform();
        double us = 0.5 - std::fabs(U);
        double k = std::floor((2 * a / us + b) * U + lam + 0.43);
        if (us >= 0.07 && V <= vr) return static_cast<int>(k);
        if (k < 0 || (us < 0.013 && V > us)) continue;
        if (std::log(V) + std::log(invalpha) - std::log(a / (us * us) + b) <=
            -lam + k * loglam - log_factorial(k))
            return static_cast<int>(k);
    }
}

int binomial_exact(PixelRng& rng, int n, double p) {
    if (n <= 0 || p <= 0.0) return 0;
    if (p >= 1.0) return n;
    if (p > 0.5) return n - binomial_exact(rng, n, 1.0 - p);
    double q = 1.0 - p;
    if (n * p < 10.0) {
        // BINV: inversion by sequential search.
        double s = p / q, a = (n + 1) * s;
        for (int attempt = 0; attempt < 8; ++attempt) {
            double r = std::pow(q, n), u = rng.uniform();
            int x = 0;
            while (u > r && x <= n) {
                u -= r;
                ++x;
                r *= (a / x - s);
            }
            if (x <= n) return x;
        }
        return static_cast<int>(n * p);
    }
    // BTRS, Hoermann 1993.
    double count = n;
    double stddev = std::sqrt(count * p * q);
    double b = 1.15 + 2.53 * stddev;
    double a = -0.0873 + 0.0248 * b + 0.01 * p;
    double c = count * p + 0.5;
    double v_r = 0.92 - 4.2 / b;
    double r = p / q;
    double alpha = (2.83 + 5.1 / b) * stddev;
    double m = std::floor((count + 1) * p);
    for (;;) {
        double u = rng.uniform() - 0.5, v = rng.uniform();
        double us = 0.5 - std::fabs(u);
        double k = std::floor((2 * a / us + b) * u + c);
        if (us >= 0.07 && v <= v_r) return static_cast<int>(k);
        if (k < 0 || k > count) continue;
        v = std::log(v * alpha / (a / (us * us) + b));
        double upper = (m + 0.5) * std::log((m + 1) / (r * (count - m + 1))) +
                       (count + 1) * std::log((count - m + 1) / (count - k + 1)) +
                       (k + 0.5) * std::log(r * (count - k + 1) / (k + 1)) + stirling_tail(m) +
                       stirling_tail(count - m) - stirling_tail(k) - stirling_tail(count - k);
        if (v <= upper) return static_cast<int>(k);
    }
}

// spektrafilm.utils.fast_stats.fast_poisson
int poisson_fast(PixelRng& rng, double lam) {
    if (lam <= 0.0) return 0;
    if (lam < 30.0) {
        double L = std::exp(-lam), p = 1.0;
        int k = 0;
        while (p > L) {
            ++k;
            p *= rng.uniform();
        }
        return k - 1;
    }
    double s = lam + std::sqrt(lam) * rng.normal();
    int v = static_cast<int>(round_half_even(s));
    return v < 0 ? 0 : v;
}

// spektrafilm.utils.fast_stats.fast_binomial
int binomial_fast(PixelRng& rng, int n, double p) {
    if (p <= 0.0) return 0;
    if (p >= 1.0) return n;
    if (n < 25) {
        int count = 0;
        for (int k = 0; k < n; ++k)
            if (rng.uniform() < p) ++count;
        return count;
    }
    double mean = n * p, var = n * p * (1.0 - p);
    if (var > 10) {
        int v = static_cast<int>(round_half_even(mean + std::sqrt(var) * rng.normal()));
        return std::clamp(v, 0, n);
    }
    double u = rng.uniform(), cdf = 0.0, prob = std::pow(1.0 - p, n);
    int k = 0;
    while (cdf < u && k <= n) {
        cdf += prob;
        if (k < n) prob = prob * (static_cast<double>(n - k) / (k + 1)) * (p / (1.0 - p));
        ++k;
    }
    return k - 1;
}

}  // namespace

int sample_poisson(PixelRng& rng, double lambda, bool fast) {
    return fast ? poisson_fast(rng, lambda) : poisson_exact(rng, lambda);
}

int sample_binomial(PixelRng& rng, int n, double p, bool fast) {
    return fast ? binomial_fast(rng, n, p) : binomial_exact(rng, n, p);
}

void sample_grain_layer(const double* density, double* out, int h, int w, double density_max,
                        double n_particles, double uniformity, uint64_t seed, uint32_t stream,
                        bool fast) {
    double od_particle = density_max / n_particles;
    parallel_for(h, [&](long y0, long y1) {
        for (long y = y0; y < y1; ++y)
            for (int x = 0; x < w; ++x) {
                size_t i = static_cast<size_t>(y) * w + x;
                double p = std::clamp(density[i] / density_max, 1e-6, 1.0 - 1e-6);
                double saturation = 1.0 - p * uniformity * (1.0 - 1e-6);
                PixelRng rng(seed, stream, static_cast<uint32_t>(x), static_cast<uint32_t>(y));
                int seeds = sample_poisson(rng, n_particles / saturation, fast);
                int developed = sample_binomial(rng, seeds, p, fast);
                out[i] = developed * od_particle * saturation;
            }
    });
}

GrainLayerParams grain_layer_params(const GrainParams& g, const Curves& cv, double pixel_size_um) {
    GrainLayerParams r;
    int n_ch = cv.n_ch;
    int n_sub = cv.has_layers() ? cv.n_layers : 1;
    r.n_ch = n_ch;
    r.n_sub = n_sub;
    auto layer = [&](int i, int s, int k) { return cv.has_layers() ? cv.l(i, s, k) : cv.t(i, k); };
    Vec dmax_layers(static_cast<size_t>(n_sub) * n_ch, -INFINITY);
    for (int i = 0; i < cv.n_le; ++i)
        for (int s = 0; s < n_sub; ++s)
            for (int k = 0; k < n_ch; ++k) {
                double v = layer(i, s, k);
                if (!std::isnan(v) && v > dmax_layers[s * n_ch + k]) dmax_layers[s * n_ch + k] = v;
            }
    Vec scales(n_sub);
    for (int s = 0; s < n_sub; ++s) scales[s] = g.particle_scale_sublayers[s];
    Vec total(n_ch, 0.0), frac(static_cast<size_t>(n_sub) * n_ch);
    for (int s = 0; s < n_sub; ++s)
        for (int k = 0; k < n_ch; ++k) total[k] += dmax_layers[s * n_ch + k];
    for (int s = 0; s < n_sub; ++s)
        for (int k = 0; k < n_ch; ++k) frac[s * n_ch + k] = dmax_layers[s * n_ch + k] / total[k];
    r.density_min = match_channels(g.density_min, n_ch);
    r.uniformity = match_channels(g.uniformity, n_ch);
    Vec rms = match_channels(g.rms_granularity, n_ch);
    r.density_min_layers.resize(frac.size());
    r.density_max_layers.resize(frac.size());
    for (int s = 0; s < n_sub; ++s)
        for (int k = 0; k < n_ch; ++k) {
            r.density_min_layers[s * n_ch + k] = frac[s * n_ch + k] * r.density_min[k];
            r.density_max_layers[s * n_ch + k] = dmax_layers[s * n_ch + k] + r.density_min_layers[s * n_ch + k];
        }
    // _coarsest_area_from_curves: exact realised multilayer peak.
    Vec peak(n_ch, -INFINITY);
    for (int i = 0; i < cv.n_le; ++i)
        for (int k = 0; k < n_ch; ++k) {
            double sum = 0;
            for (int s = 0; s < n_sub; ++s) {
                double v = layer(i, s, k);
                double d_abs = (std::isnan(v) ? 0.0 : v) + r.density_min_layers[s * n_ch + k];
                double weight = scales[s] / frac[s * n_ch + k];
                sum += weight * d_abs * (r.density_max_layers[s * n_ch + k] - r.uniformity[k] * d_abs);
            }
            if (!std::isnan(sum) && sum > peak[k]) peak[k] = sum;
        }
    double pixel_area = pixel_size_um * pixel_size_um;
    r.n_particles.resize(frac.size());
    for (int k = 0; k < n_ch; ++k) {
        double sigma_in = rms[k] / 1000.0;
        double a = sigma_in * sigma_in * kRmsApertureAreaUm2 / std::max(peak[k], 1e-9);
        for (int s = 0; s < n_sub; ++s)
            r.n_particles[s * n_ch + k] = pixel_area * frac[s * n_ch + k] / (a * scales[s]);
    }
    return r;
}

Image apply_grain(const Image& density_cmy, double pixel_size_um, const GrainParams& g,
                  const Curves& cv, const std::string& profile_type, const GrainOptions& opt) {
    if (!g.active) return density_cmy;
    bool positive = profile_type == "positive";
    int n_ch = density_cmy.c, h = density_cmy.h, w = density_cmy.w;
    GrainLayerParams lp = grain_layer_params(g, cv, pixel_size_um);
    int n_sub = lp.n_sub, n_le = cv.n_le;
    size_t n = density_cmy.pixels();
    Image out(h, w, n_ch);
    Vec plane(n), blurred(n), acc(n);
    for (int ch = 0; ch < n_ch; ++ch) {
        // x axis: the channel's total curve (negated for positive film so it increases).
        Vec xa(n_le);
        for (int i = 0; i < n_le; ++i) xa[i] = positive ? -cv.t(i, ch) : cv.t(i, ch);
        std::fill(acc.begin(), acc.end(), 0.0);
        for (int sl = 0; sl < n_sub; ++sl) {
            Vec ya(n_le);
            for (int i = 0; i < n_le; ++i) ya[i] = cv.has_layers() ? cv.l(i, sl, ch) : cv.t(i, ch);
            double dmax = lp.density_max_layers[sl * n_ch + ch];
            double npp = lp.n_particles[sl * n_ch + ch];
            double dmin_layer = lp.density_min_layers[sl * n_ch + ch];
            double u = lp.uniformity[ch];
            double od_particle = dmax / npp;
            uint32_t stream = kStreamGrain + static_cast<uint32_t>(ch * 16 + sl);
            parallel_for(h, [&](long y0, long y1) {
                for (long y = y0; y < y1; ++y)
                    for (int x = 0; x < w; ++x) {
                        double d = density_cmy.at(static_cast<int>(y), x, ch);
                        plane[static_cast<size_t>(y) * w + x] =
                            fast_interp1(positive ? -d : d, xa.data(), ya.data(), n_le) + dmin_layer;
                    }
            });
            sample_grain_layer(plane.data(), plane.data(), h, w, dmax, npp, u, opt.seed, stream,
                               opt.use_fast_stats);
            if (g.blur_dye_clouds_um > 0) {
                gaussian_filter_plane(plane.data(), blurred.data(), h, w,
                                      g.blur_dye_clouds_um * std::sqrt(od_particle));
                for (size_t i = 0; i < n; ++i) acc[i] += blurred[i];
            } else {
                for (size_t i = 0; i < n; ++i) acc[i] += plane[i];
            }
        }
        insert_channel(out, ch, acc);
    }
    // add_micro_structure
    double ms_blur_px = g.micro_structure[0] / pixel_size_um;
    double ms_sigma = g.micro_structure[1] * 0.001 / pixel_size_um;
    if (ms_sigma > 0.05) {
        double sigma2 = std::log1p(ms_sigma * ms_sigma);
        double sd = std::sqrt(sigma2);
        Image clump(h, w, n_ch);
        parallel_for(h, [&](long y0, long y1) {
            for (long y = y0; y < y1; ++y)
                for (int x = 0; x < w; ++x)
                    for (int ch = 0; ch < n_ch; ++ch) {
                        PixelRng rng(opt.seed, kStreamClump + static_cast<uint32_t>(ch),
                                     static_cast<uint32_t>(x), static_cast<uint32_t>(y));
                        clump.at(static_cast<int>(y), x, ch) = std::exp(rng.normal() * sd - 0.5 * sigma2);
                    }
        });
        if (ms_blur_px > 0.4) clump = gaussian_filter(clump, ms_blur_px);
        for (size_t i = 0; i < out.v.size(); ++i) out.v[i] *= clump.v[i];
    }
    double blur = g.blur * opt.pixel_param_scale;
    double usm_sigma = g.mult_usm_sigma * opt.pixel_param_scale;
    if (blur > 0) out = gaussian_filter(out, blur);
    if (g.mult_usm_amount > 0 && usm_sigma > 0)
        out = multiplicative_unsharp_mask(out, usm_sigma, g.mult_usm_amount);
    for (size_t i = 0; i < n; ++i)
        for (int ch = 0; ch < n_ch; ++ch) out.v[i * n_ch + ch] -= lp.density_min[ch];
    return out;
}

void add_glare(Image& xyz, const Vec3& illuminant_xyz, const GlareParams& g, uint64_t seed,
               double pixel_param_scale) {
    if (!g.active || !(g.percent > 0)) return;
    int h = xyz.h, w = xyz.w;
    double m = g.percent, s = g.roughness * g.percent;
    double sigma2 = std::log(1.0 + (s * s) / (m * m));
    double sigma = std::sqrt(sigma2), mu = std::log(m) - sigma2 / 2.0;
    Vec amount(static_cast<size_t>(h) * w), blurred(amount.size());
    parallel_for(h, [&](long y0, long y1) {
        for (long y = y0; y < y1; ++y)
            for (int x = 0; x < w; ++x) {
                PixelRng rng(seed, kStreamGlare, static_cast<uint32_t>(x), static_cast<uint32_t>(y));
                amount[static_cast<size_t>(y) * w + x] =
                    sigma < 1e-6 ? std::exp(mu) : std::exp(mu + sigma * rng.normal());
            }
    });
    gaussian_filter_plane(amount.data(), blurred.data(), h, w, g.blur * pixel_param_scale);
    for (size_t i = 0; i < blurred.size(); ++i) {
        double a = blurred[i] / 100.0;
        for (int k = 0; k < 3; ++k) xyz.v[i * 3 + k] += a * illuminant_xyz[k];
    }
}

}  // namespace spectra
