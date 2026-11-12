// SPDX-License-Identifier: GPL-3.0-or-later
// GPU backend checks that need no Python goldens: GPU vs CPU reference,
// tiled vs untiled rendering, automatic tiling from the memory budget, and
// crop / tile invariance of the grain.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>

#include "spectra/core/rng.hpp"
#include "spectra/gpu/gpu_pipeline.hpp"

using namespace spectra;

static int failures = 0;
static void check(bool ok, const char* what, double value, double limit) {
    std::printf("%-4s %-62s %.3e (limit %.1e)\n", ok ? "PASS" : "FAIL", what, value, limit);
    if (!ok) ++failures;
}

static Image scene(int h, int w) {
    Image img(h, w, 3);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            double u = (x + 0.5) / w, v = (y + 0.5) / h;
            PixelRng rng(3, 9, static_cast<uint32_t>(x / 3), static_cast<uint32_t>(y / 3));
            double t = 0.6 + 0.8 * rng.uniform();
            double blob = std::exp(-((u - 0.62) * (u - 0.62) + (v - 0.35) * (v - 0.35)) / 0.0008) * 30.0;
            img.at(y, x, 0) = (0.03 + 0.5 * u * u) * t + blob;
            img.at(y, x, 1) = (0.03 + 0.4 * u * v) * t + blob * 0.9;
            img.at(y, x, 2) = (0.05 + 0.3 * v) * t + blob * 0.7;
        }
    return img;
}

static double max_diff(const Image& a, const Image& b) {
    if (!a.same_shape(b)) return 1e9;
    double m = 0;
    for (size_t i = 0; i < a.v.size(); ++i) m = std::max(m, std::fabs(a.v[i] - b.v[i]));
    return m;
}

