// SPDX-License-Identifier: GPL-3.0-or-later
#include "spectra/model/filters.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "spectra/core/parallel.hpp"

namespace spectra {

namespace {

inline int reflect_index(int i, int n) {
    if (i >= 0 && i < n) return i;
    if (i >= -n && i < 0) return -i - 1;
    if (i >= n && i < 2 * n) return 2 * n - 1 - i;
    int period = 2 * n;
    i %= period;
    if (i < 0) i += period;
    if (i >= n) i = period - 1 - i;
    return i;
}

inline int mirror_index(int i, int n) {
    if (n == 1) return 0;
    int period = 2 * (n - 1);
    i %= period;
    if (i < 0) i += period;
    if (i >= n) i = period - i;
    return i;
}

Vec gaussian_kernel(double sigma, double truncate, int& radius) {
    radius = static_cast<int>(truncate * sigma + 0.5);
    Vec k(2 * radius + 1);
    double total = 0;
    for (int i = 0; i <= 2 * radius; ++i) {
        double x = i - radius;
        double r = x / sigma;
        k[i] = std::exp(-0.5 * r * r);
        total += k[i];
    }
    for (auto& v : k) v /= total;
    return k;
}

void fir_2d(const double* image, double* output, int n, int m, const Vec& kernel, int radius) {
    parallel_for(n, [&](long i0, long i1) {
        Vec tmp(m);
        for (long i = i0; i < i1; ++i) {
            std::fill(tmp.begin(), tmp.end(), 0.0);
            for (int k = -radius; k <= radius; ++k) {
                double kw = kernel[k + radius];
                const double* row = image + static_cast<size_t>(reflect_index(static_cast<int>(i) + k, n)) * m;
                for (int j = 0; j < m; ++j) tmp[j] += row[j] * kw;
            }
            double* orow = output + static_cast<size_t>(i) * m;
            for (int j = 0; j < m; ++j) {
                double s = 0.0;
                if (j >= radius && j < m - radius) {
                    for (int k = -radius; k <= radius; ++k) s += tmp[j + k] * kernel[k + radius];
                } else {
                    for (int k = -radius; k <= radius; ++k)
                        s += tmp[reflect_index(j + k, m)] * kernel[k + radius];
                }
                orow[j] = s;
            }
        }
    });
}

void yvv_coeffs(double sigma, double& B, double& B1, double& B2, double& B3) {
    double q = sigma >= 2.5 ? 0.98711 * sigma - 0.96330
                            : 3.97156 - 4.14554 * std::sqrt(1.0 - 0.26891 * sigma);
    double q2 = q * q, q3 = q2 * q;
    double b0 = 1.57825 + 2.44413 * q + 1.4281 * q2 + 0.422205 * q3;
    double b1 = 2.44413 * q + 2.85619 * q2 + 1.26661 * q3;
    double b2 = -(1.4281 * q2 + 1.26661 * q3);
    double b3 = 0.422205 * q3;
    B = 1.0 - (b1 + b2 + b3) / b0;
    B1 = b1 / b0;
    B2 = b2 / b0;
    B3 = b3 / b0;
}

void iir_line(const double* in, double* out, int n, int stride, double B, double B1, double B2,
              double B3) {
    double x0 = in[0];
    double w1 = x0, w2 = x0, w3 = x0;
    for (int j = 0; j < n; ++j) {
        double w = B * in[static_cast<size_t>(j) * stride] + B1 * w1 + B2 * w2 + B3 * w3;
        out[static_cast<size_t>(j) * stride] = w;
        w3 = w2;
        w2 = w1;
        w1 = w;
    }
    double xn = out[static_cast<size_t>(n - 1) * stride];
    double y1 = xn, y2 = xn, y3 = xn;
    for (int j = n - 1; j >= 0; --j) {
        double y = B * out[static_cast<size_t>(j) * stride] + B1 * y1 + B2 * y2 + B3 * y3;
        out[static_cast<size_t>(j) * stride] = y;
        y3 = y2;
        y2 = y1;
        y1 = y;
    }
}

void iir_2d(const double* image, double* output, int n, int m, double sigma) {
    double B, B1, B2, B3;
    yvv_coeffs(sigma, B, B1, B2, B3);
    Vec tmp(static_cast<size_t>(n) * m);
    parallel_for(n, [&](long i0, long i1) {
        for (long i = i0; i < i1; ++i)
            iir_line(image + static_cast<size_t>(i) * m, tmp.data() + static_cast<size_t>(i) * m, m, 1,
                     B, B1, B2, B3);
    });
    parallel_for(m, [&](long j0, long j1) {
        for (long j = j0; j < j1; ++j) iir_line(tmp.data() + j, output + j, n, m, B, B1, B2, B3);
    });
}

}  // namespace

void gaussian_filter_plane(const double* in, double* out, int h, int w, double sigma,
                           double truncate) {
    size_t n = static_cast<size_t>(h) * w;
    if (sigma <= 0.0) {
        if (in != out) std::copy(in, in + n, out);
        return;
    }
    Vec src;
    if (in == out) {
        src.assign(in, in + n);
        in = src.data();
    }
    if (sigma >= 3.0) {
        iir_2d(in, out, h, w, sigma);
    } else {
        int radius;
        Vec k = gaussian_kernel(sigma, truncate, radius);
        fir_2d(in, out, h, w, k, radius);
    }
}

Image gaussian_filter(const Image& img, const Vec& sigma, double truncate) {
    if (static_cast<int>(sigma.size()) != img.c)
        throw std::runtime_error("sigma length does not match channel count");
    Image out(img.h, img.w, img.c);
    size_t n = img.pixels();
    Vec a(n), b(n);
    for (int k = 0; k < img.c; ++k) {
        for (size_t i = 0; i < n; ++i) a[i] = img.v[i * img.c + k];
        gaussian_filter_plane(a.data(), b.data(), img.h, img.w, sigma[k], truncate);
        for (size_t i = 0; i < n; ++i) out.v[i * img.c + k] = b[i];
    }
    return out;
}

Image gaussian_filter(const Image& img, double sigma, double truncate) {
    return gaussian_filter(img, Vec(img.c, sigma), truncate);
}

Image exponential_filter(const Image& img, const Vec& decay) {
    static const double fit[3][2] = {{0.1633, 0.5360}, {0.6496, 1.5236}, {0.1870, 2.7684}};
    Image result;
    for (int g = 0; g < 3; ++g) {
        Vec s(decay.size());
        for (size_t i = 0; i < s.size(); ++i) s[i] = fit[g][1] * decay[i];
        Image comp = gaussian_filter(img, s, 3.0);
        if (g == 0) {
            result = std::move(comp);
            for (auto& v : result.v) v = fit[0][0] * v;
        } else {
            for (size_t i = 0; i < result.v.size(); ++i) result.v[i] += fit[g][0] * comp.v[i];
        }
    }
    return result;
}

void scipy_gaussian_filter1d(const double* in, double* out, int n, int stride, double sigma,
                             int mode, double truncate) {
    int radius = static_cast<int>(truncate * sigma + 0.5);
    Vec k(2 * radius + 1);
    double total = 0;
    double sigma2 = sigma * sigma;
    for (int i = -radius; i <= radius; ++i) {
        k[i + radius] = std::exp(-0.5 / sigma2 * i * i);
        total += k[i + radius];
    }
    for (auto& v : k) v /= total;
    Vec src(n);
    for (int i = 0; i < n; ++i) src[i] = in[static_cast<size_t>(i) * stride];
    for (int i = 0; i < n; ++i) {
        double s = 0;
        for (int j = -radius; j <= radius; ++j) {
            int idx = mode == 0 ? reflect_index(i + j, n) : mirror_index(i + j, n);
            s += src[idx] * k[j + radius];
        }
        out[static_cast<size_t>(i) * stride] = s;
    }
}

Image unsharp_mask(const Image& img, double sigma, double amount) {
    Image blur = gaussian_filter(img, sigma);
    Image out(img.h, img.w, img.c);
    for (size_t i = 0; i < img.v.size(); ++i) out.v[i] = img.v[i] + amount * (img.v[i] - blur.v[i]);
    return out;
}

Image multiplicative_unsharp_mask(const Image& img, double sigma, double amount) {
    if (sigma <= 0 || amount <= 0) return img;
    const double eps = 1e-6;
    Image d(img.h, img.w, img.c);
    for (size_t i = 0; i < img.v.size(); ++i) d.v[i] = std::max(img.v[i], 0.0);
    Image blur = gaussian_filter(d, sigma);
    Image out(img.h, img.w, img.c);
    for (size_t i = 0; i < img.v.size(); ++i) {
        double b = std::max(blur.v[i], eps);
        out.v[i] = d.v[i] <= 0.0 ? 0.0
                                 : std::pow(std::max(d.v[i], eps), 1.0 + amount) / std::pow(b, amount);
    }
    size_t n = img.pixels();
    for (int k = 0; k < img.c; ++k) {
        double s = 0, sd = 0;
        for (size_t i = 0; i < n; ++i) {
            s += out.v[i * img.c + k];
            sd += d.v[i * img.c + k];
        }
        if (s > 0) {
            double f = sd / s;
            for (size_t i = 0; i < n; ++i) out.v[i * img.c + k] *= f;
        }
    }
    return out;
}

Vec match_channels(const Vec3& values, int n_ch) {
    if (n_ch == 3) return {values[0], values[1], values[2]};
    if (n_ch == 1) return {values[0]};
    throw std::runtime_error("cannot adapt 3 per-channel values to " + std::to_string(n_ch) +
                             " channels");
}

}  // namespace spectra
