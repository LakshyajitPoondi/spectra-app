// SPDX-License-Identifier: GPL-3.0-or-later
#include "spectra/model/diffusion.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <map>
#include <stdexcept>

#include <pocketfft_hdronly.h>

#include "spectra/core/parallel.hpp"
#include "spectra/model/filters.hpp"

namespace spectra {

void boost_highlights(Image& raw, double boost_ev, double boost_range, double protect_ev,
                      const std::optional<double>& max_override, double midgray) {
    if (boost_ev < 0) throw std::runtime_error("boost_ev must be >= 0");
    if (!(boost_range >= 0.0 && boost_range <= 1.0))
        throw std::runtime_error("boost_range must be in [0, 1]");
    if (protect_ev < 0) throw std::runtime_error("protect_ev must be >= 0");
    if (boost_ev == 0) return;
    double max_raw;
    if (max_override) {
        max_raw = *max_override;
    } else {
        max_raw = raw.v.empty() ? 0.0 : raw.v[0];
        for (double v : raw.v) max_raw = std::max(max_raw, v);
    }
    if (max_raw == 0.0) {
        std::fill(raw.v.begin(), raw.v.end(), 0.0);
        return;
    }
    double raw_x0 = std::clamp(midgray * std::pow(2.0, protect_ev), 0.0, max_raw);
    if (raw_x0 == max_raw) return;
    double a = std::pow(28.0, 1.0 - boost_range);
    double x0 = raw_x0 / max_raw;
    double denom = std::exp(a * (1.0 - x0)) - a * (1.0 - x0) - 1.0;
    if (denom <= 0.0) throw std::runtime_error("boost: invalid parameters");
    double k = (std::pow(2.0, boost_ev) - 1.0) / denom;
    double inv_max = 1.0 / max_raw, scale = k * max_raw;
    parallel_for(static_cast<long>(raw.v.size()), [&](long b, long e) {
        for (long i = b; i < e; ++i) {
            double xv = raw.v[i];
            if (xv > raw_x0) {
                double dx = (xv - raw_x0) * inv_max;
                raw.v[i] = xv + scale * (std::exp(a * dx) - a * dx - 1.0);
            }
        }
    }, 4096);
}

void apply_halation_um(Image& raw, const HalationParams& h, double pixel_size_um) {
    if (!h.active) return;
    int n_ch = raw.c;
    size_t n = raw.pixels();
    double s_amount = h.scatter_amount, s_scale = h.scatter_spatial_scale;
    Vec w_s = match_channels(h.scatter_tail_weight, n_ch);
    Vec sigma_c = match_channels(h.scatter_core_um, n_ch);
    Vec lambda_t = match_channels(h.scatter_tail_um, n_ch);
    bool any_c = false, any_t = false;
    for (int k = 0; k < n_ch; ++k) {
        sigma_c[k] = sigma_c[k] * s_scale / pixel_size_um;
        lambda_t[k] = lambda_t[k] * s_scale / pixel_size_um;
        any_c |= sigma_c[k] > 0;
        any_t |= lambda_t[k] > 0;
    }
    if (s_amount > 0 && (any_c || any_t)) {
        Vec sc(n_ch), lt(n_ch);
        for (int k = 0; k < n_ch; ++k) {
            sc[k] = std::max(sigma_c[k], 1e-6);
            lt[k] = std::max(lambda_t[k], 1e-6);
        }
        Image core = gaussian_filter(raw, sc);
        Image tail = exponential_filter(raw, lt);
        for (size_t i = 0; i < n; ++i)
            for (int k = 0; k < n_ch; ++k) {
                size_t j = i * n_ch + k;
                double c = core.v[j] * (1.0 - w_s[k]);
                double t = tail.v[j] * w_s[k];
                c += t;
                raw.v[j] = raw.v[j] * (1.0 - s_amount) + c * s_amount;
            }
    }
    double h_amount = h.halation_amount, h_scale = h.halation_spatial_scale;
    Vec a_tot = match_channels(h.halation_strength, n_ch);
    Vec sigma_h = match_channels(h.halation_first_sigma_um, n_ch);
    bool any_a = false, any_s = false;
    for (int k = 0; k < n_ch; ++k) {
        a_tot[k] *= h_amount;
        sigma_h[k] = sigma_h[k] * h_scale / pixel_size_um;
        any_a |= a_tot[k] > 0;
        any_s |= sigma_h[k] > 0;
    }
    int N = h.halation_n_bounces;
    double rho = h.halation_bounce_decay;
    if (N >= 1 && any_a && any_s) {
        Vec decay(N);
        double dsum = 0;
        for (int k = 1; k <= N; ++k) {
            decay[k - 1] = std::pow(rho, k - 1);
            dsum += decay[k - 1];
        }
        for (auto& d : decay) d /= dsum;
        Image acc;
        for (int k = 1; k <= N; ++k) {
            Vec sk(n_ch);
            for (int c = 0; c < n_ch; ++c) sk[c] = std::max(sigma_h[c] * std::sqrt(static_cast<double>(k)), 1e-6);
            Image blurred = gaussian_filter(raw, sk);
            double wk = decay[k - 1];
            if (k == 1) {
                acc = std::move(blurred);
                for (auto& v : acc.v) v *= wk;
            } else {
                for (size_t i = 0; i < acc.v.size(); ++i) acc.v[i] += blurred.v[i] * wk;
            }
        }
        for (size_t i = 0; i < n; ++i)
            for (int c = 0; c < n_ch; ++c) {
                size_t j = i * n_ch + c;
                raw.v[j] += acc.v[j] * a_tot[c];
                if (h.halation_renormalize) raw.v[j] /= (1.0 + a_tot[c]);
            }
    }
}

Image apply_gaussian_blur_um(const Image& img, double sigma_um, double pixel_size_um) {
    if (sigma_um <= 0) return img;
    double sigma = sigma_um / pixel_size_um;
    if (sigma > 0) return gaussian_filter(img, sigma);
    return img;
}

namespace {

struct Group {
    double lambda_um, spread;
    int n;
    double alpha;
};
struct Family {
    Group core, halo, bloom;
    double w_c, w_h, w_b, warmth_base, gain;
};

const std::map<std::string, Family>& families() {
    static const std::map<std::string, Family> f = {
        {"glimmerglass", {{10.0, 1.5, 2, 0}, {50.0, 2.0, 3, 0}, {260.0, 2.5, 4, 3.2}, 0.60, 0.30, 0.10, 0.0, 0.65}},
        {"black_pro_mist", {{16.0, 1.5, 2, 0}, {95.0, 2.0, 3, 0}, {380.0, 2.5, 4, 3.5}, 0.40, 0.47, 0.13, 0.65, 0.75}},
        {"pro_mist", {{14.0, 1.5, 2, 0}, {150.0, 2.0, 3, 0}, {650.0, 2.5, 4, 2.9}, 0.28, 0.42, 0.30, 0.40, 1.05}},
        {"cinebloom", {{20.0, 1.5, 2, 0}, {200.0, 2.0, 3, 0}, {1000.0, 2.5, 4, 2.5}, 0.22, 0.30, 0.48, 0.85, 1.00}},
    };
    return f;
}

const Family& family_of(const std::string& name) {
    auto it = families().find(name);
    if (it == families().end()) throw std::runtime_error("unknown diffusion filter family '" + name + "'");
    return it->second;
}

Family resolve_family(const DiffusionFilterParams& p) {
    Family f = family_of(p.filter_family);
    double ci = p.core_intensity, hi = p.halo_intensity, bi = p.bloom_intensity;
    double cs = p.core_size, hs = p.halo_size, bs = p.bloom_size;
    if (ci == 1.0 && hi == 1.0 && bi == 1.0 && cs == 1.0 && hs == 1.0 && bs == 1.0) return f;
    double w_c = f.w_c * std::max(ci, 0.0), w_h = f.w_h * std::max(hi, 0.0),
           w_b = f.w_b * std::max(bi, 0.0);
    double total = w_c + w_h + w_b;
    if (total <= 0.0) return f;
    f.core.lambda_um *= std::max(cs, 1e-6);
    f.halo.lambda_um *= std::max(hs, 1e-6);
    f.bloom.lambda_um *= std::max(bs, 1e-6);
    f.w_c = w_c / total;
    f.w_h = w_h / total;
    f.w_b = w_b / total;
    return f;
}

void expand_group(const Group& g, bool bloom, Vec& lambdas, Vec& weights) {
    int n = std::max(g.n, 1);
    if (n == 1 || g.spread <= 1.0) {
        lambdas = {g.lambda_um};
        weights = {1.0};
        return;
    }
    Vec logs = linspace(std::log(g.lambda_um / g.spread), std::log(g.lambda_um * g.spread), n);
    lambdas.resize(n);
    weights.resize(n);
    double sum = 0;
    for (int i = 0; i < n; ++i) {
        lambdas[i] = std::exp(logs[i]);
        weights[i] = bloom ? std::pow(lambdas[i], 2.0 - g.alpha) : 1.0;
        sum += weights[i];
    }
    for (auto& w : weights) w /= sum;
}

// _halo_channel_weights: [3][n]
std::vector<Vec> halo_channel_weights(const Vec& weights, double warmth) {
    static const double axis[3] = {+1.30, +0.15, -1.45};
    size_t n = weights.size();
    std::vector<Vec> out(3, weights);
    if (n < 2) return out;
    warmth = std::clamp(warmth, -1.5, 1.5);
    Vec g = linspace(-1.0, 1.0, static_cast<int>(n));
    double wsum = 0, gw = 0;
    for (size_t i = 0; i < n; ++i) {
        wsum += weights[i];
        gw += g[i] * weights[i];
    }
    double avg = gw / wsum;
    for (auto& v : g) v -= avg;
    for (int c = 0; c < 3; ++c) {
        Vec raw(n);
        double s = 0;
        for (size_t i = 0; i < n; ++i) {
            raw[i] = std::max(weights[i] * (1.0 + warmth * axis[c] * g[i]), 0.0);
            s += raw[i];
        }
        if (s > 0.0)
            for (size_t i = 0; i < n; ++i) out[c][i] = raw[i] * (wsum / s);
    }
    return out;
}

}  // namespace

double diffusion_strength_to_scatter(double strength, const std::string& family) {
    if (strength <= 0) return 0.0;
    static const Vec breaks = {0.125, 0.25, 0.5, 1.0, 2.0};
    static const Vec frac = {0.10, 0.20, 0.35, 0.55, 0.75};
    double ls = std::log2(std::max(strength, 1e-6));
    Vec lb(breaks.size());
    for (size_t i = 0; i < breaks.size(); ++i) lb[i] = std::log2(breaks[i]);
    double base = np_interp(ls, lb, frac);
    auto it = families().find(family);
    double gain = it == families().end() ? 1.0 : it->second.gain;
    return std::clamp(base * gain, 0.0, 0.99);
}

Image diffusion_filter_psf(int size, const DiffusionFilterParams& p, double pixel_size_um) {
    Family f = resolve_family(p);
    double warmth = f.warmth_base + p.halo_warmth;
    double scale = std::max(p.spatial_scale, 1e-6);
    Vec cl, cw, hl, hw, bl, bw;
    expand_group(f.core, false, cl, cw);
    expand_group(f.halo, false, hl, hw);
    expand_group(f.bloom, true, bl, bw);
    std::vector<Vec> halo_w = halo_channel_weights(hw, warmth);
    auto to_px = [&](Vec& l) {
        for (auto& v : l) v = std::max(v * scale / pixel_size_um, 1e-6);
    };
    to_px(cl);
    to_px(hl);
    to_px(bl);
    Image psf(size, size, 3);
    int c0 = size / 2;
    auto exp_sum = [](double r, const Vec& lambdas, const Vec& weights) {
        double total = 0;
        for (size_t k = 0; k < lambdas.size(); ++k)
            total += weights[k] * std::exp(-r / lambdas[k]) / (2.0 * kPi * lambdas[k] * lambdas[k]);
        return total;
    };
    parallel_for(size, [&](long y0, long y1) {
        for (long y = y0; y < y1; ++y)
            for (int x = 0; x < size; ++x) {
                double dx = x - c0, dy = static_cast<double>(y) - c0;
                double r = std::sqrt(dx * dx + dy * dy);
                double core = f.w_c * exp_sum(r, cl, cw);
                double bloom = f.w_b * exp_sum(r, bl, bw);
                for (int c = 0; c < 3; ++c)
                    psf.at(static_cast<int>(y), x, c) = core + f.w_h * exp_sum(r, hl, halo_w[c]) + bloom;
            }
    });
    for (int c = 0; c < 3; ++c) {
        double s = 0;
        for (size_t i = 0; i < psf.pixels(); ++i) s += psf.v[i * 3 + c];
        for (size_t i = 0; i < psf.pixels(); ++i) psf.v[i * 3 + c] /= s;
    }
    return psf;
}

void fft_convolve_reflect(const double* in, double* out, int h, int w, const double* kernel,
                          int radius) {
    using pocketfft::detail::util;
    int ks = 2 * radius + 1;
    size_t fh = util::good_size_real(static_cast<size_t>(h) + 4 * static_cast<size_t>(radius));
    size_t fw = util::good_size_real(static_cast<size_t>(w) + 4 * static_cast<size_t>(radius));
    std::vector<double> a(fh * fw, 0.0), b(fh * fw, 0.0);
    auto refl = [](int i, int n) {
        if (n == 1) return 0;
        int period = 2 * (n - 1);
        i %= period;
        if (i < 0) i += period;
        return i >= n ? period - i : i;
    };
    int ph = h + 2 * radius, pw = w + 2 * radius;
    for (int y = 0; y < ph; ++y) {
        const double* row = in + static_cast<size_t>(refl(y - radius, h)) * w;
        double* dst = a.data() + static_cast<size_t>(y) * fw;
        for (int x = 0; x < pw; ++x) dst[x] = row[refl(x - radius, w)];
    }
    for (int y = 0; y < ks; ++y)
        std::copy(kernel + static_cast<size_t>(y) * ks, kernel + static_cast<size_t>(y + 1) * ks,
                  b.data() + static_cast<size_t>(y) * fw);
    pocketfft::shape_t shape{fh, fw}, axes{0, 1};
    pocketfft::stride_t sr{static_cast<ptrdiff_t>(fw * sizeof(double)), sizeof(double)};
    size_t cw = fw / 2 + 1;
    pocketfft::stride_t sc{static_cast<ptrdiff_t>(cw * sizeof(std::complex<double>)),
                           sizeof(std::complex<double>)};
    std::vector<std::complex<double>> fa(fh * cw), fb(fh * cw);
    size_t nthreads = static_cast<size_t>(std::max(1, thread_count()));
    pocketfft::r2c(shape, sr, sc, axes, pocketfft::FORWARD, a.data(), fa.data(), 1.0, nthreads);
    pocketfft::r2c(shape, sr, sc, axes, pocketfft::FORWARD, b.data(), fb.data(), 1.0, nthreads);
    for (size_t i = 0; i < fa.size(); ++i) fa[i] *= fb[i];
    pocketfft::c2r(shape, sc, sr, axes, pocketfft::BACKWARD, fa.data(), a.data(),
                   1.0 / static_cast<double>(fh * fw), nthreads);
    // full[2r + i] = sum_d padded[i + d] * kernel[2r - d]
    for (int y = 0; y < h; ++y)
        std::copy(a.data() + static_cast<size_t>(y + 2 * radius) * fw + 2 * radius,
                  a.data() + static_cast<size_t>(y + 2 * radius) * fw + 2 * radius + w,
                  out + static_cast<size_t>(y) * w);
}

Image apply_diffusion_filter_um(const Image& img, const DiffusionFilterParams& p,
                                double pixel_size_um, bool clamp_radius) {
    if (!p.active) return img;
    if (p.strength <= 0 || p.spatial_scale <= 0) return img;
    family_of(p.filter_family);
    double p_s = diffusion_strength_to_scatter(p.strength, p.filter_family);
    if (p_s <= 0) return img;
    Family f = resolve_family(p);
    double bloom_max_px = f.bloom.lambda_um * f.bloom.spread * p.spatial_scale / pixel_size_um;
    int radius = static_cast<int>(std::ceil(std::max(8.0 * bloom_max_px, 5.0)));
    if (clamp_radius) radius = std::min(radius, std::max(std::min(img.h, img.w) / 2 - 1, 1));
    int size = 2 * radius + 1;
    Image psf = diffusion_filter_psf(size, p, pixel_size_um);
    Image out(img.h, img.w, img.c);
    size_t n = img.pixels();
    Vec plane(n), blurred(n), kernel(static_cast<size_t>(size) * size);
    for (int c = 0; c < img.c; ++c) {
        for (size_t i = 0; i < n; ++i) plane[i] = img.v[i * img.c + c];
        for (size_t i = 0; i < kernel.size(); ++i) kernel[i] = psf.v[i * 3 + c];
        fft_convolve_reflect(plane.data(), blurred.data(), img.h, img.w, kernel.data(), radius);
        for (size_t i = 0; i < n; ++i)
            out.v[i * img.c + c] = img.v[i * img.c + c] * (1.0 - p_s) + blurred[i] * p_s;
    }
    return out;
}

}  // namespace spectra
