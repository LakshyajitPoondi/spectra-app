// SPDX-License-Identifier: GPL-3.0-or-later
#include "parity.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>

#include <complex>

#include <pocketfft_hdronly.h>

#include "spectra/core/npy.hpp"
#include "spectra/core/rng.hpp"
#include "spectra/model/diffusion.hpp"
#include "spectra/model/emulsion.hpp"
#include "spectra/model/filters.hpp"
#include "spectra/model/metering.hpp"
#include "spectra/pipeline/pipeline.hpp"

namespace spectra {

using nlohmann::json;
namespace fs = std::filesystem;

namespace {

struct Metrics {
    double max_abs = 0, rmse = 0, p99 = 0, worst_ratio = 0;
    size_t n = 0;
    bool shape_ok = true;
    std::string note;
};

Metrics compare(const std::vector<size_t>& shape_a, const Vec& a, const std::vector<size_t>& shape_b,
                const Vec& b, double atol, double rtol) {
    Metrics m;
    size_t na = 1, nb = 1;
    for (size_t d : shape_a) na *= d;
    for (size_t d : shape_b) nb *= d;
    if (na != nb || a.size() != b.size()) {
        m.shape_ok = false;
        m.note = "shape mismatch";
        return m;
    }
    m.n = a.size();
    Vec diffs(a.size());
    double se = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        double d;
        if (std::isnan(a[i]) && std::isnan(b[i])) d = 0;
        else if (std::isnan(a[i]) || std::isnan(b[i])) d = INFINITY;
        else if (std::isinf(a[i]) && a[i] == b[i]) d = 0;
        else d = std::fabs(a[i] - b[i]);
        diffs[i] = d;
        se += d * d;
        m.max_abs = std::max(m.max_abs, d);
        double scale = atol + rtol * std::fabs(b[i]);
        m.worst_ratio = std::max(m.worst_ratio, d / scale);
    }
    m.rmse = std::sqrt(se / std::max<size_t>(1, a.size()));
    if (!diffs.empty()) {
        size_t k = static_cast<size_t>(0.99 * (diffs.size() - 1));
        std::nth_element(diffs.begin(), diffs.begin() + static_cast<long>(k), diffs.end());
        m.p99 = diffs[k];
    }
    return m;
}

void flatten_json(const json& j, const std::string& path, std::vector<std::pair<std::string, json>>& out) {
    if (j.is_object()) {
        for (auto it = j.begin(); it != j.end(); ++it) flatten_json(it.value(), path + "." + it.key(), out);
    } else if (j.is_array()) {
        for (size_t i = 0; i < j.size(); ++i) flatten_json(j[i], path + "[" + std::to_string(i) + "]", out);
    } else {
        out.emplace_back(path, j);
    }
}

// Statistics of a flat patch; mirrors grain_stats() in tools/parity/make_golden.py.
Vec patch_stats(const Image& img, double pixel_size_um, const std::string& stat) {
    Vec out;
    int h = img.h, w = img.w;
    size_t n = img.pixels();
    for (int ch = 0; ch < img.c; ++ch) {
        Vec p = extract_channel(img, ch);
        double mean = 0;
        for (double v : p) mean += v;
        mean /= static_cast<double>(n);
        if (stat == "mean") {
            out.push_back(mean);
        } else if (stat == "std") {
            double s2 = 0;
            for (double v : p) s2 += (v - mean) * (v - mean);
            out.push_back(std::sqrt(s2 / static_cast<double>(n)));
        } else if (stat == "rms48") {
            double r = 24.0 / pixel_size_um;
            int k = static_cast<int>(std::floor(r));
            std::vector<std::pair<int, int>> offs;
            for (int dy = -k; dy <= k; ++dy)
                for (int dx = -k; dx <= k; ++dx)
                    if (dx * dx + dy * dy <= r * r) offs.emplace_back(dy, dx);
            Vec ap;
            for (int y = k; y < h - k; ++y)
                for (int x = k; x < w - k; ++x) {
                    double s = 0;
                    for (auto& o : offs) s += p[static_cast<size_t>(y + o.first) * w + x + o.second];
                    ap.push_back(s / static_cast<double>(offs.size()));
                }
            double m = 0, s2 = 0;
            for (double v : ap) m += v;
            m /= static_cast<double>(ap.size());
            for (double v : ap) s2 += (v - m) * (v - m);
            out.push_back(std::sqrt(s2 / static_cast<double>(ap.size())) * 1000.0);
        } else if (stat == "spectrum") {
            std::vector<std::complex<double>> f(n);
            for (size_t i = 0; i < n; ++i) f[i] = p[i] - mean;
            pocketfft::shape_t shape{static_cast<size_t>(h), static_cast<size_t>(w)};
            pocketfft::stride_t st{static_cast<ptrdiff_t>(w * sizeof(std::complex<double>)),
                                   sizeof(std::complex<double>)};
            pocketfft::c2c(shape, st, st, {0, 1}, pocketfft::FORWARD, f.data(), f.data(), 1.0);
            double lo = std::min(h, w) / 32.0, hi = std::min(h, w) / 2.0;
            Vec edges = linspace(lo, hi, 9);
            double sum[8] = {0}, cnt[8] = {0};
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) {
                    double fy = y < (h + 1) / 2 ? y : y - h, fx = x < (w + 1) / 2 ? x : x - w;
                    double rad = std::sqrt(fy * fy + fx * fx);
                    for (int b = 0; b < 8; ++b)
                        if (rad >= edges[b] && rad < edges[b + 1]) {
                            sum[b] += std::norm(f[static_cast<size_t>(y) * w + x]);
                            cnt[b] += 1;
                        }
                }
            double total = 0;
            for (int b = 0; b < 8; ++b) total += sum[b] / cnt[b];
            for (int b = 0; b < 8; ++b) out.push_back(sum[b] / cnt[b] / total);
        } else {
            throw std::runtime_error("unknown statistic '" + stat + "'");
        }
    }
    return out;
}

