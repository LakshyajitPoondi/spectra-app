// SPDX-License-Identifier: GPL-3.0-or-later
// spectra-cli bench: timing and memory of the render paths.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "spectra/core/parallel.hpp"
#include "spectra/core/rng.hpp"
#include "spectra/pipeline/pipeline.hpp"
#ifdef SPECTRA_HAVE_GPU
#include "spectra/gpu/gpu_pipeline.hpp"
#endif
#ifdef SPECTRA_HAVE_IO
#include "spectra/io/raw.hpp"
#endif

namespace spectra {

namespace {

double ms_since(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}

// Synthetic photograph-like scene (smooth gradients, texture, a few highlights).
Image synthetic(int w, int h) {
    Image img(h, w, 3);
    parallel_for(h, [&](long y0, long y1) {
        for (long y = y0; y < y1; ++y)
            for (int x = 0; x < w; ++x) {
                double u = (x + 0.5) / w, v = (y + 0.5) / h;
                PixelRng rng(11, 5, static_cast<uint32_t>(x / 4), static_cast<uint32_t>(y / 4));
                double t = 0.6 + 0.8 * rng.uniform();
                double blob = std::exp(-((u - 0.7) * (u - 0.7) + (v - 0.3) * (v - 0.3)) / 0.0004) * 20.0;
                img.at(static_cast<int>(y), x, 0) = (0.03 + 0.45 * u * u) * t + blob;
                img.at(static_cast<int>(y), x, 1) = (0.03 + 0.35 * u * v + 0.1 * v) * t + blob * 0.9;
                img.at(static_cast<int>(y), x, 2) = (0.05 + 0.3 * v) * t + blob * 0.7;
            }
    });
    return img;
}

struct Line {
    std::string text;
};

}  // namespace

// Returns 0, or 1 when the peak process memory exceeded `limit_gb`.
int run_bench(std::shared_ptr<const ColourData> cd, std::map<std::string, std::string>& opt) {
    std::string backend = opt.count("backend") ? opt["backend"] : "gpu";
    std::vector<double> sizes;
    {
        std::stringstream ss(opt.count("mp") ? opt["mp"] : "20,30");
        std::string tok;
        while (std::getline(ss, tok, ',')) sizes.push_back(std::stod(tok));
    }
    int preview_px = opt.count("preview") ? std::stoi(opt["preview"]) : 1600;
    int repeats = opt.count("repeats") ? std::stoi(opt["repeats"]) : 3;
    double limit_gb = opt.count("limit-gb") ? std::stod(opt["limit-gb"]) : 10.0;
    std::string out;
    auto say = [&](const char* fmt, auto... args) {
        char buf[1024];
        std::snprintf(buf, sizeof buf, fmt, args...);
        std::fputs(buf, stdout);
        std::fflush(stdout);
        out += buf;
    };
    Params p;
    if (opt.count("film")) p.film = opt["film"];
    if (opt.count("print")) p.print = opt["print"];
    Params d = digest_params(cd->pack(), p);
    RenderOptions ro;
    ro.seed = 1;

#ifdef SPECTRA_HAVE_GPU
    std::shared_ptr<gpu::Context> ctx;
    gpu::GpuOptions go;
    if (backend == "gpu") {
        gpu::ContextOptions co;
        if (opt.count("gpu")) co.device = opt["gpu"];
        ctx = std::make_shared<gpu::Context>(co);
        if (opt.count("tile")) go.tile_size = std::stoi(opt["tile"]);
        if (opt.count("budget-mb")) go.memory_budget_bytes = std::stoull(opt["budget-mb"]) << 20;
        const auto& i = ctx->info();
        say("device: %s (vendor 0x%04x, driver %s, %s, memory budget %.1f GiB)\n", i.name.c_str(), i.vendor_id,
            i.driver.c_str(), i.is_software() ? "SOFTWARE RENDERER - timings are not representative" : "hardware",
            ctx->query_budget_bytes() / 1073741824.0);
    }
#else
    if (backend == "gpu") throw std::runtime_error("this build has no GPU backend");
#endif
    say("backend: %s, film %s -> print %s, all default effects (grain, halation, DIR couplers, glare)\n",
        backend.c_str(), d.film.c_str(), d.print.c_str());

    struct Result {
        Image img;
        double ms = 0;
        std::map<std::string, double> stages;
        uint64_t gpu_peak = 0;
        int tiles = 1;
        std::vector<std::string> fallbacks;
    };
    auto render = [&](const Image& in, const Params& params, const RenderOptions& options) {
        Result r;
        auto t = std::chrono::steady_clock::now();
#ifdef SPECTRA_HAVE_GPU
        if (backend == "gpu") {
            gpu::GpuRenderInfo gi;
            r.img = gpu::GpuPipeline(ctx, cd, params, options, go).process(in, Tap::RgbOut, &gi);
            r.stages = gi.stage_ms;
            r.gpu_peak = gi.peak_gpu_bytes;
            r.tiles = gi.tiles;
            r.fallbacks = gi.cpu_fallbacks;
        } else
#endif
            r.img = Pipeline(cd, params, options).process(in);
        r.ms = ms_since(t);
        return r;
    };
    auto stage_line = [&](const Result& r) {
        std::string s;
        for (const char* name : {"film exposure", "film development", "grain", "print exposure", "print development", "scan", "download"}) {
            auto it = r.stages.find(name);
            if (it == r.stages.end()) continue;
            char buf[64];
            std::snprintf(buf, sizeof buf, "%s %.0f, ", name, it->second);
            s += buf;
        }
        if (s.size() > 2) s.resize(s.size() - 2);
        return s;
    };

    for (double mp : sizes) {
        int w = static_cast<int>(std::lround(std::sqrt(mp * 1e6 * 1.5) / 16) * 16), h = static_cast<int>(std::lround(w / 1.5 / 16) * 16);
        say("\n== %.0f MP (%d x %d) ==\n", mp, w, h);
        Image full = synthetic(w, h);
        // full-resolution render
        Result fr = render(full, d, ro);
        say("full-resolution render: %.0f ms, %d tile(s), peak GPU memory %.0f MiB, peak process memory %.2f GiB\n", fr.ms,
            fr.tiles, fr.gpu_peak / 1048576.0, peak_process_memory_bytes() / 1073741824.0);
        if (!fr.stages.empty()) say("  stages (ms): %s\n", stage_line(fr).c_str());
        for (auto& f : fr.fallbacks) say("  ran on the CPU: %s\n", f.c_str());
        fr.img = Image();
        // interactive preview: the image reduced to the viewport, pixel size kept physical
        Image small = downsample_area(full, preview_px);
        render(small, d, ro);  // warm-up (pipeline creation, table upload)
        double best = 1e30, sum = 0;
        Result last;
        for (int i = 0; i < repeats; ++i) {
            last = render(small, d, ro);
            best = std::min(best, last.ms);
            sum += last.ms;
        }
        say("preview %d x %d, complete pipeline: best %.1f ms, mean %.1f ms (%d runs)\n", small.w, small.h, best,
            sum / repeats, repeats);
        if (!last.stages.empty()) say("  stages (ms): %s\n", stage_line(last).c_str());
        // One slider change re-runs only the stages after the changed parameter.
        if (!last.stages.empty()) {
            auto get = [&](const char* n) { return last.stages.count(n) ? last.stages.at(n) : 0.0; };
            double scan = get("scan") + get("download");
            double print = get("print exposure") + get("print development") + scan;
            double film = get("film development") + get("grain") + print;
            say("  slider-change cost with stage caching (ms): scanner / output %.1f, print exposure / filters %.1f, "
                "film development / grain %.1f, camera exposure %.1f\n", scan, print, film, last.ms);
        }
    }

#ifdef SPECTRA_HAVE_IO
    if (opt.count("in")) {
        std::stringstream ss(opt["in"]);
        std::string path;
        while (std::getline(ss, path, ',')) {
            say("\n== RAW open to first preview: %s ==\n", path.c_str());
            for (int run = 0; run < 2; ++run) {
                auto t_all = std::chrono::steady_clock::now();
                RawLoadOptions rlo;
                rlo.half_size = true;
                RawInfo info;
                std::string space;
                Image img = load_any(*cd, path, rlo, &space, &info);
                double t_decode = ms_since(t_all);
                auto t = std::chrono::steady_clock::now();
                Image small = downsample_area(img, preview_px);
                double t_reduce = ms_since(t);
                t = std::chrono::steady_clock::now();
                Params q = p;
                if (!space.empty()) q.io.input_color_space = space;
                Params dq = digest_params(cd->pack(), q);
                Result r = render(small, dq, ro);
                double t_render = ms_since(t);
                say("  %s: %s %s %dx%d -> %dx%d: decode (half-size) %.0f ms, reduce %.0f ms, tables + render %.0f ms, "
                    "TOTAL %.0f ms\n", run == 0 ? "cold" : "warm", info.make.c_str(), info.model.c_str(), info.width * 2,
                    info.height * 2, small.w, small.h, t_decode, t_reduce, t_render, ms_since(t_all));
            }
        }
    }
#endif
    double peak_gb = peak_process_memory_bytes() / 1073741824.0;
    bool ok = peak_gb <= limit_gb;
    say("\npeak process memory over the whole benchmark: %.2f GiB (limit %.0f GiB): %s\n", peak_gb, limit_gb,
        ok ? "OK" : "EXCEEDED");
    if (opt.count("report")) {
        if (std::FILE* f = std::fopen(opt["report"].c_str(), "a")) {
            std::fputs(out.c_str(), f);
            std::fclose(f);
        }
    }
    return ok ? 0 : 1;
}

}  // namespace spectra
