// SPDX-License-Identifier: GPL-3.0-or-later
// Sanity tests that need no Python goldens: finite output, determinism,
// thread invariance, resolution independence and whole-image statistic reuse.
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "spectra/core/parallel.hpp"
#include "spectra/core/rng.hpp"
#include "spectra/pipeline/pipeline.hpp"

using namespace spectra;
using nlohmann::json;

static int failures = 0;
#define CHECK(cond, ...)                                 \
    do {                                                 \
        if (!(cond)) {                                   \
            ++failures;                                  \
            std::printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            std::printf(__VA_ARGS__);                    \
            std::printf("\n");                           \
        }                                                \
    } while (0)

static Image test_scene(int h, int w) {
    Image img(h, w, 3);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            double u = (x + 0.5) / w, v = (y + 0.5) / h;
            double base = 0.02 + 0.5 * u * u;
            double blob = std::exp(-((u - 0.3) * (u - 0.3) + (v - 0.4) * (v - 0.4)) / 0.004) * 3.0;
            img.at(y, x, 0) = base + blob;
            img.at(y, x, 1) = base * (0.6 + 0.4 * v) + blob * 0.9;
            img.at(y, x, 2) = 0.05 + 0.3 * v + blob * 0.7;
        }
    return img;
}

static Image upsample2(const Image& a) {
    Image b(a.h * 2, a.w * 2, a.c);
    for (int y = 0; y < b.h; ++y)
        for (int x = 0; x < b.w; ++x)
            for (int k = 0; k < a.c; ++k) b.at(y, x, k) = a.at(y / 2, x / 2, k);
    return b;
}

static Image downsample2(const Image& a) {
    Image b(a.h / 2, a.w / 2, a.c);
    for (int y = 0; y < b.h; ++y)
        for (int x = 0; x < b.w; ++x)
            for (int k = 0; k < a.c; ++k)
                b.at(y, x, k) = 0.25 * (a.at(2 * y, 2 * x, k) + a.at(2 * y + 1, 2 * x, k) +
                                        a.at(2 * y, 2 * x + 1, k) + a.at(2 * y + 1, 2 * x + 1, k));
    return b;
}