struct Built {
    Params digested;
    RenderOptions options;
};

Built build_params(const ColourData& cd, const json& c) {
    Params p;
    p.film = c.value("film", p.film);
    p.print = c.value("print", p.print);
    if (c.contains("pre")) apply_params_patch(p, c.at("pre"));
    Params d = digest_params(cd.pack(), p);
    if (c.contains("post")) apply_params_patch(d, c.at("post"));
    RenderOptions o;
    o.match_python = true;
    if (c.contains("options")) apply_render_options_patch(o, c.at("options"));
    return {d, o};
}

// Computes the quantity of a "unit" case.
void run_unit(std::shared_ptr<const ColourData> cd, const json& c, const fs::path& root,
              std::vector<size_t>& shape, Vec& data) {
    std::string name = c.at("unit").get<std::string>();
    auto input_image = [&] { return npy_load_image((root / c.at("input").get<std::string>()).string()); };
    auto from_image = [&](const Image& img) {
        shape = {static_cast<size_t>(img.h), static_cast<size_t>(img.w), static_cast<size_t>(img.c)};
        data = img.v;
    };
    if (name == "illuminant") {
        data = cd->illuminant(c.at("name").get<std::string>());
        shape = {data.size()};
    } else if (name == "illuminant_xy") {
        Xy xy = cd->illuminant_xy(c.at("name").get<std::string>());
        data = {xy[0], xy[1]};
        shape = {2};
    } else if (name == "cctf") {
        NpyArray x = npy_load((root / c.at("input").get<std::string>()).string());
        std::string id = cd->space(c.at("space").get<std::string>()).cctf;
        bool enc = c.at("direction").get<std::string>() == "encode";
        data.resize(x.data.size());
        for (size_t i = 0; i < x.data.size(); ++i)
            data[i] = enc ? cctf_encode(id, x.data[i]) : cctf_decode(id, x.data[i]);
        shape = x.shape;
    } else if (name == "rgb_to_xyz_matrix" || name == "xyz_to_rgb_matrix") {
        const ColourSpace& cs = cd->space(c.at("space").get<std::string>());
        Xy xy = cd->illuminant_xy(c.at("illuminant").get<std::string>());
        Mat3 m = name == "rgb_to_xyz_matrix" ? cd->rgb_to_xyz(cs, &xy, c.at("cat").get<std::string>())
                                             : cd->xyz_to_rgb(cs, &xy, c.at("cat").get<std::string>());
        data.assign(m.begin(), m.end());
        shape = {3, 3};
    } else if (name == "gaussian_filter") {
        Image img = input_image();
        Vec sigma = c.at("sigma").get<Vec>();
        if (sigma.size() == 1) sigma.assign(img.c, sigma[0]);
        from_image(gaussian_filter(img, sigma));
    } else if (name == "exponential_filter") {
        Image img = input_image();
        Vec decay = c.at("decay").get<Vec>();
        if (decay.size() == 1) decay.assign(img.c, decay[0]);
        from_image(exponential_filter(img, decay));
    } else if (name == "metering_preview") {
        from_image(metering_preview(input_image()));
    } else if (name == "autoexposure_ev") {
        data = {measure_raw_autoexposure_ev(input_image(), c.at("method").get<std::string>())};
        shape = {1};
    } else if (name == "boost_highlights") {
        Image img = input_image();
        boost_highlights(img, c.at("boost_ev").get<double>(), c.at("boost_range").get<double>(),
                         c.at("protect_ev").get<double>());
        from_image(img);
    } else if (name == "diffusion_psf") {
        DiffusionFilterParams dp;
        json j = c.at("diffusion");
        Params tmp;
        apply_params_patch(tmp, json{{"camera", {{"diffusion_filter", j}}}});
        from_image(diffusion_filter_psf(c.at("size").get<int>(), tmp.camera.diffusion_filter,
                                        c.at("pixel_size_um").get<double>()));
    } else if (name == "diffusion_filter") {
        Params tmp;
        apply_params_patch(tmp, json{{"camera", {{"diffusion_filter", c.at("diffusion")}}}});
        from_image(apply_diffusion_filter_um(input_image(), tmp.camera.diffusion_filter,
                                             c.at("pixel_size_um").get<double>()));
    } else if (name == "halation") {
        Params tmp;
        if (c.contains("halation")) apply_params_patch(tmp, json{{"film_render", {{"halation", c.at("halation")}}}});
        Image img = input_image();
        apply_halation_um(img, tmp.film_render.halation, c.at("pixel_size_um").get<double>());
        from_image(img);
    } else if (name == "sampler_pmf") {
        int kmax = c.at("kmax").get<int>();
        long samples = c.at("samples").get<long>();
        bool fast = c.at("fast").get<bool>();
        bool poisson = c.at("dist").get<std::string>() == "poisson";
        double lam = c.value("lam", 0.0), p = c.value("p", 0.0);
        int nn = c.value("n", 0);
        std::vector<long> hist(static_cast<size_t>(kmax) + 1, 0);
        for (long i = 0; i < samples; ++i) {
            PixelRng rng(c.value("seed", 12345u), 7, static_cast<uint32_t>(i & 0xffff), static_cast<uint32_t>(i >> 16));
            int k = poisson ? sample_poisson(rng, lam, fast) : sample_binomial(rng, nn, p, fast);
            ++hist[static_cast<size_t>(std::clamp(k, 0, kmax))];
        }
        for (long v : hist) data.push_back(static_cast<double>(v) / static_cast<double>(samples));
        shape = {data.size()};
    } else if (name == "grain_layer_stats") {
        int size = c.at("size").get<int>();
        std::string stat = c.at("stat").get<std::string>();
        uint32_t stream = 0;
        for (double level : c.at("levels").get<Vec>()) {
            Image plane(size, size, 1, level);
            sample_grain_layer(plane.v.data(), plane.v.data(), size, size, c.at("density_max").get<double>(),
                               c.at("n_particles").get<double>(), c.at("uniformity").get<double>(),
                               c.value("seed", 99u), stream++, c.at("fast").get<bool>());
            data.push_back(patch_stats(plane, 6.0, stat)[0]);
        }
        shape = {data.size()};
    } else if (name == "cct_to_xy") {
        Xy xy = cct_to_xy(c.at("temperature").get<double>());
        data = {xy[0], xy[1]};
        shape = {2};
    } else if (name == "raw_white_balance") {
        Image img = input_image();
        Mat3 m = raw_white_balance_matrix(*cd, c.at("temperature").get<double>(), c.value("tint", 1.0));
        for (size_t i = 0; i < img.pixels(); ++i) {
            Vec3 o = mat3_apply(m, {img.v[i * 3], img.v[i * 3 + 1], img.v[i * 3 + 2]});
            for (int k = 0; k < 3; ++k) img.v[i * 3 + k] = o[k];
        }
        from_image(img);
    } else if (name == "crop") {
        Params tmp;
        apply_params_patch(tmp, json{{"io", c.at("io")}});
        from_image(crop_image(input_image(), tmp.io.crop_center, tmp.io.crop_size));
    } else {
        // Units that need a built pipeline.
        Built b = build_params(*cd, c);
        Pipeline pipe(cd, b.digested, b.options);
        if (name == "tc_lut") {
            const TcLut& lut = pipe.tc_lut();
            int step = c.value("subsample", 1);
            int n = (lut.size + step - 1) / step;
            shape = {static_cast<size_t>(n), static_cast<size_t>(n), static_cast<size_t>(lut.n_ch)};
            for (int i = 0; i < lut.size; i += step)
                for (int j = 0; j < lut.size; j += step)
                    for (int k = 0; k < lut.n_ch; ++k)
                        data.push_back(lut.v[(static_cast<size_t>(i) * lut.size + j) * lut.n_ch + k]);
        } else if (name == "film_curves") {
            const Curves& cv = pipe.film().curves;
            shape = {static_cast<size_t>(cv.n_le), static_cast<size_t>(cv.n_ch)};
            data = cv.total;
        } else if (name == "film_curves_layers") {
            const Curves& cv = pipe.film().curves;
            shape = {static_cast<size_t>(cv.n_le), static_cast<size_t>(cv.n_layers), static_cast<size_t>(cv.n_ch)};
            data = cv.layers;
        } else if (name == "print_curves_morphed") {
            data = pipe.print_curves_morphed();
            shape = {data.size() / pipe.print().n_ch, static_cast<size_t>(pipe.print().n_ch)};
        } else if (name == "density_spectral_midgray") {
            data = pipe.density_spectral_midgray();
            shape = {data.size()};
        } else if (name == "grain_particles") {
            Curves norm = pipe.film().curves;
            Vec mins = norm.column_min();
            for (int i = 0; i < norm.n_le; ++i)
                for (int k = 0; k < norm.n_ch; ++k) norm.total[static_cast<size_t>(i) * norm.n_ch + k] -= mins[k];
            GrainLayerParams lp = grain_layer_params(b.digested.film_render.grain, norm,
                                                     c.at("pixel_size_um").get<double>());
            data = lp.n_particles;
            shape = {static_cast<size_t>(lp.n_sub), static_cast<size_t>(lp.n_ch)};
        } else if (name == "dir_couplers_curves") {
            Curves norm = pipe.film().curves;
            Vec mins = norm.column_min();
            for (int i = 0; i < norm.n_le; ++i)
                for (int k = 0; k < norm.n_ch; ++k) norm.total[static_cast<size_t>(i) * norm.n_ch + k] -= mins[k];
            DirCouplersTables t = build_dir_couplers_tables(b.digested.film_render.dir_couplers, cd->log_exposure(),
                                                            norm.total, norm.n_ch, pipe.film().info.type);
            data = t.curves_before;
            shape = {static_cast<size_t>(norm.n_le), static_cast<size_t>(norm.n_ch)};
        } else {
            throw std::runtime_error("unknown unit '" + name + "'");
        }
    }
}

}  // namespace

