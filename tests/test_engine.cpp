// SPDX-License-Identifier: GPL-3.0-or-later
// Engine tests: the stage cache must give exactly what a fresh render gives for
// a change of any parameter; previews, calibration, presets, LUT export.
#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "spectra/engine/engine.hpp"
#include "spectra/model/metering.hpp"
#ifdef SPECTRA_HAVE_GPU
#include "spectra/gpu/gpu_pipeline.hpp"
#endif

using namespace spectra;
using nlohmann::json;

static int g_fail = 0;
#define CHECK(cond, ...)                         \
    do {                                         \
        if (!(cond)) {                           \
            ++g_fail;                            \
            std::printf("FAIL: " __VA_ARGS__);   \
            std::printf("\n");                   \
        }                                        \
    } while (0)

static Image test_image(int w, int h) {
    Image img(h, w, 3);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            double u = (x + 0.5) / w, v = (y + 0.5) / h;
            double base = 0.184 * std::pow(2.0, 5.0 * (u - 0.5));
            double tex = 0.9 + 0.2 * std::sin(x * 0.9) * std::cos(y * 0.7);
            img.at(y, x, 0) = base * tex * (0.7 + 0.6 * v);
            img.at(y, x, 1) = base * tex;
            img.at(y, x, 2) = base * tex * (1.3 - 0.6 * v);
        }
    return img;
}

static double max_abs_diff(const Image& a, const Image& b) {
    if (!a.same_shape(b)) return 1e9;
    double m = 0;
    for (size_t i = 0; i < a.v.size(); ++i) {
        double d = std::fabs(a.v[i] - b.v[i]);
        if (!(d <= m)) m = std::isfinite(d) ? d : 1e9;
    }
    return m;
}

static bool finite(const Image& a) {
    for (double v : a.v)
        if (!std::isfinite(v)) return false;
    return !a.v.empty();
}

// Every leaf of the parameter tree as (json pointer, value).
static void leaves(const json& j, const std::string& path, std::vector<std::pair<std::string, json>>& out) {
    if (j.is_object()) {
        for (auto it = j.begin(); it != j.end(); ++it) leaves(it.value(), path + "/" + it.key(), out);
    } else {
        out.emplace_back(path, j);
    }
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: test_engine pack.sfpk\n");
        return 2;
    }
    auto pack = Pack::load(argv[1]);
    auto cd = std::make_shared<const ColourData>(pack);