static double rms_diff(const Image& a, const Image& b) {
    double s = 0;
    for (size_t i = 0; i < a.v.size(); ++i) s += (a.v[i] - b.v[i]) * (a.v[i] - b.v[i]);
    return std::sqrt(s / static_cast<double>(a.v.size()));
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::puts("usage: test_gpu path/to/spektrafilm.sfpk");
        return 2;
    }
    auto pack = Pack::load(argv[1]);
    auto cd = std::make_shared<const ColourData>(pack);
    auto ctx = std::make_shared<gpu::Context>();
    std::printf("device: %s\n", ctx->info().name.c_str());
    Image img = scene(400, 592);

    Params p;
    p.camera.film_format_mm = 592 * 6.0 / 1000.0;  // 6 um pixels
    p.film_render.halation.boost_ev = 2.0;         // exercises the whole-image maximum
    Params d = digest_params(*pack, p);
    RenderOptions opt;
    opt.seed = 77;

    // 1. GPU vs the CPU reference. With grain the two backends draw from the same
    //    random streams but round differently (fp32 vs fp64), so individual grain
    //    counts can differ: the difference must stay grain-sized and unbiased.
    Image cpu = Pipeline(cd, d, opt).process(img);
    gpu::GpuRenderInfo info;
    Image full = gpu::GpuPipeline(ctx, cd, d, opt).process(img, Tap::RgbOut, &info);
    {
        double bias = 0;
        for (size_t i = 0; i < cpu.v.size(); ++i) bias += full.v[i] - cpu.v[i];
        bias = std::fabs(bias / static_cast<double>(cpu.v.size()));
        check(rms_diff(cpu, full) < 0.03, "GPU vs CPU reference, grain on (RMS, grain-sized)", rms_diff(cpu, full), 0.03);
        check(bias < 1e-3, "GPU vs CPU reference, grain on (mean bias)", bias, 1e-3);
    }
    check(!info.tiled && info.tiles == 1, "small image renders untiled", info.tiles, 1);
    Params det = d;  // deterministic effects only
    det.film_render.grain.active = false;
    det.print_render.glare.active = false;
    Image full_det = gpu::GpuPipeline(ctx, cd, det, opt).process(img);
    {
        Image a = Pipeline(cd, det, opt).process(img);
        check(max_diff(a, full_det) < 2e-3, "GPU vs CPU reference, deterministic effects (max)", max_diff(a, full_det), 2e-3);
    }

    // 2. Tiled vs untiled on the GPU. The default effects need a border wider than
    //    this small image, so the spatial reach is shortened to make real tiles.
    Params small = d;
    small.film_render.dir_couplers.diffusion_size_um = 12.0;
    small.film_render.dir_couplers.diffusion_tail_um = 12.0;
    small.film_render.halation.halation_first_sigma_um = {9.0, 9.0, 9.0};
    Params small_det = small;
    small_det.film_render.grain.active = false;
    small_det.print_render.glare.active = false;
    Image ref_det = gpu::GpuPipeline(ctx, cd, small_det, opt).process(img);
    Image ref_grain = gpu::GpuPipeline(ctx, cd, small, opt).process(img);
    for (int tile : {128, 200}) {
        gpu::GpuOptions go;
        go.tile_size = tile;
        gpu::GpuRenderInfo ti;
        Image tiled = gpu::GpuPipeline(ctx, cd, small_det, opt, go).process(img, Tap::RgbOut, &ti);
        char name[96];
        std::snprintf(name, sizeof name, "tiled %d px (%d tiles, border %d) vs untiled, no grain (max)", tile, ti.tiles, ti.apron);
        check(ti.tiles > 1 && max_diff(ref_det, tiled) < 2e-3, name, max_diff(ref_det, tiled), 2e-3);
        Image tg = gpu::GpuPipeline(ctx, cd, small, opt, go).process(img);
        std::snprintf(name, sizeof name, "tiled %d px vs untiled, grain on (RMS)", tile);
        check(rms_diff(ref_grain, tg) < 1.5e-3, name, rms_diff(ref_grain, tg), 1.5e-3);
    }

    // 3. Automatic tiling from the memory budget, and the budget is respected.
    {
        gpu::GpuOptions go;
        go.memory_budget_bytes = 12ull << 20;  // below the ~21 MiB working set of this image
        gpu::GpuRenderInfo ti;
        Image tiled = gpu::GpuPipeline(ctx, cd, small_det, opt, go).process(img, Tap::RgbOut, &ti);
        check(ti.tiled && ti.tiles > 1, "a small budget switches to tiles automatically (tiles)", ti.tiles, 2);
        check(ti.peak_gpu_bytes <= go.memory_budget_bytes, "peak GPU memory stays inside the budget (bytes)",
              static_cast<double>(ti.peak_gpu_bytes), static_cast<double>(go.memory_budget_bytes));
        check(max_diff(ref_det, tiled) < 2e-3, "budget-tiled vs untiled (max)", max_diff(ref_det, tiled), 2e-3);
    }

    // 3b. Extended features: the GPU kernels agree with the CPU implementation.
    {
        struct Ext {
            const char* name;
            void (*set)(Params&);
        };
        const Ext cases[] = {
            {"push +1.5 standard", [](Params& q) { q.extended.push_pull.active = true; q.extended.push_pull.film_stops = 1.5; q.extended.push_pull.print_stops = -1.0; }},
            {"pull -1 experimental", [](Params& q) { q.extended.push_pull.active = true; q.extended.push_pull.mode = "experimental"; q.extended.push_pull.film_stops = -1.0; }},
            {"push +2 experimental", [](Params& q) { q.extended.push_pull.active = true; q.extended.push_pull.mode = "experimental"; q.extended.push_pull.film_stops = 2.0; }},
            {"bleach bypass film + print", [](Params& q) { q.extended.bleach_bypass.active = true; q.extended.bleach_bypass.film_amount = 0.8; q.extended.bleach_bypass.print_amount = 0.6; }},
            {"bleach bypass, slide scan", [](Params& q) { q.film = "fujifilm_provia_100f"; q.io.scan_film = true; q.extended.bleach_bypass.active = true; q.extended.bleach_bypass.film_amount = 1.0; }},
            {"HDR PQ", [](Params& q) { q.extended.hdr_output.active = true; q.io.output_color_space = "ITU-R BT.2020"; }},
            {"HDR HLG clip", [](Params& q) { q.extended.hdr_output.active = true; q.extended.hdr_output.transfer = "hlg"; q.extended.hdr_output.tone_mapping = "clip"; q.io.output_color_space = "ITU-R BT.2020"; }},
            {"HDR PQ + cam16ucs (CPU gamut)", [](Params& q) { q.extended.hdr_output.active = true; q.io.output_color_space = "ITU-R BT.2020"; q.io.output_gamut_compress.algorithm = "cam16ucs"; }},
        };
        Image small_img = scene(96, 144);
        for (const Ext& e : cases) {
            Params q;
            q.camera.film_format_mm = 144 * 6.0 / 1000.0;
            q.debug.deactivate_stochastic_effects = true;
            e.set(q);
            Params dq = digest_params(*pack, q);
            Image a = Pipeline(cd, dq, opt).process(small_img), b = gpu::GpuPipeline(ctx, cd, dq, opt).process(small_img);
            char name[96];
            std::snprintf(name, sizeof name, "Extended on GPU vs CPU: %s (max)", e.name);
            check(max_diff(a, b) < 2e-3, name, max_diff(a, b), 2e-3);
        }
    }

    // 4. The working-set estimate is an upper bound of the measured peak.
    check(info.peak_gpu_bytes <= info.estimated_bytes, "working-set estimate >= measured peak (bytes)",
          static_cast<double>(info.peak_gpu_bytes), static_cast<double>(info.estimated_bytes));
    std::printf("     measured peak %.1f MiB, estimate %.1f MiB for %dx%d (%.1f / %.1f bytes per pixel)\n",
                info.peak_gpu_bytes / 1048576.0, info.estimated_bytes / 1048576.0, img.w, img.h,
                static_cast<double>(info.peak_gpu_bytes) / (img.w * img.h),
                static_cast<double>(info.estimated_bytes) / (img.w * img.h));

    // 5. CPU: a crop rendered with its origin reproduces the full render's grain field.
    {
        Params q;
        q.debug.deactivate_spatial_effects = true;  // isolate the random field (no blur, no global sums)
        q.camera.auto_exposure = false;
        q.camera.film_format_mm = p.camera.film_format_mm;
        Params dq = digest_params(*pack, q);
        Image whole = Pipeline(cd, dq, opt).process(img, Tap::RgbIn, Tap::CmyFilm);
        Image part(60, 80, 3);
        for (int y = 0; y < 60; ++y)
            for (int x = 0; x < 80; ++x)
                for (int k = 0; k < 3; ++k) part.at(y, x, k) = img.at(100 + y, 200 + x, k);
        RenderOptions o2 = opt;
        o2.origin_x = 200;
        o2.origin_y = 100;
        o2.full_long_edge_px = std::max(img.w, img.h);
        Image crop = Pipeline(cd, dq, o2).process(part, Tap::RgbIn, Tap::CmyFilm);
        double worst = 0;
        for (int y = 0; y < 60; ++y)
            for (int x = 0; x < 80; ++x)
                for (int k = 0; k < 3; ++k) worst = std::max(worst, std::fabs(crop.at(y, x, k) - whole.at(100 + y, 200 + x, k)));
        check(worst < 1e-12, "CPU: grain of a crop equals the full render's (max)", worst, 1e-12);
    }

    std::printf(failures ? "%d GPU check(s) FAILED\n" : "all GPU checks passed\n", failures);
    return failures ? 1 : 0;
}