int run_parity(std::shared_ptr<const ColourData> cd, const ParityOptions& opt) {
    std::ifstream f(opt.cases_path);
    if (!f) throw std::runtime_error("cannot open " + opt.cases_path);
    json doc = json::parse(f);
    fs::path root = fs::path(opt.cases_path).parent_path();
    int failed = 0, total = 0;
    json report = json::array();
    double worst = 0;
    std::string worst_id;
    auto t_all = std::chrono::steady_clock::now();
    for (const json& c : doc.at("cases")) {
        std::string id = c.at("id").get<std::string>();
        if (!opt.filter.empty() && id.find(opt.filter) == std::string::npos) continue;
        ++total;
        std::string kind = c.value("kind", "pipeline");
        double atol = c.value("atol", 1e-9), rtol = c.value("rtol", 1e-9);
        Metrics m;
        std::string error;
        auto t0 = std::chrono::steady_clock::now();
        try {
            if (kind == "digest") {
                Built b = build_params(*cd, c);
                json ours = params_to_json(b.digested);
                std::ifstream gf(root / c.at("golden").get<std::string>());
                json golden = json::parse(gf);
                std::vector<std::pair<std::string, json>> leaves;
                flatten_json(golden, "", leaves);
                for (auto& [path, gv] : leaves) {
                    // walk our json along the same path
                    const json* cur = &ours;
                    size_t pos = 0;
                    bool ok = true;
                    while (pos < path.size() && ok) {
                        if (path[pos] == '.') {
                            size_t e = path.find_first_of(".[", pos + 1);
                            std::string key = path.substr(pos + 1, e == std::string::npos ? e : e - pos - 1);
                            if (!cur->is_object() || !cur->contains(key)) ok = false;
                            else cur = &cur->at(key);
                            pos = e == std::string::npos ? path.size() : e;
                        } else {
                            size_t e = path.find(']', pos);
                            size_t idx = std::stoul(path.substr(pos + 1, e - pos - 1));
                            if (!cur->is_array() || idx >= cur->size()) ok = false;
                            else cur = &cur->at(idx);
                            pos = e + 1;
                        }
                    }
                    ++m.n;
                    double d = 0;
                    if (!ok) {
                        continue;  // field has no C++ counterpart (profiles are referenced by name)
                    } else if (gv.is_number() && cur->is_number()) {
                        d = std::fabs(gv.get<double>() - cur->get<double>());
                        m.worst_ratio = std::max(m.worst_ratio, d / (atol + rtol * std::fabs(gv.get<double>())));
                    } else if (gv.is_null() && cur->is_number() && path.rfind(".film_render.grain.", 0) == 0) {
                        continue;  // channel a B&W grain preset omits; never read for one channel
                    } else if (gv != *cur) {
                        d = INFINITY;
                        m.worst_ratio = INFINITY;
                        m.note = "mismatch at " + path;
                    }
                    m.max_abs = std::max(m.max_abs, d);
                }
            } else {
                std::vector<size_t> shape;
                Vec data;
                if (kind == "unit") {
                    run_unit(cd, c, root, shape, data);
                } else if (kind == "pipeline_stats") {
                    // Consecutive cases share one render (they differ only in the statistic).
                    static std::string cached_key;
                    static Image cached;
                    json keyj = c;
                    for (const char* k : {"id", "stat", "atol", "rtol", "golden"}) keyj.erase(k);
                    std::string key = keyj.dump();
                    if (key != cached_key) {
                        Built b = build_params(*cd, c);
                        b.options.seed = c.value("seed", 2024u);
                        Pipeline pipe(cd, b.digested, b.options);
                        int size = c.at("flat").at("size").get<int>();
                        Image in(size, size, 3, c.at("flat").at("value").get<double>());
                        cached = pipe.process(in, Tap::RgbIn, tap_from_string(c.value("collect", "rgb_out")));
                        cached_key = key;
                    }
                    data = patch_stats(cached, c.at("pixel_size_um").get<double>(), c.at("stat").get<std::string>());
                    shape = {data.size()};
                } else if (kind == "pipeline") {
                    Built b = build_params(*cd, c);
                    Pipeline pipe(cd, b.digested, b.options);
                    Image in = npy_load_image((root / c.at("input").get<std::string>()).string());
                    Image out = pipe.process(in, tap_from_string(c.value("inject", "rgb_in")),
                                             tap_from_string(c.value("collect", "rgb_out")));
                    int step = c.value("subsample", 1);
                    if (step > 1) {
                        Image sub((out.h + step - 1) / step, (out.w + step - 1) / step, out.c);
                        for (int y = 0; y < sub.h; ++y)
                            for (int x = 0; x < sub.w; ++x)
                                for (int k = 0; k < out.c; ++k) sub.at(y, x, k) = out.at(y * step, x * step, k);
                        out = std::move(sub);
                    }
                    shape = {static_cast<size_t>(out.h), static_cast<size_t>(out.w), static_cast<size_t>(out.c)};
                    data = std::move(out.v);
                } else {
                    throw std::runtime_error("unknown case kind '" + kind + "'");
                }
                if (!opt.dump_dir.empty()) {
                    fs::create_directories(opt.dump_dir);
                    npy_save((fs::path(opt.dump_dir) / (id + ".npy")).string(), shape, data);
                }
                NpyArray g = npy_load((root / c.at("golden").get<std::string>()).string());
                m = compare(shape, data, g.shape, g.data, atol, rtol);
            }
        } catch (const std::exception& e) {
            error = e.what();
        }
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        bool pass = error.empty() && m.shape_ok && m.worst_ratio <= 1.0;
        if (c.contains("p99_max") && m.p99 > c.at("p99_max").get<double>()) pass = false;
        if (c.contains("rmse_max") && m.rmse > c.at("rmse_max").get<double>()) pass = false;
        if (!pass) ++failed;
        if (m.worst_ratio > worst && std::isfinite(m.worst_ratio)) {
            worst = m.worst_ratio;
            worst_id = id;
        }
        if (!pass || opt.verbose)
            std::printf("%-4s %-58s max_abs=%.3e rmse=%.3e p99=%.3e ratio=%.3g %s%s (%.0f ms)\n", pass ? "PASS" : "FAIL",
                        id.c_str(), m.max_abs, m.rmse, m.p99, m.worst_ratio, m.note.c_str(),
                        error.empty() ? "" : (" ERROR: " + error).c_str(), ms);
        report.push_back({{"id", id}, {"pass", pass}, {"max_abs", m.max_abs}, {"rmse", m.rmse}, {"p99", m.p99},
                          {"ratio", std::isfinite(m.worst_ratio) ? json(m.worst_ratio) : json("inf")},
                          {"error", error}, {"ms", ms}});
    }
    double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t_all).count();
    std::printf("%s: %d/%d cases passed (worst tolerance ratio %.3g in %s) in %.1f s\n", opt.cases_path.c_str(),
                total - failed, total, worst, worst_id.c_str(), sec);
    if (!opt.report_path.empty()) {
        std::ofstream rf(opt.report_path);
        rf << json{{"cases", report}, {"passed", total - failed}, {"total", total}}.dump(1);
    }
    return failed;
}

}  // namespace spectra
