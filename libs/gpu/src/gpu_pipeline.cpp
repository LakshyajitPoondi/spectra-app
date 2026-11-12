// SPDX-License-Identifier: GPL-3.0-or-later
#include "spectra/gpu/gpu_pipeline.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

#include "spectra/core/parallel.hpp"
#include "spectra/model/diffusion.hpp"
#include "spectra/model/emulsion.hpp"
#include "spectra/model/extended.hpp"
#include "spectra/model/filters.hpp"
#include "spectra/model/metering.hpp"

namespace spectra::gpu {

namespace {

constexpr int NL = kNumWavelengths;
// Gaussian filter selection, following the reference (fast_gaussian_filter):
//   sigma < 3          separable FIR truncated at 3 sigma, 'reflect' boundaries (exact port)
//   3 <= sigma <= 24   Young - van Vliet recursive filter, replicated boundaries (exact port, fp32)
//   sigma > 24         the same recursive filter on a 2^k-times reduced copy of the plane
// The last case is an approximation: the fp32 recursion loses accuracy for very
// wide kernels (its feed-forward gain is about 1.6 / (0.42 sigma^3)), so wide
// Gaussians are evaluated at reduced resolution instead.
constexpr double kIirMinSigma = 3.0, kPyramidSigma = 24.0;
// Stream identifiers, identical to the CPU reference (libs/model/src/grain.cpp).
constexpr uint32_t kStreamGrain = 0x100, kStreamClump = 0x200, kStreamGlare = 0x300;

double ms_since(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}

std::vector<float> to_float(const Vec& v) { return std::vector<float>(v.begin(), v.end()); }

void cctf_id(const std::string& cctf, uint32_t& id, float& gamma) {
    gamma = 1.0f;
    if (cctf == "linear") id = 0;
    else if (cctf == "srgb") id = 1;
    else if (cctf == "romm") id = 2;
    else if (cctf == "bt2020") id = 3;
    else if (cctf.rfind("gamma:", 0) == 0) {
        id = 4;
        gamma = std::stof(cctf.substr(6));
    } else {
        throw std::runtime_error("unknown CCTF '" + cctf + "'");
    }
}

// FIR weights of a normalised Gaussian. The reference uses a 3-sigma FIR below
// sigma = 3 and a recursive (effectively untruncated) filter above; 4 sigma is
// used there.
Vec gaussian_weights(double sigma, int& radius) {
    if (!(sigma > 0)) {
        radius = 0;
        return {1.0};
    }
    double truncate = sigma < 3.0 ? 3.0 : 4.0;
    radius = static_cast<int>(truncate * sigma + 0.5);
    Vec k(2 * radius + 1);
    double total = 0;
    for (int i = 0; i <= 2 * radius; ++i) {
        double r = (i - radius) / sigma;
        k[i] = std::exp(-0.5 * r * r);
        total += k[i];
    }
    for (auto& v : k) v /= total;
    return k;
}

}  // namespace

GpuPipeline::GpuPipeline(std::shared_ptr<Context> ctx, std::shared_ptr<const ColourData> colour,
                         const Params& digested, const RenderOptions& options, const GpuOptions& gopt)
    : ctx_(std::move(ctx)), cd_(std::move(colour)), p_(digested), opt_(options), gopt_(gopt) {
    cpu_ = std::make_unique<Pipeline>(cd_, p_, opt_);
    corr_ = cpu_->static_corrections();
}

GpuPipeline::~GpuPipeline() {
    for (auto& b : temp_) ctx_->destroy(b);
    for (auto& kv : tables_) ctx_->destroy(kv.second);
}

GpuPipeline::Plane GpuPipeline::make(int w, int h, int c) {
    Plane p;
    p.w = w;
    p.h = h;
    p.c = c;
    p.b = ctx_->create(p.floats());
    return p;
}

void GpuPipeline::free(Plane& p) {
    ctx_->destroy(p.b);
    p = Plane{};
}

GpuPipeline::Plane GpuPipeline::upload(const Image& img) {
    Plane p = make(img.w, img.h, img.c);
    std::vector<float> f(img.v.size());
    parallel_for(static_cast<long>(f.size()), [&](long a, long b) {
        for (long i = a; i < b; ++i) f[static_cast<size_t>(i)] = static_cast<float>(img.v[static_cast<size_t>(i)]);
    }, 1 << 16);
    ctx_->upload(p.b, f.data(), f.size());
    return p;
}

Image GpuPipeline::download(const Plane& p) {
    std::vector<float> f(p.floats());
    ctx_->download(p.b, f.data(), f.size());
    Image img(p.h, p.w, p.c);
    parallel_for(static_cast<long>(f.size()), [&](long a, long b) {
        for (long i = a; i < b; ++i) img.v[static_cast<size_t>(i)] = f[static_cast<size_t>(i)];
    }, 1 << 16);
    return img;
}

const Buffer* GpuPipeline::params(const std::vector<float>& v) {
    Buffer b = ctx_->create(std::max<size_t>(v.size(), 4));
    if (!v.empty()) ctx_->upload(b, v.data(), v.size());
    temp_.push_back(b);
    return &temp_.back();
}

const Buffer* GpuPipeline::uparams(const std::vector<uint32_t>& v) {
    Buffer b = ctx_->create(std::max<size_t>(v.size(), 4));
    ctx_->upload(b, reinterpret_cast<const float*>(v.data()), v.size());
    temp_.push_back(b);
    return &temp_.back();
}

const Buffer* GpuPipeline::table(const std::string& name, const std::vector<float>& v) {
    auto it = tables_.find(name);
    if (it == tables_.end()) {
        Buffer b = ctx_->create(std::max<size_t>(v.size(), 4));
        ctx_->upload(b, v.data(), v.size());
        it = tables_.emplace(name, b).first;
    }
    return &it->second;
}

void GpuPipeline::run(const std::string& kernel, const std::vector<const Buffer*>& b, const PushConstants& pc,
                      int w, int h) {
    // temp_ may reallocate while parameter buffers are created, so callers pass
    // pointers obtained after all params()/uparams() calls of this dispatch.
    ctx_->dispatch(kernel, b, pc, static_cast<uint32_t>((w + 15) / 16), static_cast<uint32_t>((h + 15) / 16));
}

void GpuPipeline::pointwise(int mode, const Plane& x, const Plane* y, Plane& out, const std::vector<float>& f,
                            const std::vector<float>& p) {
    PushConstants pc;
    pc.u[0] = static_cast<uint32_t>(x.w);
    pc.u[1] = static_cast<uint32_t>(x.h);
    pc.u[2] = static_cast<uint32_t>(x.c);
    pc.u[3] = static_cast<uint32_t>(mode);
    for (size_t i = 0; i < f.size() && i < 8; ++i) pc.f[i] = f[i];
    const Buffer* pb = p.empty() ? nullptr : params(p);
    run("pointwise", {&x.b, y ? &y->b : nullptr, &out.b, nullptr, nullptr, nullptr, nullptr, pb}, pc, x.w, x.h);
}

void GpuPipeline::combine(const Plane& x, const Plane* y, Plane& out, const Vec& a, const Vec& b, const Vec& add,
                          const Vec& div) {
    std::vector<float> p(16, 0.0f);
    for (int k = 0; k < x.c; ++k) {
        p[k] = static_cast<float>(a.empty() ? 1.0 : a[a.size() == 1 ? 0 : k]);
        p[4 + k] = static_cast<float>(b.empty() ? 0.0 : b[b.size() == 1 ? 0 : k]);
        p[8 + k] = static_cast<float>(add.empty() ? 0.0 : add[add.size() == 1 ? 0 : k]);
        p[12 + k] = static_cast<float>(div.empty() ? 1.0 : div[div.size() == 1 ? 0 : k]);
    }
    pointwise(0, x, y, out, {}, p);
}

void GpuPipeline::gaussian(const Plane& src, Plane& dst, const Vec& sigma_in, const Vec& coef_in, bool accumulate) {
    int c = src.c;
    Vec sigma(c), coef(c);
    for (int k = 0; k < c; ++k) {
        sigma[k] = sigma_in[sigma_in.size() == 1 ? 0 : k];
        coef[k] = coef_in.empty() ? 1.0 : coef_in[coef_in.size() == 1 ? 0 : k];
    }
    double max_sigma = *std::max_element(sigma.begin(), sigma.end());
    bool into_channel = dst.c != src.c;  // single-channel source into one channel of dst
    if (max_sigma > kPyramidSigma && !into_channel) {
        // Wide Gaussian: reduce by s = 2^k with 2x2 box filters, blur there, enlarge
        // bilinearly. The box chain adds a variance of (s^2 - 1) / 12 and the
        // bilinear enlargement about s^2 / 6 (full-resolution pixels^2), which the
        // low-resolution sigma accounts for.
        int levels = 0;
        while (max_sigma / (1 << levels) > kPyramidSigma) ++levels;
        double s = static_cast<double>(1 << levels);
        std::vector<Plane> pyr;
        const Plane* cur = &src;
        for (int l = 0; l < levels; ++l) {
            Plane lo = make((cur->w + 1) / 2, (cur->h + 1) / 2, c);
            PushConstants pc;
            pc.u[0] = static_cast<uint32_t>(lo.w);
            pc.u[1] = static_cast<uint32_t>(lo.h);
            pc.u[2] = static_cast<uint32_t>(c);
            pc.u[3] = static_cast<uint32_t>(cur->w);
            pc.u[4] = static_cast<uint32_t>(cur->h);
            pc.u[5] = 0;
            run("resample", {&cur->b, &lo.b}, pc, lo.w, lo.h);
            pyr.push_back(lo);
            cur = &pyr.back();
        }
        Vec low_sigma(c);
        for (int k = 0; k < c; ++k) {
            double var = sigma[k] * sigma[k] - (s * s - 1) / 12.0 - s * s / 6.0;
            low_sigma[k] = std::sqrt(std::max(var, 0.0)) / s;
        }
        Plane low_out = make(pyr.back().w, pyr.back().h, c);
        gaussian(pyr.back(), low_out, low_sigma, coef, false);
        Plane up = accumulate ? make(dst.w, dst.h, c) : Plane{};
        Plane& target = accumulate ? up : dst;
        PushConstants pc;
        pc.u[0] = static_cast<uint32_t>(target.w);
        pc.u[1] = static_cast<uint32_t>(target.h);
        pc.u[2] = static_cast<uint32_t>(c);
        pc.u[3] = static_cast<uint32_t>(low_out.w);
        pc.u[4] = static_cast<uint32_t>(low_out.h);
        pc.u[5] = 1;
        pc.f[0] = static_cast<float>(s);
        run("resample", {&low_out.b, &target.b}, pc, target.w, target.h);
        if (accumulate) {
            combine(dst, &up, dst, {1.0}, {1.0}, {}, {});
            free(up);
        }
        free(low_out);
        for (auto& p : pyr) free(p);
        return;
    }
    if (max_sigma >= kIirMinSigma && !into_channel) {
        // Recursive filter; channels below the threshold fall back to the identity
        // coefficients only when their sigma is zero (callers pass similar sigmas).
        std::vector<float> pv(20, 0.0f);
        for (int k = 0; k < c; ++k) {
            double sg = std::max(sigma[k], 0.5);
            double q = sg >= 2.5 ? 0.98711 * sg - 0.96330 : 3.97156 - 4.14554 * std::sqrt(1.0 - 0.26891 * sg);
            double q2 = q * q, q3 = q2 * q;
            double b0 = 1.57825 + 2.44413 * q + 1.4281 * q2 + 0.422205 * q3;
            double b1 = 2.44413 * q + 2.85619 * q2 + 1.26661 * q3;
            double b2 = -(1.4281 * q2 + 1.26661 * q3);
            double b3 = 0.422205 * q3;
            pv[k * 4] = static_cast<float>(1.0 - (b1 + b2 + b3) / b0);
            pv[k * 4 + 1] = static_cast<float>(b1 / b0);
            pv[k * 4 + 2] = static_cast<float>(b2 / b0);
            pv[k * 4 + 3] = static_cast<float>(b3 / b0);
            pv[16 + k] = 1.0f;
        }
        Plane tmp = make(src.w, src.h, c);
        PushConstants pc;
        pc.u[0] = static_cast<uint32_t>(src.w);
        pc.u[1] = static_cast<uint32_t>(src.h);
        pc.u[2] = static_cast<uint32_t>(c);
        pc.u[4] = 0;  // rows first, as the reference
        const Buffer* p1 = params(pv);
        ctx_->dispatch("iir", {&src.b, &tmp.b, nullptr, nullptr, nullptr, nullptr, nullptr, p1}, pc,
                       static_cast<uint32_t>((src.h + 15) / 16), 1);
        for (int k = 0; k < c; ++k) pv[16 + k] = static_cast<float>(coef[k]);
        pc.u[4] = 1;
        const Buffer* p2 = params(pv);
        if (accumulate) {
            Plane res = make(src.w, src.h, c);
            ctx_->dispatch("iir", {&tmp.b, &res.b, nullptr, nullptr, nullptr, nullptr, nullptr, p2}, pc,
                           static_cast<uint32_t>((src.w + 15) / 16), 1);
            combine(dst, &res, dst, {1.0}, {1.0}, {}, {});
            free(res);
        } else {
            ctx_->dispatch("iir", {&tmp.b, &dst.b, nullptr, nullptr, nullptr, nullptr, nullptr, p2}, pc,
                           static_cast<uint32_t>((src.w + 15) / 16), 1);
        }
        free(tmp);
        return;
    }
    int R = 0;
    std::vector<Vec> wts(c);
    std::vector<int> radius(c);
    for (int k = 0; k < c; ++k) {
        wts[k] = gaussian_weights(sigma[k], radius[k]);
        R = std::max(R, radius[k]);
    }
    int taps = 2 * R + 1;
    std::vector<float> wx(static_cast<size_t>(c) * taps, 0.0f), wy(wx.size(), 0.0f);
    for (int k = 0; k < c; ++k)
        for (int j = -radius[k]; j <= radius[k]; ++j) {
            wx[static_cast<size_t>(k) * taps + (j + R)] = static_cast<float>(wts[k][j + radius[k]]);
            wy[static_cast<size_t>(k) * taps + (j + R)] = static_cast<float>(wts[k][j + radius[k]] * coef[k]);
        }
    Plane tmp = make(src.w, src.h, c);
    PushConstants pc;
    pc.u[0] = static_cast<uint32_t>(src.w);
    pc.u[1] = static_cast<uint32_t>(src.h);
    pc.u[2] = static_cast<uint32_t>(c);
    pc.u[3] = static_cast<uint32_t>(R);
    pc.u[4] = 1;  // vertical first, as the reference's fused FIR
    pc.u[5] = 0;
    pc.u[6] = static_cast<uint32_t>(c);
    pc.u[7] = 0;
    const Buffer* bx = params(wx);
    run("blur", {&src.b, &tmp.b, bx}, pc, src.w, src.h);
    pc.u[4] = 0;
    pc.u[5] = accumulate ? 1 : 0;
    pc.u[6] = static_cast<uint32_t>(dst.c);
    pc.u[7] = 0;
    const Buffer* by = params(wy);
    run("blur", {&tmp.b, &dst.b, by}, pc, src.w, src.h);
    free(tmp);
}

void GpuPipeline::exponential(const Plane& src, Plane& dst, const Vec& decay, const Vec& coef, bool accumulate) {
    static const double fit[3][2] = {{0.1633, 0.5360}, {0.6496, 1.5236}, {0.1870, 2.7684}};
    for (int g = 0; g < 3; ++g) {
        Vec s(src.c), cf(src.c);
        for (int k = 0; k < src.c; ++k) {
            s[k] = fit[g][1] * decay[decay.size() == 1 ? 0 : k];
            cf[k] = fit[g][0] * (coef.empty() ? 1.0 : coef[coef.size() == 1 ? 0 : k]);
        }
        gaussian(src, dst, s, cf, accumulate || g > 0);
    }
}

Vec GpuPipeline::reduce(const Plane& p, bool sum) {
    Plane rows = make(1, p.h, p.c);
    PushConstants pc;
    pc.u[0] = static_cast<uint32_t>(p.w);
    pc.u[1] = static_cast<uint32_t>(p.h);
    pc.u[2] = static_cast<uint32_t>(p.c);
    pc.u[3] = sum ? 1 : 0;
    ctx_->dispatch("reduce", {&p.b, &rows.b}, pc, 1, static_cast<uint32_t>((p.h + 15) / 16));
    std::vector<float> f(rows.floats());
    ctx_->download(rows.b, f.data(), f.size());
    free(rows);
    Vec out(p.c, sum ? 0.0 : -INFINITY);
    for (int y = 0; y < p.h; ++y)
        for (int k = 0; k < p.c; ++k) {
            double v = f[static_cast<size_t>(y) * p.c + k];
            if (sum) out[k] += v;
            else out[k] = std::max(out[k], v);
        }
    return out;
}

GpuPipeline::Plane GpuPipeline::cpu_stage(const Plane& p, const std::string& what,
                                          const std::function<Image(const Image&)>& fn) {
    if (fallbacks_ && std::find(fallbacks_->begin(), fallbacks_->end(), what) == fallbacks_->end())
        fallbacks_->push_back(what);
    return upload(fn(download(p)));
}

std::vector<float> GpuPipeline::test_sampler(Context& ctx, bool poisson, double a, double b, bool fast, int w, int h,
                                             uint64_t seed) {
    Buffer out = ctx.create(static_cast<size_t>(w) * h), sb = ctx.create(4);
    uint32_t sv[4] = {static_cast<uint32_t>(seed), static_cast<uint32_t>(seed >> 32), 0, 0};
    ctx.upload(sb, reinterpret_cast<const float*>(sv), 4);
    PushConstants pc;
    pc.u[0] = static_cast<uint32_t>(w);
    pc.u[1] = static_cast<uint32_t>(h);
    pc.u[2] = 1;
    pc.u[3] = 2;
    pc.u[4] = poisson ? 0 : 1;
    pc.u[6] = 7;
    pc.u[7] = fast ? 2 : 0;
    pc.f[0] = static_cast<float>(a);
    pc.f[1] = static_cast<float>(b);
    ctx.dispatch("grain", {nullptr, nullptr, &out, nullptr, nullptr, nullptr, &sb}, pc, static_cast<uint32_t>((w + 15) / 16),
                 static_cast<uint32_t>((h + 15) / 16));
    std::vector<float> v(static_cast<size_t>(w) * h);
    ctx.download(out, v.data(), v.size());
    ctx.destroy(out);
    ctx.destroy(sb);
    return v;
}

std::vector<float> GpuPipeline::test_grain_layer(Context& ctx, double density, double density_max, double n_particles,
                                                 double uniformity, bool fast, int size, uint64_t seed, uint32_t stream) {
    size_t n = static_cast<size_t>(size) * size;
    Buffer in = ctx.create(n), out = ctx.create(n), xa = ctx.create(4), sb = ctx.create(4);
    std::vector<float> flat(n, static_cast<float>(density));
    ctx.upload(in, flat.data(), n);
    float axis[2] = {0.0f, 100.0f};  // identity curve: layer density = density
    ctx.upload(xa, axis, 2);
    uint32_t sv[4] = {static_cast<uint32_t>(seed), static_cast<uint32_t>(seed >> 32), 0, 0};
    ctx.upload(sb, reinterpret_cast<const float*>(sv), 4);
    PushConstants pc;
    pc.u[0] = pc.u[1] = static_cast<uint32_t>(size);
    pc.u[2] = 1;
    pc.u[3] = 0;
    pc.u[4] = 0;
    pc.u[5] = 2;
    pc.u[6] = stream;
    pc.u[7] = fast ? 2 : 0;
    pc.f[0] = 0.0f;
    pc.f[1] = static_cast<float>(density_max);
    pc.f[2] = static_cast<float>(n_particles);
    pc.f[3] = static_cast<float>(uniformity);
    uint32_t g = static_cast<uint32_t>((size + 15) / 16);
    ctx.dispatch("grain", {&in, nullptr, &out, &xa, &xa, nullptr, &sb}, pc, g, g);
    ctx.download(out, flat.data(), n);
    for (Buffer* b : {&in, &out, &xa, &sb}) ctx.destroy(*b);
    return flat;
}

uint64_t GpuPipeline::estimate_bytes(int w, int h) const {
    // Peak working set: about 7 full planes of the widest channel count (the grain
    // and film-spatial stages), measured with Context::peak_allocated_bytes().
    int c = std::max(cpu_->film().n_ch, 3);
    return static_cast<uint64_t>(w) * h * (7ull * c + 2ull) * sizeof(float);
}

// ---- stages -------------------------------------------------------------------

GpuPipeline::Plane GpuPipeline::stage_expose(const Image& rgb, const Region& r, GpuRenderInfo* info, double ae_ev) {
    const Pipeline& cpu = *cpu_;
    int fc = cpu.film_.n_ch;
    double scale = std::pow(2.0, ae_ev) * std::pow(2.0, p_.camera.exposure_compensation_ev);
    Plane raw;
    if (p_.settings.rgb_to_raw_method == "hanatos2025") {
        Plane in = upload(rgb);
        raw = make(rgb.w, rgb.h, fc);
        const ColourSpace& cs = cd_->space(p_.io.input_color_space);
        Xy illu = cd_->illuminant_xy(cpu.film_.info.reference_illuminant);
        Mat3 M = cd_->rgb_to_xyz(cs, &illu, "CAT16");
        PushConstants pc;
        pc.u[0] = static_cast<uint32_t>(rgb.w);
        pc.u[1] = static_cast<uint32_t>(rgb.h);
        pc.u[2] = 3;
        if (p_.io.input_cctf_decoding) cctf_id(cs.cctf, pc.u[3], pc.f[0]);
        pc.u[4] = static_cast<uint32_t>(cpu.tc_lut_.size);
        pc.u[5] = static_cast<uint32_t>(fc);
        pc.f[1] = static_cast<float>(scale);
        const Buffer* lut = table("tc_lut", to_float(cpu.tc_lut_.v));
        const Buffer* pm = params(std::vector<float>(M.begin(), M.end()));
        run("expose", {&in.b, lut, &raw.b, nullptr, nullptr, nullptr, nullptr, pm}, pc, rgb.w, rgb.h);
        free(in);
    } else {
        if (fallbacks_) fallbacks_->push_back("rgb_to_raw (mallett2019)");
        Image cr = cpu.rgb_to_film_raw(rgb, p_.io.input_color_space, p_.io.input_cctf_decoding);
        for (auto& v : cr.v) v *= scale;
        raw = upload(cr);
    }
    const HalationParams& hp = p_.film_render.halation;
    if (hp.boost_ev != 0) {
        double max_raw;
        if (r.opt.boost_max_raw) {
            max_raw = *r.opt.boost_max_raw;
        } else {
            Vec m = reduce(raw, false);
            max_raw = *std::max_element(m.begin(), m.end());
        }
        if (info) info->stats.boost_max_raw = max_raw;
        double x0 = std::clamp(0.184 * std::pow(2.0, hp.protect_ev), 0.0, max_raw);
        if (max_raw == 0.0) {
            combine(raw, nullptr, raw, {0.0}, {}, {}, {});
        } else if (x0 != max_raw) {
            double a = std::pow(28.0, 1.0 - hp.boost_range);
            double xn = x0 / max_raw;
            double denom = std::exp(a * (1.0 - xn)) - a * (1.0 - xn) - 1.0;
            double k = (std::pow(2.0, hp.boost_ev) - 1.0) / denom;
            pointwise(1, raw, nullptr, raw,
                      {static_cast<float>(x0), static_cast<float>(1.0 / max_raw), static_cast<float>(a),
                       static_cast<float>(k * max_raw)});
        }
    }
    if (p_.camera.diffusion_filter.active && p_.camera.diffusion_filter.strength > 0) {
        Plane next = cpu_stage(raw, "camera diffusion filter", [&](const Image& img) {
            return apply_diffusion_filter_um(img, p_.camera.diffusion_filter, r.pixel_size_um);
        });
        free(raw);
        raw = next;
    }
    if (p_.camera.lens_blur_um > 0) {
        Plane next = make(raw.w, raw.h, raw.c);
        gaussian(raw, next, {p_.camera.lens_blur_um / r.pixel_size_um}, {}, false);
        free(raw);
        raw = next;
    }
    if (hp.active) {
        double s_amount = hp.scatter_amount, s_scale = hp.scatter_spatial_scale;
        Vec w_s = match_channels(hp.scatter_tail_weight, fc), sc = match_channels(hp.scatter_core_um, fc),
            lt = match_channels(hp.scatter_tail_um, fc);
        bool any = false;
        for (int k = 0; k < fc; ++k) {
            sc[k] = sc[k] * s_scale / r.pixel_size_um;
            lt[k] = lt[k] * s_scale / r.pixel_size_um;
            any = any || sc[k] > 0 || lt[k] > 0;
        }
        if (s_amount > 0 && any) {
            Vec c_core(fc), c_tail(fc);
            for (int k = 0; k < fc; ++k) {
                sc[k] = std::max(sc[k], 1e-6);
                lt[k] = std::max(lt[k], 1e-6);
                c_core[k] = s_amount * (1.0 - w_s[k]);
                c_tail[k] = s_amount * w_s[k];
            }
            Plane acc = make(raw.w, raw.h, fc);
            gaussian(raw, acc, sc, c_core, false);
            exponential(raw, acc, lt, c_tail, true);
            combine(raw, &acc, raw, {1.0 - s_amount}, {1.0}, {}, {});
            free(acc);
        }
        Vec a_tot = match_channels(hp.halation_strength, fc), sh = match_channels(hp.halation_first_sigma_um, fc);
        bool any_a = false, any_s = false;
        for (int k = 0; k < fc; ++k) {
            a_tot[k] *= hp.halation_amount;
            sh[k] = sh[k] * hp.halation_spatial_scale / r.pixel_size_um;
            any_a = any_a || a_tot[k] > 0;
            any_s = any_s || sh[k] > 0;
        }
        int N = hp.halation_n_bounces;
        if (N >= 1 && any_a && any_s) {
            Vec decay(N);
            double dsum = 0;
            for (int k = 1; k <= N; ++k) dsum += decay[k - 1] = std::pow(hp.halation_bounce_decay, k - 1);
            Plane acc = make(raw.w, raw.h, fc);
            for (int k = 1; k <= N; ++k) {
                Vec sk(fc), ck(fc);
                for (int ch = 0; ch < fc; ++ch) {
                    sk[ch] = std::max(sh[ch] * std::sqrt(static_cast<double>(k)), 1e-6);
                    ck[ch] = decay[k - 1] / dsum * a_tot[ch];
                }
                gaussian(raw, acc, sk, ck, k > 1);
            }
            Vec div(fc, 1.0);
            if (hp.halation_renormalize)
                for (int ch = 0; ch < fc; ++ch) div[ch] = 1.0 + a_tot[ch];
            combine(raw, &acc, raw, {1.0}, {1.0}, {}, div);
            free(acc);
        }
    }
    pointwise(2, raw, nullptr, raw, {static_cast<float>(corr_.filming)});
    return raw;
}

GpuPipeline::Plane GpuPipeline::stage_develop(Plane& log_raw, const Region& r) {
    const Pipeline& cpu = *cpu_;
    const Vec& le = cd_->log_exposure();
    int n_le = static_cast<int>(le.size()), c = log_raw.c;
    Curves norm = cpu.film_.curves;
    Vec mins = norm.column_min();
    for (int i = 0; i < norm.n_le; ++i)
        for (int k = 0; k < c; ++k) norm.total[static_cast<size_t>(i) * c + k] -= mins[k];
    const Buffer* curves = table("film_curves", to_float(norm.total));
    PushConstants base;
    base.u[0] = static_cast<uint32_t>(log_raw.w);
    base.u[1] = static_cast<uint32_t>(log_raw.h);
    base.u[2] = static_cast<uint32_t>(c);
    base.u[4] = static_cast<uint32_t>(n_le);
    base.f[0] = static_cast<float>(le.front());
    base.f[1] = static_cast<float>((n_le - 1) / (le.back() - le.front()));
    base.f[2] = 1.0f;
    base.f[3] = static_cast<float>(p_.extended.push_pull.film_stops);

    bool experimental = cpu.experimental_push();
    Plane warped, scaled;
    const Plane* lookup = &log_raw;
    if (experimental) {
        warped = make(log_raw.w, log_raw.h, c);
        PushConstants pc = base;
        pc.u[3] = 3;
        run("develop", {&log_raw.b, nullptr, &warped.b, curves}, pc, log_raw.w, log_raw.h);
        lookup = &warped;
    }
    if (cpu.film_axis_gamma_ != 1.0) {
        scaled = make(log_raw.w, log_raw.h, c);
        combine(*lookup, nullptr, scaled, {cpu.film_axis_gamma_}, {}, {}, {});
        lookup = &scaled;
    }
    Plane density = make(log_raw.w, log_raw.h, c);
    {
        PushConstants pc = base;
        pc.u[3] = 0;
        run("develop", {&lookup->b, nullptr, &density.b, curves}, pc, log_raw.w, log_raw.h);
    }
    const DirCouplersParams& dp = p_.film_render.dir_couplers;
    if (dp.active) {
        DirCouplersTables t = build_dir_couplers_tables(dp, le, norm.total, c, cpu.film_.info.type);
        std::vector<float> pv(24, 0.0f);
        for (int k = 0; k < c; ++k) {
            for (int m = 0; m < c; ++m) pv[static_cast<size_t>(k) * c + m] = static_cast<float>(t.matrix[static_cast<size_t>(k) * c + m]);
            pv[9 + k] = static_cast<float>(t.density_max[k]);
            pv[12 + k] = t.has_donor_k ? (std::isfinite(t.donor_k[k]) ? static_cast<float>(t.donor_k[k]) : 3e38f) : 0.0f;
            pv[15 + k] = static_cast<float>(t.d_ref[k]);
            pv[18 + k] = t.has_receiver ? (std::isfinite(t.receiver_kr[k]) ? static_cast<float>(t.receiver_kr[k]) : 3e38f) : 0.0f;
            pv[21 + k] = t.has_receiver ? static_cast<float>(t.receiver_c_ref[k]) : 0.0f;
        }
        const Buffer* before = table("film_curves_before_dir", to_float(t.curves_before));
        Plane corr = make(log_raw.w, log_raw.h, c);
        {
            PushConstants pc = base;
            pc.u[3] = 1;
            pc.u[5] = t.positive ? 1 : 0;
            pc.u[6] = t.has_donor_k ? 1 : 0;
            const Buffer* pb = params(pv);
            run("develop", {&density.b, nullptr, &corr.b, curves, nullptr, nullptr, nullptr, pb}, pc, log_raw.w, log_raw.h);
        }
        if (dp.diffusion_size_um > 0) {
            double size_px = dp.diffusion_size_um / r.pixel_size_um, tail_px = dp.diffusion_tail_um / r.pixel_size_um;
            Plane bl = make(log_raw.w, log_raw.h, c);
            gaussian(corr, bl, {size_px}, {1.0 - dp.diffusion_tail_weight}, false);
            exponential(corr, bl, {tail_px}, {dp.diffusion_tail_weight}, true);
            free(corr);
            corr = bl;
        }
        {
            PushConstants pc = base;
            pc.u[3] = 2;
            pc.u[7] = t.has_receiver ? 1 : 0;
            const Buffer* pb = params(pv);
            run("develop", {&corr.b, &lookup->b, &density.b, before, nullptr, nullptr, nullptr, pb}, pc, log_raw.w, log_raw.h);
        }
        free(corr);
    }
    if (experimental) {
        PushConstants pc = base;
        pc.u[3] = 4;
        run("develop", {&density.b, &warped.b, &density.b, curves}, pc, log_raw.w, log_raw.h);
    }
    if (warped.b.buffer) free(warped);
    if (scaled.b.buffer) free(scaled);
    return density;
}

GpuPipeline::Plane GpuPipeline::stage_grain(Plane& cmy, const Region& r) {
    const GrainParams& g = p_.film_render.grain;
    if (!g.active) return cmy;
    const Pipeline& cpu = *cpu_;
    int c = cmy.c, w = cmy.w, h = cmy.h;
    Curves norm = cpu.film_.curves;
    Vec mins = norm.column_min();
    for (int i = 0; i < norm.n_le; ++i)
        for (int k = 0; k < c; ++k) norm.total[static_cast<size_t>(i) * c + k] -= mins[k];
    GrainLayerParams lp = grain_layer_params(g, norm, r.pixel_size_um);
    bool positive = cpu.film_.info.is_positive();
    int n_le = norm.n_le;
    std::vector<uint32_t> seed = {static_cast<uint32_t>(r.opt.seed), static_cast<uint32_t>(r.opt.seed >> 32),
                                  r.opt.origin_x, r.opt.origin_y};
    Plane out = make(w, h, c), plane = make(w, h, 1), ptmp = make(w, h, 1);
    for (int ch = 0; ch < c; ++ch) {
        std::vector<float> xa(n_le);
        for (int i = 0; i < n_le; ++i) xa[i] = static_cast<float>(positive ? -norm.t(i, ch) : norm.t(i, ch));
        for (int sl = 0; sl < lp.n_sub; ++sl) {
            std::vector<float> ya(n_le);
            for (int i = 0; i < n_le; ++i) ya[i] = static_cast<float>(norm.has_layers() ? norm.l(i, sl, ch) : norm.t(i, ch));
            double dmax = lp.density_max_layers[sl * c + ch], npp = lp.n_particles[sl * c + ch];
            PushConstants pc;
            pc.u[0] = static_cast<uint32_t>(w);
            pc.u[1] = static_cast<uint32_t>(h);
            pc.u[2] = static_cast<uint32_t>(c);
            pc.u[3] = 0;
            pc.u[4] = static_cast<uint32_t>(ch);
            pc.u[5] = static_cast<uint32_t>(n_le);
            pc.u[6] = kStreamGrain + static_cast<uint32_t>(ch * 16 + sl);
            pc.u[7] = (positive ? 1u : 0u) | (p_.settings.use_fast_stats ? 2u : 0u);
            pc.f[0] = static_cast<float>(lp.density_min_layers[sl * c + ch]);
            pc.f[1] = static_cast<float>(dmax);
            pc.f[2] = static_cast<float>(npp);
            pc.f[3] = static_cast<float>(lp.uniformity[ch]);
            const Buffer* bx = params(xa);
            const Buffer* by = params(ya);
            const Buffer* bs = uparams(seed);
            run("grain", {&cmy.b, nullptr, &plane.b, bx, by, nullptr, bs}, pc, w, h);
            // Dye-cloud blur of this sub-layer, accumulated into channel `ch` of the output.
            double sigma = g.blur_dye_clouds_um > 0 ? g.blur_dye_clouds_um * std::sqrt(dmax / npp) : 0.0;
            int radius;
            Vec wts = gaussian_weights(sigma, radius);
            std::vector<float> wf(wts.begin(), wts.end());
            PushConstants bp;
            bp.u[0] = static_cast<uint32_t>(w);
            bp.u[1] = static_cast<uint32_t>(h);
            bp.u[2] = 1;
            bp.u[3] = static_cast<uint32_t>(radius);
            bp.u[4] = 1;
            bp.u[5] = 0;
            bp.u[6] = 1;
            const Buffer* bw = params(wf);
            run("blur", {&plane.b, &ptmp.b, bw}, bp, w, h);
            bp.u[4] = 0;
            bp.u[5] = sl > 0 ? 1 : 0;
            bp.u[6] = static_cast<uint32_t>(c);
            bp.u[7] = static_cast<uint32_t>(ch);
            // A one-channel destination has c == cd, so the shader maps it directly.
            run("blur", {&ptmp.b, &out.b, bw}, bp, w, h);
        }
    }
    free(plane);
    free(ptmp);
    free(cmy);
    // micro-structure clumping
    double ms_blur_px = g.micro_structure[0] / r.pixel_size_um, ms_sigma = g.micro_structure[1] * 0.001 / r.pixel_size_um;
    if (ms_sigma > 0.05) {
        double sigma2 = std::log1p(ms_sigma * ms_sigma);
        Plane clump = make(w, h, c);
        PushConstants pc;
        pc.u[0] = static_cast<uint32_t>(w);
        pc.u[1] = static_cast<uint32_t>(h);
        pc.u[2] = static_cast<uint32_t>(c);
        pc.u[3] = 1;
        pc.u[6] = kStreamClump;
        pc.f[0] = static_cast<float>(std::sqrt(sigma2));
        pc.f[1] = static_cast<float>(-0.5 * sigma2);
        const Buffer* bs = uparams(seed);
        run("grain", {nullptr, nullptr, &clump.b, nullptr, nullptr, nullptr, bs}, pc, w, h);
        if (ms_blur_px > 0.4) {
            Plane t = make(w, h, c);
            gaussian(clump, t, {ms_blur_px}, {}, false);
            free(clump);
            clump = t;
        }
        pointwise(6, out, &clump, out);
        free(clump);
    }
    double blur = g.blur * r.scale, usm_sigma = g.mult_usm_sigma * r.scale;
    if (blur > 0) {
        Plane t = make(w, h, c);
        gaussian(out, t, {blur}, {}, false);
        free(out);
        out = t;
    }
    Vec minus_dmin(c);
    for (int k = 0; k < c; ++k) minus_dmin[k] = -lp.density_min[k];
    if (g.mult_usm_amount > 0 && usm_sigma > 0) {
        pointwise(7, out, nullptr, out);  // D = max(x, 0)
        Plane bl = make(w, h, c), res = make(w, h, c);
        gaussian(out, bl, {usm_sigma}, {}, false);
        pointwise(4, out, &bl, res, {static_cast<float>(g.mult_usm_amount)});
        free(bl);
        // Mass-conserving renormalisation: per-channel sums over the image.
        Vec sd = reduce(out, true), sr = reduce(res, true);
        Vec factor(c, 1.0);
        for (int k = 0; k < c; ++k)
            if (sr[k] > 0) factor[k] = sd[k] / sr[k];
        combine(res, nullptr, res, factor, {}, minus_dmin, {});
        free(out);
        out = res;
    } else {
        combine(out, nullptr, out, {1.0}, {}, minus_dmin, {});
    }
    return out;
}

GpuPipeline::Plane GpuPipeline::stage_print(Plane& cmy, const Region& r) {
    const Pipeline& cpu = *cpu_;
    int fc = cpu.film_.n_ch, pcn = cpu.print_.n_ch;
    std::vector<float> D(static_cast<size_t>(NL) * 4, 0.0f), R(static_cast<size_t>(NL) * 3, 0.0f);
    for (int l = 0; l < NL; ++l) {
        bool ok = !std::isnan(cpu.film_.base_density[l]) && !std::isnan(cpu.print_illuminant_[l]);
        for (int k = 0; k < fc; ++k) ok = ok && !std::isnan(cpu.film_.info.channel_density[l * fc + k]);
        if (!ok) continue;  // undefined wavelengths transmit nothing
        for (int k = 0; k < fc; ++k) D[l * 4 + k] = static_cast<float>(cpu.film_.info.channel_density[l * fc + k]);
        D[l * 4 + 3] = static_cast<float>(cpu.film_.base_density[l]);
        for (int m = 0; m < pcn; ++m) R[l * 3 + m] = static_cast<float>(cpu.print_illuminant_[l] * cpu.print_.sensitivity[l * pcn + m]);
    }
    std::vector<float> pv(9, 0.0f);
    for (int m = 0; m < pcn; ++m) {
        pv[m] = static_cast<float>(cpu.retime_[m] * cpu.exposure_factor_);
        pv[3 + m] = static_cast<float>(cpu.preflash_raw_[m]);
    }
    for (int k = 0; k < fc; ++k) pv[6 + k] = static_cast<float>(cpu.film_dmax_norm_[k]);
    const BleachBypassParams& bb = p_.extended.bleach_bypass;
    PushConstants pc;
    pc.u[0] = static_cast<uint32_t>(cmy.w);
    pc.u[1] = static_cast<uint32_t>(cmy.h);
    pc.u[2] = static_cast<uint32_t>(fc);
    pc.u[3] = 0;
    pc.u[4] = static_cast<uint32_t>(pcn);
    pc.u[5] = bb.active && bb.film_amount > 0 ? 1 : 0;
    pc.u[6] = cpu.film_.info.is_positive() ? 1 : 0;
    pc.f[0] = static_cast<float>(p_.enlarger.print_exposure * corr_.printing);
    pc.f[1] = static_cast<float>(bb.film_amount);
    pc.f[2] = static_cast<float>(bb.leuco_cyan_coupling);
    Plane raw = make(cmy.w, cmy.h, pcn);
    const Buffer* bd = table("print_D", D);
    const Buffer* br = table("print_R", R);
    const Buffer* pb = params(pv);
    run("spectral", {&cmy.b, nullptr, &raw.b, bd, br, nullptr, nullptr, pb}, pc, cmy.w, cmy.h);
    free(cmy);
    if (p_.enlarger.diffusion_filter.active && p_.enlarger.diffusion_filter.strength > 0) {
        Plane next = cpu_stage(raw, "enlarger diffusion filter", [&](const Image& img) {
            return apply_diffusion_filter_um(img, p_.enlarger.diffusion_filter, r.pixel_size_um);
        });
        free(raw);
        raw = next;
    }
    pointwise(2, raw, nullptr, raw, {1.0f});
    return raw;
}

GpuPipeline::Plane GpuPipeline::stage_scan(Plane& density, const Region& r) {
    const Pipeline& cpu = *cpu_;
    int c = cpu.scan_n_ch_, w = density.w, h = density.h;
    bool scan_film = p_.io.scan_film;
    const Vec& cmf = cd_->cmfs();
    std::vector<float> D(static_cast<size_t>(NL) * 4, 0.0f), R(static_cast<size_t>(NL) * 3, 0.0f);
    for (int l = 0; l < NL; ++l) {
        bool ok = !std::isnan(cpu.scan_base_[l]) && !std::isnan(cpu.scan_illuminant_[l]);
        for (int k = 0; k < c; ++k) ok = ok && !std::isnan(cpu.scan_channel_density_[l * c + k]);
        if (!ok) continue;
        for (int k = 0; k < c; ++k) D[l * 4 + k] = static_cast<float>(cpu.scan_channel_density_[l * c + k]);
        D[l * 4 + 3] = static_cast<float>(cpu.scan_base_[l]);
        for (int m = 0; m < 3; ++m) R[l * 3 + m] = static_cast<float>(cpu.scan_illuminant_[l] * cmf[l * 3 + m] / cpu.scan_norm_);
    }
    const BleachBypassParams& bb = p_.extended.bleach_bypass;
    std::vector<float> pv(9, 0.0f);
    PushConstants pc;
    pc.u[0] = static_cast<uint32_t>(w);
    pc.u[1] = static_cast<uint32_t>(h);
    pc.u[2] = static_cast<uint32_t>(c);
    pc.u[3] = 1;
    pc.u[4] = 3;
    if (scan_film) {
        pc.u[5] = bb.active && bb.film_amount > 0 ? 1 : 0;
        pc.u[6] = cpu.film_.info.is_positive() ? 1 : 0;
        pc.f[1] = static_cast<float>(bb.film_amount);
        pc.f[2] = static_cast<float>(bb.leuco_cyan_coupling);
        for (int k = 0; k < c; ++k) pv[6 + k] = static_cast<float>(cpu.film_dmax_norm_[k]);
    } else {
        pc.u[5] = bb.active && bb.print_amount > 0 ? 2 : 0;
        pc.f[1] = static_cast<float>(bb.print_amount);
        pc.f[2] = 1.0f;
        for (int k = 0; k < c; ++k) pv[6 + k] = static_cast<float>(cpu.print_dmax_[k]);
    }
    pc.u[7] = corr_.xyz_active ? 1 : 0;
    pc.f[3] = static_cast<float>(corr_.m);
    pc.f[4] = static_cast<float>(corr_.q);
    Plane xyz = make(w, h, 3);
    {
        const Buffer* bd = table("scan_D", D);
        const Buffer* br = table("scan_R", R);
        const Buffer* pb = params(pv);
        run("spectral", {&density.b, nullptr, &xyz.b, bd, br, nullptr, nullptr, pb}, pc, w, h);
    }
    free(density);

    // glare
    const GlareParams& gl = p_.print_render.glare;
    Plane glare;
    bool has_glare = !scan_film && gl.active && gl.percent > 0;
    if (has_glare) {
        double m = gl.percent, s = gl.roughness * gl.percent;
        double sigma2 = std::log(1.0 + (s * s) / (m * m));
        Plane noise = make(w, h, 1);
        PushConstants gp;
        gp.u[0] = static_cast<uint32_t>(w);
        gp.u[1] = static_cast<uint32_t>(h);
        gp.u[2] = 1;
        gp.u[3] = 1;
        gp.u[6] = kStreamGlare;
        gp.f[0] = static_cast<float>(std::sqrt(sigma2));
        gp.f[1] = static_cast<float>(std::log(m) - sigma2 / 2.0);
        const Buffer* bs = uparams({static_cast<uint32_t>(r.opt.seed), static_cast<uint32_t>(r.opt.seed >> 32),
                                    r.opt.origin_x, r.opt.origin_y});
        run("grain", {nullptr, nullptr, &noise.b, nullptr, nullptr, nullptr, bs}, gp, w, h);
        glare = make(w, h, 1);
        gaussian(noise, glare, {gl.blur * r.scale}, {}, false);
        free(noise);
    }

    // colour, gamut compression, HDR tone map
    const ColourSpace& cs = cd_->space(p_.io.output_color_space);
    const OutputGamutCompressSpec& spec = p_.io.output_gamut_compress;
    const HdrOutputParams& hdr = p_.extended.hdr_output;
    uint32_t algo = 0;
    bool cpu_gamut = false;
    if (spec.algorithm == "aces_rgc") algo = 1;
    else if (spec.algorithm == "oklch") algo = 2;
    else if (spec.algorithm == "oklrab") algo = 3;
    else if (spec.algorithm != "off") cpu_gamut = true;  // jzazbz, cam16ucs: CPU for now
    std::vector<float> cp(78, 0.0f);
    auto put = [&](size_t off, const Mat3& m) {
        for (int i = 0; i < 9; ++i) cp[off + i] = static_cast<float>(m[i]);
    };
    put(0, cpu.xyz_to_rgb_);
    for (int k = 0; k < 3; ++k) cp[9 + k] = static_cast<float>(cpu.scan_illuminant_xyz_[k]);
    put(12, cd_->rgb_to_xyz(cs, &cs.white_xy, "CAT02"));
    put(21, cd_->xyz_to_rgb(cs, &cs.white_xy, "CAT02"));
    put(30, cd_->oklab_m1);
    put(39, cd_->oklab_m2);
    put(48, cd_->oklab_m1_inv);
    put(57, cd_->oklab_m2_inv);
    for (int k = 0; k < 3; ++k) cp[66 + k] = static_cast<float>(spec.knee[k]);
    if (spec.lightness_compression)
        for (int k = 0; k < 3; ++k) cp[69 + k] = static_cast<float>((*spec.lightness_compression)[k]);
    const CMaxTable& tab = cpu.compressor_->table();
    cp[74] = 1.0f;
    double hdr_ref = std::max(hdr.reference_white_nits, 1.0), hdr_peak = std::max(hdr.peak_nits, hdr_ref + 1.0);
    cp[75] = static_cast<float>(hdr_ref);
    cp[76] = static_cast<float>(hdr_peak);
    cp[77] = static_cast<float>(hdr_ref * std::exp2(hdr.exposure_ev));
    PushConstants cc;
    cc.u[0] = static_cast<uint32_t>(w);
    cc.u[1] = static_cast<uint32_t>(h);
    cc.u[2] = 3;
    cc.u[3] = has_glare ? 1 : 0;
    cc.u[4] = cpu_gamut ? 0 : algo;
    cc.u[5] = spec.lightness_compression ? 1 : 0;
    const Buffer* tb = nullptr;
    if (algo >= 2 && !cpu_gamut) {
        cp[72] = static_cast<float>(tab.l_grid.front());
        cp[73] = static_cast<float>(tab.l_grid.back());
        cc.u[6] = static_cast<uint32_t>(tab.n_l);
        cc.u[7] = static_cast<uint32_t>(tab.n_h);
        tb = table("cmax", to_float(tab.table));
    }
    bool hdr_gpu = hdr.active && !cpu_gamut;
    cc.f[0] = hdr_gpu ? 1.0f : 0.0f;
    cc.f[1] = hdr.tone_mapping == "clip" ? 1.0f : 0.0f;
    Plane rgb = make(w, h, 3);
    {
        const Buffer* pb = params(cp);
        run("color", {&xyz.b, has_glare ? &glare.b : nullptr, &rgb.b, tb, nullptr, nullptr, nullptr, pb}, cc, w, h);
    }
    free(xyz);
    if (has_glare) free(glare);
    if (cpu_gamut) {
        Plane next = cpu_stage(rgb, "output gamut compression (" + spec.algorithm + ")", [&](const Image& img) {
            Image o = img;
            for (size_t i = 0; i < o.pixels(); ++i) {
                Vec3 v{o.v[i * 3], o.v[i * 3 + 1], o.v[i * 3 + 2]};
                if (hdr.active) {
                    v = extended::hdr_tone_map(v, hdr);
                    v = cpu.compressor_->apply(Vec3{v[0] / hdr_peak, v[1] / hdr_peak, v[2] / hdr_peak});
                    for (auto& x : v) x = std::isnan(x) ? 0.0 : std::clamp(x * hdr_peak, 0.0, hdr_peak);
                } else {
                    v = cpu.compressor_->apply(v);
                }
                for (int k = 0; k < 3; ++k) o.v[i * 3 + k] = v[k];
            }
            return o;
        });
        free(rgb);
        rgb = next;
    }

    double s = r.scale;
    if (p_.scanner.lens_blur * s > 0) {
        Plane t = make(w, h, 3);
        gaussian(rgb, t, {p_.scanner.lens_blur * s}, {}, false);
        free(rgb);
        rgb = t;
    }
    double usm_sigma = p_.scanner.unsharp_mask[0] * s, usm_amount = p_.scanner.unsharp_mask[1];
    if (usm_sigma > 0 && usm_amount > 0) {
        Plane bl = make(w, h, 3);
        gaussian(rgb, bl, {usm_sigma}, {}, false);
        combine(rgb, &bl, rgb, {1.0 + usm_amount}, {-usm_amount}, {}, {});
        free(bl);
    }
    // output transfer function
    PushConstants ec;
    ec.u[0] = static_cast<uint32_t>(w);
    ec.u[1] = static_cast<uint32_t>(h);
    ec.u[2] = 3;
    cctf_id(cs.cctf, ec.u[3], ec.f[0]);
    ec.u[4] = hdr.active ? (hdr.transfer == "pq" ? 1 : 2) : 0;
    ec.u[5] = p_.io.output_cctf_encoding ? 1 : 0;
    ec.f[1] = static_cast<float>(hdr_peak);
    if (hdr.active || p_.io.output_cctf_encoding) {
        Mat3 M = cd_->rgb_to_rgb(cs, cs);
        const Buffer* pb = params(std::vector<float>(M.begin(), M.end()));
        run("encode", {&rgb.b, nullptr, &rgb.b, nullptr, nullptr, nullptr, nullptr, pb}, ec, w, h);
    }
    return rgb;
}

Image GpuPipeline::render_region(const Image& rgb, Tap collect, const Region& region, GpuRenderInfo* info) {
    auto timed = [&](const char* name, std::chrono::steady_clock::time_point t0) {
        if (info) info->stage_ms[name] += ms_since(t0);
    };
    auto finish = [&](Plane& p) {
        auto t0 = std::chrono::steady_clock::now();
        Image out = download(p);
        free(p);
        for (auto& b : temp_) ctx_->destroy(b);
        temp_.clear();
        timed("download", t0);
        return out;
    };
    temp_.reserve(4096);  // parameter buffers must not move during a render
    double ae = region.opt.autoexposure_ev ? *region.opt.autoexposure_ev : 0.0;
    auto t0 = std::chrono::steady_clock::now();
    Plane log_raw = stage_expose(rgb, region, info, ae);
    timed("film exposure", t0);
    if (collect == Tap::LogEFilm) return finish(log_raw);
    t0 = std::chrono::steady_clock::now();
    Plane cmy = stage_develop(log_raw, region);
    free(log_raw);
    timed("film development", t0);
    t0 = std::chrono::steady_clock::now();
    cmy = stage_grain(cmy, region);
    timed("grain", t0);
    if (collect == Tap::CmyFilm) return finish(cmy);
    Plane density;
    if (p_.io.scan_film) {
        if (collect != Tap::RgbOut) throw std::runtime_error("the scan-film route has no print taps");
        density = cmy;
    } else {
        t0 = std::chrono::steady_clock::now();
        Plane log_print = stage_print(cmy, region);
        timed("print exposure", t0);
        if (collect == Tap::LogEPrint) return finish(log_print);
        t0 = std::chrono::steady_clock::now();
        const Vec& le = cd_->log_exposure();
        int n_le = static_cast<int>(le.size());
        density = make(log_print.w, log_print.h, log_print.c);
        PushConstants pc;
        pc.u[0] = static_cast<uint32_t>(log_print.w);
        pc.u[1] = static_cast<uint32_t>(log_print.h);
        pc.u[2] = static_cast<uint32_t>(log_print.c);
        pc.u[3] = 0;
        pc.u[4] = static_cast<uint32_t>(n_le);
        pc.f[0] = static_cast<float>(le.front());
        pc.f[1] = static_cast<float>((n_le - 1) / (le.back() - le.front()));
        pc.f[2] = 1.0f;
        const Buffer* curves = table("print_curves", to_float(cpu_->print_curves_morphed_));
        run("develop", {&log_print.b, nullptr, &density.b, curves}, pc, log_print.w, log_print.h);
        free(log_print);
        timed("print development", t0);
        if (collect == Tap::CmyPrint) return finish(density);
    }
    t0 = std::chrono::steady_clock::now();
    Plane out = stage_scan(density, region);
    timed("scan", t0);
    return finish(out);
}

Image GpuPipeline::process(const Image& image, Tap collect, GpuRenderInfo* info) {
    auto t_all = std::chrono::steady_clock::now();
    GpuRenderInfo local;
    if (!info) info = &local;
    *info = GpuRenderInfo{};
    fallbacks_ = &info->cpu_fallbacks;
    if (collect == Tap::RgbIn) return image;
    if (image.c < 3) throw std::runtime_error("input image must have at least 3 channels");
    ctx_->reset_peak();

    // preprocess (CPU): first three channels, crop, rescale
    Image img;
    const Image* src = &image;
    if (image.c != 3) {
        img = Image(image.h, image.w, 3);
        for (size_t i = 0; i < image.pixels(); ++i)
            for (int k = 0; k < 3; ++k) img.v[i * 3 + k] = image.v[i * image.c + k];
        src = &img;
    }
    double long_edge = opt_.full_long_edge_px ? *opt_.full_long_edge_px : std::max(src->h, src->w);
    double pixel_size = p_.camera.film_format_mm * 1000 / long_edge;
    Region region;
    region.opt = opt_;
    if (p_.camera.auto_exposure && !region.opt.autoexposure_ev) {
        Image meter = cpu_->rgb_to_film_raw(metering_preview(*src), p_.io.input_color_space, p_.io.input_cctf_decoding);
        region.opt.autoexposure_ev = measure_raw_autoexposure_ev(meter, p_.camera.auto_exposure_method);
    }
    if (!p_.camera.auto_exposure) region.opt.autoexposure_ev = 0.0;
    info->stats.autoexposure_ev = region.opt.autoexposure_ev;
    if (collect == Tap::RgbPre || p_.io.crop || p_.io.upscale_factor != 1.0) {
        Image pre = *src;
        if (p_.io.crop) pre = crop_image(pre, p_.io.crop_center, p_.io.crop_size);
        if (p_.io.upscale_factor != 1.0) {
            pixel_size /= p_.io.upscale_factor;
            pre = rescale_cubic(pre, p_.io.upscale_factor);
        }
        img = std::move(pre);
        src = &img;
    }
    if (collect == Tap::RgbPre) return *src;
    region.pixel_size_um = pixel_size;
    region.scale = opt_.match_python ? 1.0 : opt_.reference_pixel_um / pixel_size;
    info->pixel_size_um = pixel_size;
    info->stats.pixel_size_um = pixel_size;

    uint64_t budget = gopt_.memory_budget_bytes;
    if (budget == 0) budget = std::min<uint64_t>(4ull << 30, ctx_->query_budget_bytes() * 6 / 10);
    info->budget_bytes = budget;
    info->estimated_bytes = estimate_bytes(src->w, src->h);
    bool tile = gopt_.tile_size > 0 || info->estimated_bytes > budget;
    Image out;
    if (!tile) {
        out = render_region(*src, collect, region, info);
        info->tiles = 1;
    } else {
        if (p_.camera.diffusion_filter.active || p_.enlarger.diffusion_filter.active)
            throw std::runtime_error("the diffusion filter cannot be rendered in tiles yet; "
                                     "raise the GPU memory budget or use the CPU backend");
        // Apron: the support of every spatial effect that is switched on.
        const HalationParams& hp = p_.film_render.halation;
        const DirCouplersParams& dp = p_.film_render.dir_couplers;
        double px = pixel_size, apron = 8;
        apron += 4 * p_.camera.lens_blur_um / px;
        if (hp.active) {
            double core = std::max({hp.scatter_core_um[0], hp.scatter_core_um[1], hp.scatter_core_um[2]});
            double tail = std::max({hp.scatter_tail_um[0], hp.scatter_tail_um[1], hp.scatter_tail_um[2]});
            double first = std::max({hp.halation_first_sigma_um[0], hp.halation_first_sigma_um[1], hp.halation_first_sigma_um[2]});
            apron += 4 * std::max(core, 2.7684 * tail) * hp.scatter_spatial_scale / px;
            apron += 4 * first * std::sqrt(static_cast<double>(std::max(hp.halation_n_bounces, 1))) * hp.halation_spatial_scale / px;
        }
        if (dp.active && dp.diffusion_size_um > 0) apron += 4 * std::max(dp.diffusion_size_um, 2.7684 * dp.diffusion_tail_um) / px;
        if (p_.film_render.grain.active)
            apron += 4 * (p_.film_render.grain.blur + p_.film_render.grain.mult_usm_sigma) * region.scale + 8;
        apron += 4 * (p_.print_render.glare.blur + p_.scanner.lens_blur + p_.scanner.unsharp_mask[0]) * region.scale;
        int A = static_cast<int>(std::ceil(apron));
        int T = gopt_.tile_size;
        if (T <= 0) {
            // Largest square core whose padded tile fits the budget.
            double bytes_per_px = static_cast<double>(estimate_bytes(1024, 1024)) / (1024.0 * 1024.0);
            double side = std::sqrt(static_cast<double>(budget) / bytes_per_px);
            T = std::min(static_cast<int>(side) - 2 * A, 8192);
            if (T < 64)
                throw std::runtime_error("the GPU memory budget is too small for the active effects (they need a " +
                                         std::to_string(A) + " px border around every tile); raise the budget");
        }
        info->tiled = true;
        info->tile_size = T;
        info->apron = A;
        auto crop = [&](int x0, int y0, int x1, int y1) {
            Image t(y1 - y0, x1 - x0, 3);
            for (int y = y0; y < y1; ++y)
                std::copy(src->px(y, x0), src->px(y, x0) + static_cast<size_t>(x1 - x0) * 3, t.px(y - y0, 0));
            return t;
        };
        region.opt.full_long_edge_px = long_edge;
        // Whole-image statistic: the highlight boost needs the global maximum.
        if (p_.film_render.halation.boost_ev != 0 && !region.opt.boost_max_raw) {
            Params saved = p_;
            p_.film_render.halation.active = false;
            p_.film_render.halation.boost_ev = 0;
            p_.camera.lens_blur_um = 0;
            double mx = 0;
            for (int y0 = 0; y0 < src->h; y0 += T)
                for (int x0 = 0; x0 < src->w; x0 += T) {
                    Image t = crop(x0, y0, std::min(src->w, x0 + T), std::min(src->h, y0 + T));
                    Image lr = render_region(t, Tap::LogEFilm, region, nullptr);
                    for (double v : lr.v) mx = std::max(mx, v);
                }
            p_ = saved;
            region.opt.boost_max_raw = std::pow(10.0, mx) - 1e-10;
        }
        for (int y0 = 0; y0 < src->h; y0 += T)
            for (int x0 = 0; x0 < src->w; x0 += T) {
                int x1 = std::min(src->w, x0 + T), y1 = std::min(src->h, y0 + T);
                // The padded tile starts on a multiple of 32 so that reduced-resolution
                // planes (wide Gaussians) sample the same pixel groups in every tile.
                int ax0 = std::max(0, x0 - A) / 32 * 32, ay0 = std::max(0, y0 - A) / 32 * 32,
                    ax1 = std::min(src->w, x1 + A), ay1 = std::min(src->h, y1 + A);
                Region tr = region;
                tr.opt.origin_x = opt_.origin_x + static_cast<uint32_t>(ax0);
                tr.opt.origin_y = opt_.origin_y + static_cast<uint32_t>(ay0);
                Image t = render_region(crop(ax0, ay0, ax1, ay1), collect, tr, info);
                if (out.v.empty()) out = Image(src->h, src->w, t.c);
                for (int y = y0; y < y1; ++y)
                    std::copy(t.px(y - ay0, x0 - ax0), t.px(y - ay0, x0 - ax0) + static_cast<size_t>(x1 - x0) * t.c,
                              out.px(y, x0));
                ++info->tiles;
            }
    }
    info->peak_gpu_bytes = ctx_->peak_allocated_bytes();
    info->total_ms = ms_since(t_all);
    fallbacks_ = nullptr;
    return out;
}

}  // namespace spectra::gpu