static double mean_abs_diff(const Image& a, const Image& b) {
    double s = 0;
    for (size_t i = 0; i < a.v.size(); ++i) s += std::fabs(a.v[i] - b.v[i]);
    return s / static_cast<double>(a.v.size());
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::puts("usage: test_sanity path/to/spektrafilm.sfpk");
        return 2;
    }
    auto pack = Pack::load(argv[1]);
    auto cd = std::make_shared<const ColourData>(pack);
    int hw_threads = thread_count();

    // 1. Full default render of every film family: finite and inside [0, 1].
    Image scene = test_scene(96, 144);
    for (auto [film, print, scan_film] :
         {std::tuple{"kodak_portra_400", "kodak_portra_endura", false},
          std::tuple{"kodak_vision3_500t", "kodak_2383", false},
          std::tuple{"fujifilm_velvia_100", "kodak_portra_endura", true},
          std::tuple{"kodak_doublex", "kodak_2302", false},
          std::tuple{"kodak_trix", "kodak_2302", true}}) {
        Params p;
        p.film = film;
        p.print = print;
        p.io.scan_film = scan_film;
        p.camera.film_format_mm = 0.9;  // ~6 um pixels for this small image
        Image out = simulate(cd, scene, p);
        bool finite = true;
        double lo = 1e9, hi = -1e9;
        for (double v : out.v) {
            finite = finite && std::isfinite(v);
            lo = std::min(lo, v);
            hi = std::max(hi, v);
        }
        CHECK(finite, "%s: output has NaN or Inf", film);
        // The scanner unsharp mask runs after gamut compression (as upstream), so
        // small overshoots around hard edges are expected; anything larger is a bug.
        CHECK(lo >= -0.25 && hi <= 1.25, "%s: output far outside [0, 1]: %g .. %g", film, lo, hi);
        CHECK(out.c == 3 && out.h == scene.h && out.w == scene.w, "%s: unexpected output shape", film);
    }

    // 2. Determinism, seed dependence and thread invariance of the stochastic effects.
    {
        Params p;
        p.camera.film_format_mm = 0.9;
        Params d = digest_params(*pack, p);
        RenderOptions o;
        o.seed = 42;
        Pipeline pipe(cd, d, o);
        Image a = pipe.process(scene), b = pipe.process(scene);
        CHECK(a.v == b.v, "same seed must give identical output");
        thread_count() = 1;
        Image c = pipe.process(scene);
        thread_count() = hw_threads;
        CHECK(a.v == c.v, "output must not depend on the thread count");
        o.seed = 43;
        Image e = Pipeline(cd, d, o).process(scene);
        CHECK(mean_abs_diff(a, e) > 1e-4, "a different seed must change the grain");
        CHECK(mean_abs_diff(a, e) < 0.05, "a different seed must not change the picture");
    }

    // 3. Resolution independence: the same scene sampled twice as finely gives
    //    the same picture once averaged back (deterministic effects only).
    {
        Params p;
        p.debug.deactivate_stochastic_effects = true;
        p.camera.film_format_mm = 0.9;
        p.camera.lens_blur_um = 10.0;
        p.scanner.lens_blur = 2.0;         // a pixel-unit parameter
        p.scanner.unsharp_mask = {0.0, 0.0};
        Params d = digest_params(*pack, p);
        // Fine texture so that pixel-scale blurs matter.
        Image lo_in = test_scene(96, 144);
        for (int y = 0; y < lo_in.h; ++y)
            for (int x = 0; x < lo_in.w; ++x) {
                PixelRng rng(7, 1, static_cast<uint32_t>(x / 2), static_cast<uint32_t>(y / 2));
                double t = 0.5 + rng.uniform();
                for (int k = 0; k < 3; ++k) lo_in.at(y, x, k) *= t;
            }
        Image hi_in = upsample2(lo_in);
        RenderOptions phys;  // default: resolution independent
        Image lo_out = Pipeline(cd, d, phys).process(lo_in);
        Image hi_out = downsample2(Pipeline(cd, d, phys).process(hi_in));
        double diff_phys = mean_abs_diff(lo_out, hi_out);
        RenderOptions py;
        py.match_python = true;
        Image lo_py = Pipeline(cd, d, py).process(lo_in);
        Image hi_py = downsample2(Pipeline(cd, d, py).process(hi_in));
        double diff_py = mean_abs_diff(lo_py, hi_py);
        std::printf("resolution independence: mean |1x - 2x| = %.5f (default) vs %.5f (match-python)\n",
                    diff_phys, diff_py);
        CHECK(diff_phys < 0.004, "default mode should be resolution independent (%.5f)", diff_phys);
        CHECK(diff_phys < diff_py, "default mode should be more consistent than match-python mode");
    }

    // 4. Whole-image statistics reuse: a crop rendered with the full image's
    //    statistics equals the same region of the full render (non-spatial setup).
    {
        Params p;
        p.debug.deactivate_spatial_effects = true;
        p.debug.deactivate_stochastic_effects = true;
        p.film_render.halation.boost_ev = 3.0;
        p.film_render.halation.protect_ev = 1.0;
        Params d = digest_params(*pack, p);
        RenderOptions o;
        RenderStats stats;
        Image full = Pipeline(cd, d, o).process(scene, Tap::RgbIn, Tap::RgbOut, &stats);
        CHECK(stats.autoexposure_ev.has_value() && stats.boost_max_raw.has_value(), "stats not reported");
        Image crop(40, 50, 3);
        for (int y = 0; y < 40; ++y)
            for (int x = 0; x < 50; ++x)
                for (int k = 0; k < 3; ++k) crop.at(y, x, k) = scene.at(50 + y, 80 + x, k);
        RenderOptions o2;
        o2.autoexposure_ev = stats.autoexposure_ev;
        o2.boost_max_raw = stats.boost_max_raw;
        o2.full_long_edge_px = std::max(scene.h, scene.w);
        Image part = Pipeline(cd, d, o2).process(crop);
        double worst = 0;
        for (int y = 0; y < 40; ++y)
            for (int x = 0; x < 50; ++x)
                for (int k = 0; k < 3; ++k)
                    worst = std::max(worst, std::fabs(part.at(y, x, k) - full.at(50 + y, 80 + x, k)));
        CHECK(worst < 1e-12, "crop with reused statistics differs from the full render by %g", worst);
        Image naive = Pipeline(cd, d, o).process(crop);
        double naive_diff = 0;
        for (int y = 0; y < 40; ++y)
            for (int x = 0; x < 50; ++x)
                for (int k = 0; k < 3; ++k)
                    naive_diff = std::max(naive_diff, std::fabs(naive.at(y, x, k) - full.at(50 + y, 80 + x, k)));
        CHECK(naive_diff > 1e-3, "test is vacuous: the crop matches without reusing statistics");
    }

    // 5. Parameter JSON round trip.
    {
        Params p;
        p.film_render.chemistry.development_time = 6.5;
        p.io.output_gamut_compress.lightness_compression = std::nullopt;
        json j = params_to_json(p);
        Params q = params_from_json(j);
        CHECK(params_to_json(q) == j, "parameter JSON round trip changed values");
        bool threw = false;
        try {
            apply_params_patch(q, json{{"camera", {{"no_such_field", 1}}}});
        } catch (const std::exception&) {
            threw = true;
        }
        CHECK(threw, "unknown parameter names must be rejected");
    }

    std::printf(failures ? "%d sanity check(s) FAILED\n" : "all sanity checks passed\n", failures);
    return failures ? 1 : 0;
}