#ifdef SPECTRA_HAVE_GPU
    std::shared_ptr<gpu::Context> ctx;
    try {
        ctx = std::make_shared<gpu::Context>();
    } catch (const std::exception& e) {
        std::printf("no Vulkan device (%s): GPU checks skipped\n", e.what());
    }
    if (ctx) {
        // ---- 1. stage cache == fresh render, for a change of every parameter
        Image img = test_image(96, 64);
        Params base = digest_params(*pack, Params{});
        base.scanner.white_correction = true;  // exercise the corrections that couple stages
        base.scanner.black_correction = true;
        RenderOptions ro;
        ro.autoexposure_ev = 0.25;
        json jbase = params_to_json(base);
        std::vector<std::pair<std::string, json>> lv;
        leaves(jbase, "", lv);
        gpu::StageCache cache;
        int tested = 0, reused = 0;
        double worst = 0;
        std::string worst_name;
        auto fresh = [&](const Params& p) { return gpu::GpuPipeline(ctx, cd, p, ro).process(img); };
        auto cached = [&](const Params& p, gpu::GpuRenderInfo* info) {
            return gpu::GpuPipeline(ctx, cd, p, ro).process(img, Tap::RgbOut, info, &cache);
        };
        for (auto& [path, value] : lv) {
            std::vector<json> variants;
            if (value.is_boolean()) variants.push_back(!value.get<bool>());
            else if (value.is_number_integer()) variants.push_back(value.get<long>() + 1);
            else if (value.is_number_float()) variants.push_back(value.get<double>() * 1.15 + 0.07);
            else if (value.is_array() && !value.empty() && value[0].is_number()) {
                json a = value;
                for (auto& e : a) e = e.get<double>() * 1.15 + 0.07;
                variants.push_back(a);
            } else if (path == "/print") variants.push_back("kodak_supra_endura");
            else if (path == "/io/output_color_space") variants.push_back("Display P3");
            else if (path == "/io/output_gamut_compress/algorithm") variants.push_back("aces_rgc");
            else if (path == "/extended/push_pull/mode") variants.push_back("experimental");
            else if (path == "/extended/hdr_output/transfer") variants.push_back("hlg");
            if (variants.empty()) continue;
            for (auto& nv : variants) {
                Params p = base;
                // Features that are off hide their parameters: switch the Extended ones on.
                p.extended.push_pull.active = true;
                p.extended.push_pull.film_stops = 0.5;
                p.extended.bleach_bypass.active = true;
                p.extended.bleach_bypass.film_amount = 0.3;
                p.extended.bleach_bypass.print_amount = 0.2;
                Params before = p;
                json patch;
                patch[json::json_pointer(path)] = nv;
                Image want;
                try {
                    apply_params_patch(p, patch);
                    want = fresh(p);
                } catch (const std::exception&) {
                    continue;  // not a valid value for this parameter
                }
                cached(before, nullptr);
                gpu::GpuRenderInfo info;
                Image got = cached(p, &info);
                double d = max_abs_diff(got, want);
                ++tested;
                if (info.first_stage_run > 0) ++reused;
                if (d > worst) {
                    worst = d;
                    worst_name = path;
                }
                CHECK(d <= 1e-6, "stage cache differs from a fresh render for %s: %.3g (first stage run %d)",
                      path.c_str(), d, info.first_stage_run);
            }
        }
        cache.clear(*ctx);
        std::printf("stage cache: %d parameter changes, %d reused earlier stages, worst difference %.3g (%s)\n", tested,
                    reused, worst, worst_name.c_str());
        CHECK(tested > 100 && reused > 40, "the cache test covered too little (%d, %d)", tested, reused);

        // ---- 2. cancellation leaves no GPU memory behind and the next render works
        {
            Image big = test_image(512, 340);
            uint64_t before = ctx->allocated_bytes() - ctx->pooled_bytes();
            std::atomic<bool> cancel{true};
            ctx->set_cancel_flag(&cancel);
            bool threw = false;
            try {
                gpu::GpuPipeline(ctx, cd, base, ro).process(big);
            } catch (const gpu::Cancelled&) {
                threw = true;
            }
            ctx->set_cancel_flag(nullptr);
            CHECK(threw, "a raised cancel flag did not stop the render");
            uint64_t after = ctx->allocated_bytes() - ctx->pooled_bytes();
            CHECK(after == before, "cancelled render leaked %lld bytes of GPU memory",
                  static_cast<long long>(after) - static_cast<long long>(before));
            Image ok = gpu::GpuPipeline(ctx, cd, base, ro).process(big);
            CHECK(finite(ok), "render after a cancellation is not finite");
            ctx->trim_pool();
        }

        // ---- 3. batching does not change the result
        {
            Image im = test_image(200, 130);
            gpu::BatchOptions one;
            one.max_dispatches = 1;
            ctx->set_batching(one);
            Image a = gpu::GpuPipeline(ctx, cd, base, ro).process(im);
            gpu::GpuRenderInfo ia, ib;
            gpu::GpuPipeline(ctx, cd, base, ro).process(im, Tap::RgbOut, &ia);
            ctx->set_batching(gpu::BatchOptions{});
            Image b = gpu::GpuPipeline(ctx, cd, base, ro).process(im, Tap::RgbOut, &ib);
            CHECK(max_abs_diff(a, b) == 0.0, "batched dispatches change the result: %.3g", max_abs_diff(a, b));
            CHECK(ib.submits * 3 < ia.submits, "batching did not reduce submissions (%llu vs %llu)",
                  static_cast<unsigned long long>(ib.submits), static_cast<unsigned long long>(ia.submits));
            std::printf("batching: %llu dispatches in %llu submissions (unbatched: %llu)\n",
                        static_cast<unsigned long long>(ib.dispatches), static_cast<unsigned long long>(ib.submits),
                        static_cast<unsigned long long>(ia.submits));
        }
        ctx.reset();
    }
