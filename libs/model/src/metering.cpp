// SPDX-License-Identifier: GPL-3.0-or-later
#include "spectra/model/metering.hpp"

#include <algorithm>
#include <cmath>
#include <array>
#include <string>
#include <vector>
#include <limits>
#include <stdexcept>

#include "spectra/core/parallel.hpp"
#include "spectra/model/filters.hpp"

namespace spectra {

namespace {

// Returns false when the method is unknown (metered value "None").
bool metered_value(const Vec& f, int h, int w, const std::string& method, double& value) {
    size_t n = f.size();
    auto mean_all = [&] {
        double s = 0;
        for (double v : f) s += v;
        return s / static_cast<double>(n);
    };
    double mx = std::max(h, w);
    double ny = h / mx, nx = w / mx;
    auto xc = [&](int j) { return (static_cast<double>(j) / w - 0.5) * nx; };
    auto yc = [&](int i) { return (static_cast<double>(i) / h - 0.5) * ny; };
    if (method == "average") {
        value = mean_all();
        return true;
    }
    if (method == "median") {
        Vec s(f);
        std::sort(s.begin(), s.end());
        value = n % 2 ? s[n / 2] : 0.5 * (s[n / 2 - 1] + s[n / 2]);
        return true;
    }
    if (method == "center_weighted") {
        const double sigma = 0.2;
        double msum = 0, acc = 0;
        Vec mask(n);
        for (int i = 0; i < h; ++i)
            for (int j = 0; j < w; ++j) {
                double x = xc(j), y = yc(i);
                double m = std::exp(-(x * x + y * y) / (2 * sigma * sigma));
                mask[static_cast<size_t>(i) * w + j] = m;
                msum += m;
            }
        for (size_t i = 0; i < n; ++i) acc += f[i] * (mask[i] / msum);
        value = acc;
        return true;
    }
    if (method == "partial") {
        double s = 0;
        long cnt = 0;
        for (int i = 0; i < h; ++i)
            for (int j = 0; j < w; ++j) {
                double x = xc(j), y = yc(i);
                if (std::sqrt(x * x + y * y) < 0.15) {
                    s += f[static_cast<size_t>(i) * w + j];
                    ++cnt;
                }
            }
        value = cnt ? s / cnt : mean_all();
        return true;
    }
    if (method == "matrix") {
        const int rows = 5, cols = 5;
        int ch = h / rows, cw = w / cols;
        Vec means, weights;
        for (int r = 0; r < rows; ++r)
            for (int c = 0; c < cols; ++c) {
                if (ch == 0 || cw == 0) continue;
                double s = 0;
                for (int i = r * ch; i < (r + 1) * ch; ++i)
                    for (int j = c * cw; j < (c + 1) * cw; ++j) s += f[static_cast<size_t>(i) * w + j];
                means.push_back(s / (static_cast<double>(ch) * cw));
                double dy = (r - (rows - 1) / 2.0) / ((rows - 1) / 2.0);
                double dx = (c - (cols - 1) / 2.0) / ((cols - 1) / 2.0);
                double dist = std::sqrt(dx * dx + dy * dy) / std::sqrt(2.0);
                weights.push_back(0.5 * (1.0 + std::cos(kPi * dist)));
            }
        double ws = 0, acc = 0;
        for (double v : weights) ws += v;
        for (size_t i = 0; i < means.size(); ++i) acc += (weights[i] / ws) * means[i];
        value = acc;  // 0 when there are no zones, as numpy.dot of empty arrays
        return true;
    }
    if (method == "multi_zone") {
        const double bounds[3][2] = {{0.00, 0.05}, {0.05, 0.25}, {0.25, 0.50}};
        const double rw[3] = {0.50, 0.30, 0.20};
        double sums[3] = {0, 0, 0};
        long cnts[3] = {0, 0, 0};
        for (int i = 0; i < h; ++i)
            for (int j = 0; j < w; ++j) {
                double x = xc(j), y = yc(i);
                double r = std::sqrt(x * x + y * y);
                for (int z = 0; z < 3; ++z)
                    if (r >= bounds[z][0] && r < bounds[z][1]) {
                        sums[z] += f[static_cast<size_t>(i) * w + j];
                        ++cnts[z];
                    }
            }
        double ws = 0, wt = 0;
        for (int z = 0; z < 3; ++z)
            if (cnts[z]) {
                ws += rw[z] * (sums[z] / cnts[z]);
                wt += rw[z];
            }
        value = wt > 0 ? ws / wt : mean_all();
        return true;
    }
    if (method == "highlight_weighted") {
        double total = 0, acc = 0;
        for (double v : f) total += v * v;
        if (total < 1e-12) {
            value = mean_all();
            return true;
        }
        for (double v : f) acc += v * (v * v);
        value = acc / total;
        return true;
    }
    return false;
}

}  // namespace

double measure_raw_autoexposure_ev(const Image& raw, const std::string& method, double target) {
    Vec field(raw.pixels());
    for (size_t i = 0; i < field.size(); ++i) {
        double s = 0;
        for (int k = 0; k < raw.c; ++k) s += raw.v[i * raw.c + k];
        field[i] = s / raw.c;
    }
    double metered;
    if (!metered_value(field, raw.h, raw.w, method, metered)) return 0.0;
    double ev = -std::log2(metered / target);
    if (std::isinf(ev)) return 0.0;
    return ev;
}

Image metering_preview(const Image& img, int max_size) {
    int mx = std::max(img.h, img.w);
    if (mx <= max_size) return img;
    double scale = static_cast<double>(max_size) / mx;
    int oh = static_cast<int>(std::max(round_half_even(scale * img.h), 1.0));
    int ow = static_cast<int>(std::max(round_half_even(scale * img.w), 1.0));
    double fh = static_cast<double>(img.h) / oh, fw = static_cast<double>(img.w) / ow;
    double sh = std::max(0.0, (fh - 1) / 2), sw = std::max(0.0, (fw - 1) / 2);
    Image filt = img;
    int c = img.c;
    if (sh > 1e-15) {
        Image tmp(img.h, img.w, c);
        parallel_for(img.w, [&](long a, long b) {
            for (long x = a; x < b; ++x)
                for (int k = 0; k < c; ++k)
                    scipy_gaussian_filter1d(&filt.v[static_cast<size_t>(x) * c + k],
                                            &tmp.v[static_cast<size_t>(x) * c + k], img.h, img.w * c, sh, 1);
        });
        filt = std::move(tmp);
    }
    if (sw > 1e-15) {
        Image tmp(img.h, img.w, c);
        parallel_for(img.h, [&](long a, long b) {
            for (long y = a; y < b; ++y)
                for (int k = 0; k < c; ++k)
                    scipy_gaussian_filter1d(&filt.v[(static_cast<size_t>(y) * img.w) * c + k],
                                            &tmp.v[(static_cast<size_t>(y) * img.w) * c + k], img.w, c, sw, 1);
        });
        filt = std::move(tmp);
    }
    Image out(oh, ow, c);
    for (int y = 0; y < oh; ++y) {
        int sy = std::clamp(static_cast<int>(std::floor((y + 0.5) * fh - 0.5 + 0.5)), 0, img.h - 1);
        for (int x = 0; x < ow; ++x) {
            int sx = std::clamp(static_cast<int>(std::floor((x + 0.5) * fw - 0.5 + 0.5)), 0, img.w - 1);
            for (int k = 0; k < c; ++k) out.at(y, x, k) = filt.at(sy, sx, k);
        }
    }
    return out;
}

namespace {

// scipy.ndimage cubic spline prefilter along one strided line, mirror boundary.
void spline_prefilter_mirror(double* c, int n, size_t stride) {
    if (n < 2) return;
    const double z = std::sqrt(3.0) - 2.0;
    auto at = [&](int i) -> double& { return c[static_cast<size_t>(i) * stride]; };
    const double gain = (1.0 - z) * (1.0 - 1.0 / z);
    for (int i = 0; i < n; ++i) at(i) *= gain;
    double z_i = z;
    const double z_n_1 = std::pow(z, n - 1);
    at(0) = at(0) + z_n_1 * at(n - 1);
    for (int i = 1; i < n - 1; ++i) {
        at(0) += z_i * (at(i) + z_n_1 * at(n - 1 - i));
        z_i *= z;
    }
    at(0) /= 1 - z_n_1 * z_n_1;
    for (int i = 1; i < n; ++i) at(i) += z * at(i - 1);
    at(n - 1) = (z * at(n - 2) + at(n - 1)) * z / (z * z - 1);
    for (int i = n - 2; i >= 0; --i) at(i) = z * (at(i + 1) - at(i));
}

inline int mirror_idx(int i, int n) {
    if (n == 1) return 0;
    int period = 2 * (n - 1);
    i %= period;
    if (i < 0) i += period;
    return i >= n ? period - i : i;
}

// Sample positions and cubic B-spline weights of ndimage.zoom(grid_mode=True, mode='mirror').
void zoom_taps(int n_in, int n_out, std::vector<int>& idx, Vec& w) {
    idx.resize(static_cast<size_t>(n_out) * 4);
    w.resize(static_cast<size_t>(n_out) * 4);
    double zoom = static_cast<double>(n_in) / n_out;
    for (int o = 0; o < n_out; ++o) {
        double cc = (o + 0.5) * zoom - 0.5;
        if (n_in > 1) {
            double period = 2.0 * (n_in - 1);
            if (cc < 0) cc = -cc;
            if (cc > n_in - 1) {
                cc = std::fmod(cc, period);
                if (cc > n_in - 1) cc = period - cc;
            }
        } else {
            cc = 0;
        }
        int start = static_cast<int>(std::floor(cc)) - 1;
        double t = cc - std::floor(cc);
        double ws[4] = {(1 - t) * (1 - t) * (1 - t) / 6.0, (3 * t * t * t - 6 * t * t + 4) / 6.0,
                        (-3 * t * t * t + 3 * t * t + 3 * t + 1) / 6.0, t * t * t / 6.0};
        for (int k = 0; k < 4; ++k) {
            idx[static_cast<size_t>(o) * 4 + k] = mirror_idx(start + k, n_in);
            w[static_cast<size_t>(o) * 4 + k] = ws[k];
        }
    }
}

}  // namespace

Image rescale_cubic(const Image& img, double factor) {
    int oh = static_cast<int>(std::max(round_half_even(factor * img.h), 1.0));
    int ow = static_cast<int>(std::max(round_half_even(factor * img.w), 1.0));
    int c = img.c;
    double lo = INFINITY, hi = -INFINITY;
    for (double v : img.v) {
        lo = std::min(lo, v);
        hi = std::max(hi, v);
    }
    Image f = img;
    // Anti-aliasing when reducing (skimage: sigma = (in/out - 1) / 2, mirror, truncate 4).
    if (oh < img.h || ow < img.w) {
        double sh = std::max(0.0, (static_cast<double>(img.h) / oh - 1) / 2);
        double sw = std::max(0.0, (static_cast<double>(img.w) / ow - 1) / 2);
        if (sh > 1e-15) {
            Image tmp(img.h, img.w, c);
            parallel_for(img.w, [&](long a, long b) {
                for (long x = a; x < b; ++x)
                    for (int k = 0; k < c; ++k)
                        scipy_gaussian_filter1d(&f.v[static_cast<size_t>(x) * c + k], &tmp.v[static_cast<size_t>(x) * c + k],
                                                img.h, img.w * c, sh, 1);
            });
            f = std::move(tmp);
        }
        if (sw > 1e-15) {
            Image tmp(img.h, img.w, c);
            parallel_for(img.h, [&](long a, long b) {
                for (long y = a; y < b; ++y)
                    for (int k = 0; k < c; ++k)
                        scipy_gaussian_filter1d(&f.v[(static_cast<size_t>(y) * img.w) * c + k],
                                                &tmp.v[(static_cast<size_t>(y) * img.w) * c + k], img.w, c, sw, 1);
            });
            f = std::move(tmp);
        }
    }
    // Spline prefilter along both image axes.
    parallel_for(img.w, [&](long a, long b) {
        for (long x = a; x < b; ++x)
            for (int k = 0; k < c; ++k)
                spline_prefilter_mirror(&f.v[static_cast<size_t>(x) * c + k], img.h, static_cast<size_t>(img.w) * c);
    });
    parallel_for(img.h, [&](long a, long b) {
        for (long y = a; y < b; ++y)
            for (int k = 0; k < c; ++k)
                spline_prefilter_mirror(&f.v[(static_cast<size_t>(y) * img.w) * c + k], img.w, static_cast<size_t>(c));
    });
    std::vector<int> iy, ix;
    Vec wy, wx;
    zoom_taps(img.h, oh, iy, wy);
    zoom_taps(img.w, ow, ix, wx);
    Image out(oh, ow, c);
    parallel_for(oh, [&](long a, long b) {
        for (long y = a; y < b; ++y)
            for (int x = 0; x < ow; ++x)
                for (int k = 0; k < c; ++k) {
                    double s = 0;
                    for (int j = 0; j < 4; ++j) {
                        double row = 0;
                        const double* src = &f.v[(static_cast<size_t>(iy[y * 4 + j]) * img.w) * c + k];
                        for (int i = 0; i < 4; ++i) row += wx[static_cast<size_t>(x) * 4 + i] * src[static_cast<size_t>(ix[static_cast<size_t>(x) * 4 + i]) * c];
                        s += wy[static_cast<size_t>(y) * 4 + j] * row;
                    }
                    out.at(static_cast<int>(y), x, k) = std::clamp(s, lo, hi);
                }
    });
    return out;
}

Image crop_image(const Image& img, const std::array<double, 2>& center,
                 const std::array<double, 2>& size) {
    double shape[2] = {static_cast<double>(img.h), static_cast<double>(img.w)};
    double cn[2] = {round_half_even(shape[0] * center[1]), round_half_even(shape[1] * center[0])};
    double mx = std::max(shape[0], shape[1]);
    double szd[2] = {round_half_even(mx * size[1]), round_half_even(mx * size[0])};
    long x0[2], sz[2];
    for (int i = 0; i < 2; ++i) {
        x0[i] = static_cast<long>(round_half_even(cn[i] - szd[i] / 2));
        sz[i] = static_cast<long>(szd[i]);
        if (x0[i] < 0) x0[i] = 0;
        if (x0[i] + sz[i] > static_cast<long>(shape[i])) x0[i] = static_cast<long>(shape[i]) - sz[i];
        if (x0[i] < 0) {
            x0[i] = 0;
            sz[i] = static_cast<long>(shape[i]);
        }
    }
    Image out(static_cast<int>(sz[0]), static_cast<int>(sz[1]), img.c);
    for (int y = 0; y < out.h; ++y)
        std::copy(img.px(static_cast<int>(x0[0]) + y, static_cast<int>(x0[1])),
                  img.px(static_cast<int>(x0[0]) + y, static_cast<int>(x0[1])) + static_cast<size_t>(out.w) * img.c,
                  out.px(y, 0));
    return out;
}

Image downsample_area(const Image& img, int long_edge) {
    int mx = std::max(img.h, img.w);
    if (long_edge <= 0 || mx <= long_edge) return img;
    double scale = static_cast<double>(mx) / long_edge;
    int oh = std::max(1, static_cast<int>(std::lround(img.h / scale)));
    int ow = std::max(1, static_cast<int>(std::lround(img.w / scale)));
    double sy = static_cast<double>(img.h) / oh, sx = static_cast<double>(img.w) / ow;
    Image out(oh, ow, img.c);
    parallel_for(oh, [&](long a, long b) {
        Vec acc(img.c);
        for (long y = a; y < b; ++y) {
            double y0 = y * sy, y1 = (y + 1) * sy;
            int iy0 = static_cast<int>(std::floor(y0)), iy1 = std::min(img.h, static_cast<int>(std::ceil(y1)));
            for (int x = 0; x < ow; ++x) {
                double x0 = x * sx, x1 = (x + 1) * sx;
                int ix0 = static_cast<int>(std::floor(x0)), ix1 = std::min(img.w, static_cast<int>(std::ceil(x1)));
                std::fill(acc.begin(), acc.end(), 0.0);
                double wsum = 0;
                for (int iy = iy0; iy < iy1; ++iy) {
                    double wy = std::min<double>(iy + 1, y1) - std::max<double>(iy, y0);
                    for (int ix = ix0; ix < ix1; ++ix) {
                        double w = wy * (std::min<double>(ix + 1, x1) - std::max<double>(ix, x0));
                        const double* p = img.px(iy, ix);
                        for (int k = 0; k < img.c; ++k) acc[k] += w * p[k];
                        wsum += w;
                    }
                }
                double* o = out.px(static_cast<int>(y), x);
                for (int k = 0; k < img.c; ++k) o[k] = acc[k] / wsum;
            }
        }
    });
    return out;
}

Lut3D create_lut_3d(const PixelFn& fn, int n_out, const Vec& xmin, const Vec& xmax, int steps) {
    Lut3D lut;
    lut.steps = steps;
    lut.n_out = n_out;
    for (int k = 0; k < 3; ++k) {
        lut.xmin[k] = xmin[k];
        lut.xmax[k] = xmax[k];
        if (!(xmax[k] > xmin[k])) throw std::runtime_error("LUT: xmax must be greater than xmin");
    }
    Vec xr = linspace(xmin[0], xmax[0], steps), xg = linspace(xmin[1], xmax[1], steps),
        xb = linspace(xmin[2], xmax[2], steps);
    lut.v.resize(static_cast<size_t>(steps) * steps * steps * n_out);
    parallel_for(steps, [&](long a, long b) {
        for (long i = a; i < b; ++i)
            for (int j = 0; j < steps; ++j)
                for (int k = 0; k < steps; ++k) {
                    double in[3] = {xr[i], xg[j], xb[k]};
                    fn(in, &lut.v[((static_cast<size_t>(i) * steps + j) * steps + k) * n_out]);
                }
    });
    return lut;
}

Image apply_lut_3d(const Lut3D& lut, const Image& data) {
    int S = lut.steps, n_out = lut.n_out;
    Image out(data.h, data.w, n_out);
    auto base_frac = [S](double coord, int& base, double& frac) {
        if (coord <= 0.0) coord = 0.0;
        double upper = S - 1;
        if (coord >= upper) {
            base = S - 2;
            frac = 1.0;
            return;
        }
        base = static_cast<int>(std::floor(coord));
        frac = coord - base;
    };
    double scale = S - 1;
    parallel_for(static_cast<long>(data.pixels()), [&](long a, long b) {
        for (long p = a; p < b; ++p) {
            const double* d = &data.v[static_cast<size_t>(p) * data.c];
            int i, j, k;
            double fr, fg, fb;
            base_frac((d[0] - lut.xmin[0]) / (lut.xmax[0] - lut.xmin[0]) * scale, i, fr);
            base_frac((d[1] - lut.xmin[1]) / (lut.xmax[1] - lut.xmin[1]) * scale, j, fg);
            base_frac((d[2] - lut.xmin[2]) / (lut.xmax[2] - lut.xmin[2]) * scale, k, fb);
            auto L = [&](int ii, int jj, int kk, int c) {
                return lut.v[((static_cast<size_t>(ii) * S + jj) * S + kk) * n_out + c];
            };
            double* o = &out.v[static_cast<size_t>(p) * n_out];
            for (int c = 0; c < n_out; ++c) {
                double c000 = L(i, j, k, c), c111 = L(i + 1, j + 1, k + 1, c);
                if (fr > fg) {
                    if (fg > fb) {
                        double c100 = L(i + 1, j, k, c), c110 = L(i + 1, j + 1, k, c);
                        o[c] = c000 + fr * (c100 - c000) + fg * (c110 - c100) + fb * (c111 - c110);
                    } else if (fr > fb) {
                        double c100 = L(i + 1, j, k, c), c101 = L(i + 1, j, k + 1, c);
                        o[c] = c000 + fr * (c100 - c000) + fb * (c101 - c100) + fg * (c111 - c101);
                    } else {
                        double c001 = L(i, j, k + 1, c), c101 = L(i + 1, j, k + 1, c);
                        o[c] = c000 + fb * (c001 - c000) + fr * (c101 - c001) + fg * (c111 - c101);
                    }
                } else {
                    if (fb > fg) {
                        double c001 = L(i, j, k + 1, c), c011 = L(i, j + 1, k + 1, c);
                        o[c] = c000 + fb * (c001 - c000) + fg * (c011 - c001) + fr * (c111 - c011);
                    } else if (fb > fr) {
                        double c010 = L(i, j + 1, k, c), c011 = L(i, j + 1, k + 1, c);
                        o[c] = c000 + fg * (c010 - c000) + fb * (c011 - c010) + fr * (c111 - c011);
                    } else {
                        double c010 = L(i, j + 1, k, c), c110 = L(i + 1, j + 1, k, c);
                        o[c] = c000 + fg * (c010 - c000) + fr * (c110 - c010) + fb * (c111 - c110);
                    }
                }
            }
        }
    }, 1024);
    return out;
}

Image compute_with_lut_1d(const Image& data, const PixelFn& fn, int n_out, double xmin, double xmax,
                          int steps) {
    if (!(xmax > xmin)) throw std::runtime_error("LUT: xmax must be greater than xmin");
    Vec nodes = linspace(xmin, xmax, steps);
    std::vector<Vec> values(n_out, Vec(steps));
    Vec tmp(n_out);
    for (int i = 0; i < steps; ++i) {
        fn(&nodes[i], tmp.data());
        for (int c = 0; c < n_out; ++c) values[c][i] = tmp[c];
    }
    Image out(data.h, data.w, n_out);
    parallel_for(static_cast<long>(data.pixels()), [&](long a, long b) {
        for (long p = a; p < b; ++p) {
            double d = data.v[static_cast<size_t>(p) * data.c];
            for (int c = 0; c < n_out; ++c) {
                int g = 0;
                out.v[static_cast<size_t>(p) * n_out + c] = np_interp(d, nodes.data(), values[c].data(), steps, g);
            }
        }
    }, 1024);
    return out;
}

}  // namespace spectra
