// SPDX-License-Identifier: GPL-3.0-or-later
#include <cmath>
#include <string>
#include <vector>
#include <stdexcept>

#include "spectra/core/parallel.hpp"
#include "spectra/model/emulsion.hpp"
#include "spectra/model/filters.hpp"

namespace spectra {

namespace {
inline double langmuir(double d, double k, double ref) {
    if (!std::isfinite(k)) return d;
    return d * (k + ref) / (k + d);
}
}  // namespace

Vec dir_couplers_matrix(const DirCouplersParams& p, int n_ch) {
    Vec m(static_cast<size_t>(n_ch) * n_ch, 0.0);
    for (int i = 0; i < n_ch; ++i) m[i * n_ch + i] = p.gamma_samelayer_rgb[i] * p.inhibition_samelayer;
    if (n_ch == 3) {
        double inter[9] = {0, p.gamma_interlayer_r_to_gb[0], p.gamma_interlayer_r_to_gb[1],
                           p.gamma_interlayer_g_to_rb[0], 0, p.gamma_interlayer_g_to_rb[1],
                           p.gamma_interlayer_b_to_rg[0], p.gamma_interlayer_b_to_rg[1], 0};
        for (int i = 0; i < 9; ++i) m[i] = m[i] + inter[i] * p.inhibition_interlayer;
    }
    return m;
}

DirCouplersTables build_dir_couplers_tables(const DirCouplersParams& p, const Vec& le,
                                            const Vec& curves, int n_ch,
                                            const std::string& profile_type) {
    DirCouplersTables t;
    t.n_ch = n_ch;
    t.positive = profile_type == "positive";
    int n_le = static_cast<int>(le.size());
    Vec unit = dir_couplers_matrix(p, n_ch);
    Vec d_max(n_ch, -INFINITY);
    for (int i = 0; i < n_le; ++i)
        for (int k = 0; k < n_ch; ++k) {
            double v = curves[static_cast<size_t>(i) * n_ch + k];
            if (!std::isnan(v) && v > d_max[k]) d_max[k] = v;
        }
    t.density_max = d_max;
    Vec k_donor(n_ch);
    t.d_ref.resize(n_ch);
    for (int k = 0; k < n_ch; ++k) {
        t.d_ref[k] = 0.5 * d_max[k];
        k_donor[k] = p.langmuir_donor_k_rgb[k] * d_max[k];
    }
    if (t.positive) {
        t.has_receiver = true;
        t.receiver_c_ref.assign(n_ch, 0.0);
        t.receiver_kr.resize(n_ch);
        for (int m = 0; m < n_ch; ++m) {
            double c = 0;
            for (int k = 0; k < n_ch; ++k) c += t.d_ref[k] * unit[k * n_ch + m];
            t.receiver_c_ref[m] = c;
            t.receiver_kr[m] = p.langmuir_receiver_k_rgb[m] * (2.0 * c);
        }
    } else {
        t.has_donor_k = true;
        t.donor_k = k_donor;
    }
    t.matrix = unit;
    for (auto& v : t.matrix) v *= p.amount;

    // compute_density_curves_before_dir_couplers
    t.curves_before.assign(curves.size(), 0.0);
    Vec le0(n_le), col(n_le);
    std::vector<Vec> amount(n_ch, Vec(n_le));
    for (int i = 0; i < n_le; ++i) {
        Vec donor(n_ch);
        for (int k = 0; k < n_ch; ++k) {
            double d = curves[static_cast<size_t>(i) * n_ch + k];
            double silver = t.positive ? d_max[k] - d : d;
            donor[k] = t.has_donor_k ? langmuir(silver, t.donor_k[k], t.d_ref[k]) : silver;
        }
        for (int m = 0; m < n_ch; ++m) {
            double s = 0;
            for (int k = 0; k < n_ch; ++k) s += donor[k] * t.matrix[k * n_ch + m];
            if (t.has_receiver) s = langmuir(s, t.receiver_kr[m], t.receiver_c_ref[m]);
            amount[m][i] = s;
        }
    }
    for (int m = 0; m < n_ch; ++m) {
        for (int i = 0; i < n_le; ++i) {
            le0[i] = le[i] - amount[m][i];
            double d = curves[static_cast<size_t>(i) * n_ch + m];
            col[i] = t.positive ? -d : d;
        }
        int guess = 0;
        for (int i = 0; i < n_le; ++i) {
            double v = np_interp(le[i], le0.data(), col.data(), n_le, guess);
            t.curves_before[static_cast<size_t>(i) * n_ch + m] = t.positive ? -v : v;
        }
    }
    return t;
}

Image apply_dir_couplers(const Image& density_cmy, const Image& log_raw, double pixel_size_um,
                         const Vec& le, const Vec& curves, const DirCouplersParams& p,
                         const std::string& profile_type) {
    if (!p.active) return density_cmy;
    int n_ch = density_cmy.c;
    DirCouplersTables t = build_dir_couplers_tables(p, le, curves, n_ch, profile_type);
    double size_px = 0, tail_px = 0;
    if (p.diffusion_size_um > 0) {
        size_px = p.diffusion_size_um / pixel_size_um;
        tail_px = p.diffusion_tail_um / pixel_size_um;
    }
    Image corr(density_cmy.h, density_cmy.w, n_ch);
    parallel_for(static_cast<long>(density_cmy.pixels()), [&](long b, long e) {
        double donor[3];
        for (long i = b; i < e; ++i) {
            const double* d = &density_cmy.v[static_cast<size_t>(i) * n_ch];
            for (int k = 0; k < n_ch; ++k) {
                double silver = t.positive ? t.density_max[k] - d[k] : d[k];
                donor[k] = t.has_donor_k ? langmuir(silver, t.donor_k[k], t.d_ref[k]) : silver;
            }
            for (int m = 0; m < n_ch; ++m) {
                double s = 0;
                for (int k = 0; k < n_ch; ++k) s += donor[k] * t.matrix[k * n_ch + m];
                corr.v[static_cast<size_t>(i) * n_ch + m] = s;
            }
        }
    }, 4096);
    if (size_px > 0) {
        Image g = gaussian_filter(corr, size_px);
        Image ex = exponential_filter(corr, Vec(n_ch, tail_px));
        double w = p.diffusion_tail_weight;
        for (size_t i = 0; i < corr.v.size(); ++i) corr.v[i] = (1 - w) * g.v[i] + ex.v[i] * w;
    }
    Image log_raw0(density_cmy.h, density_cmy.w, n_ch);
    for (size_t i = 0; i < corr.pixels(); ++i)
        for (int m = 0; m < n_ch; ++m) {
            size_t j = i * n_ch + m;
            double c = corr.v[j];
            if (t.has_receiver) c = langmuir(c, t.receiver_kr[m], t.receiver_c_ref[m]);
            log_raw0.v[j] = log_raw.v[j] - c;
        }
    return interpolate_exposure_to_density(log_raw0, t.curves_before, le);
}

}  // namespace spectra