#endif

    // ---- 4. engine: previews, 100 % view, full render, calibration (GPU or CPU fallback)
    for (bool force_cpu : {false, true}) {
        engine::EngineConfig cfg;
        cfg.force_cpu = force_cpu;
        engine::Engine eng(pack, cd, cfg);
        if (force_cpu) CHECK(!eng.has_gpu(), "force_cpu still uses a GPU");
        std::printf("engine on %s\n", eng.device_name().c_str());
        if (!force_cpu) {
            engine::Calibration c = eng.calibrate();
            CHECK(c.preview_long_edge >= 256 && c.draft_long_edge >= 128 && c.draft_long_edge < c.preview_long_edge,
                  "calibration picked odd sizes: %d / %d", c.preview_long_edge, c.draft_long_edge);
            engine::Calibration back = engine::Calibration::from_json(json::parse(c.to_json().dump()));
            CHECK(back.preview_long_edge == c.preview_long_edge && back.batch_max_dispatches == c.batch_max_dispatches &&
                      back.device == c.device,
                  "calibration does not survive a JSON round trip");
        }
        engine::Calibration small = eng.calibration();
        small.preview_long_edge = force_cpu ? 160 : 320;
        small.draft_long_edge = force_cpu ? 96 : 128;
        eng.set_calibration(small);
        auto img = std::make_shared<Image>(test_image(force_cpu ? 300 : 900, force_cpu ? 200 : 600));
        eng.set_image(img, "", false);
        Params p;
        engine::RenderResult d = eng.render_preview(p, engine::Level::Draft, {});
        engine::RenderResult r = eng.render_preview(p, engine::Level::Preview, {});
        CHECK(finite(d.image) && finite(r.image), "preview is not finite");
        CHECK(std::max(d.image.w, d.image.h) == small.draft_long_edge, "draft size %d", d.image.w);
        CHECK(std::max(r.image.w, r.image.h) == small.preview_long_edge, "preview size %d", r.image.w);
        engine::RenderResult again = eng.render_preview(p, engine::Level::Preview, {});
        CHECK(max_abs_diff(again.image, r.image) == 0.0, "repeating a preview changes it");
        if (eng.has_gpu()) CHECK(again.first_stage == 5, "an unchanged preview was re-rendered (stage %d)", again.first_stage);
        Params q = p;
        q.enlarger.print_exposure = 1.3;
        engine::RenderResult r2 = eng.render_preview(q, engine::Level::Preview, {});
        if (eng.has_gpu()) CHECK(r2.first_stage == 3, "a print change re-ran from stage %d", r2.first_stage);
        CHECK(max_abs_diff(r2.image, r.image) > 1e-3, "a print exposure change did nothing");
        // The draft and the refined preview show the same picture (grain off: it does not survive a reduction).
        Params ng = p;
        ng.film_render.grain.active = false;
        ng.print_render.glare.active = false;
        Image a = eng.render_preview(ng, engine::Level::Preview, {}).image;
        Image b = eng.render_preview(ng, engine::Level::Draft, {}).image;
        Image a_small = downsample_area(a, std::max(b.w, b.h));
        double diff = 0;
        if (a_small.same_shape(b)) {
            for (size_t i = 0; i < b.v.size(); ++i) diff += std::fabs(a_small.v[i] - b.v[i]);
            diff /= b.v.size();
        } else {
            diff = 1e9;
        }
        CHECK(diff < 0.02, "draft and refined preview disagree: mean abs %.4f", diff);
        // 100 % view of a part
        engine::ViewRect view{img->w / 4, img->h / 4, img->w / 3, img->h / 3};
        engine::RenderResult v = eng.render_preview(ng, engine::Level::Preview, view);
        CHECK(v.image.w == view.w && v.image.h == view.h, "100%% view has size %dx%d", v.image.w, v.image.h);
        engine::RenderResult full = eng.render_full(ng);
        CHECK(full.image.w == img->w && full.image.h == img->h && finite(full.image), "full render is wrong");
        // The centre of the view matches the full render (edges carry the capped apron).
        double dv = 0;
        int n = 0;
        for (int y = view.h / 4; y < view.h * 3 / 4; ++y)
            for (int x = view.w / 4; x < view.w * 3 / 4; ++x)
                for (int k = 0; k < 3; ++k, ++n)
                    dv = std::max(dv, std::fabs(v.image.at(y, x, k) - full.image.at(y + view.y, x + view.x, k)));
        CHECK(dv < 0.02, "100%% view differs from the full render by %.4f", dv);
        std::printf("  100%% view vs full render (centre): max %.4f; draft vs refined: mean %.4f\n", dv, diff);
        Image in = eng.render_input(engine::Level::Preview, {});
        CHECK(finite(in) && in.w == r.image.w, "input view is wrong");
        // cancellation
        std::atomic<bool> cancel{true};
        bool threw = false;
        Params z = p;
        z.camera.exposure_compensation_ev = 0.4;
        try {
            eng.render_preview(z, engine::Level::Preview, {}, &cancel);
        } catch (const std::exception& e) {
            threw = engine::is_cancel(e);
        }
        CHECK(threw, "the engine ignored the cancel flag");
        CHECK(finite(eng.render_preview(z, engine::Level::Preview, {}).image), "render after cancel failed");
    }

    // ---- 5. presets
    {
        Params p;
        p.film = "kodak_gold_200";
        p.enlarger.print_exposure = 1.2;
        p.film_render.grain.active = false;
        json d = engine::params_diff(p);
        CHECK(d.size() == 3, "preset diff has %zu top-level entries", d.size());
        Params back = engine::params_from_diff(d);
        CHECK(params_to_json(back) == params_to_json(p), "preset diff does not round-trip");
        engine::Preset pr{"x", "y", d};
        engine::Preset pr2 = engine::preset_from_json(json::parse(engine::preset_to_json(pr).dump()));
        CHECK(pr2.diff == d && pr2.name == "x", "preset JSON does not round-trip");
        Image img = test_image(48, 32);
        for (auto& b : engine::builtin_presets()) {
            Params bp = engine::params_from_diff(b.diff);
            try {
                Image out = simulate(cd, img, bp);
                double mean = 0;
                for (double v : out.v) mean += v;
                mean /= out.v.size();
                CHECK(finite(out) && mean > 0.03 && mean < 0.97, "built-in preset '%s' renders badly (mean %.3f)",
                      b.name.c_str(), mean);
            } catch (const std::exception& e) {
                CHECK(false, "built-in preset '%s' failed: %s", b.name.c_str(), e.what());
            }
        }
    }

    // ---- 6. LUT export
    {
        Params p;
        for (const char* input : {"srgb", "rec709", "display_p3", "acescct"}) {
            engine::LutOptions lo;
            lo.size = 9;
            lo.input = input;
            Image lut = engine::render_lut_lattice(*pack, cd, p, lo);
            CHECK(lut.h == 81 && lut.w == 9 && finite(lut), "LUT lattice (%s) is wrong", input);
            // neutral axis: monotonic and close to neutral
            double prev = -1;
            bool mono = true;
            double cast = 0;
            for (int i = 0; i < 9; ++i) {
                const double* px = lut.px(i + 9 * i, i);
                double g = px[1];
                if (g < prev - 1e-6) mono = false;
                prev = g;
                cast = std::max({cast, std::fabs(px[0] - px[1]), std::fabs(px[2] - px[1])});
            }
            CHECK(mono, "LUT (%s) neutral axis is not monotonic", input);
            CHECK(cast < 0.08, "LUT (%s) neutral axis has a %.3f cast", input, cast);
        }
        engine::LutOptions lo;
        lo.size = 5;
        std::string path = "test_lut.cube";
        engine::export_cube_lut(*pack, cd, p, lo, path);
        std::ifstream f(path);
        std::string line;
        int rows = 0;
        bool size_ok = false;
        while (std::getline(f, line)) {
            if (line.rfind("LUT_3D_SIZE 5", 0) == 0) size_ok = true;
            if (!line.empty() && (std::isdigit(static_cast<unsigned char>(line[0])) || line[0] == '-')) ++rows;
        }
        CHECK(size_ok && rows == 125, ".cube file is malformed (%d rows)", rows);
        std::remove(path.c_str());
    }

    std::printf(g_fail ? "%d check(s) FAILED\n" : "all engine checks passed\n", g_fail);
    return g_fail ? 1 : 0;
}
