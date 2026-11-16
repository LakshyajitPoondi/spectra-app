// SPDX-License-Identifier: GPL-3.0-or-later
#include "spectra/engine/engine.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>

#include "spectra/core/parallel.hpp"
#include "spectra/model/metering.hpp"
#ifdef SPECTRA_HAVE_GPU
#include "spectra/gpu/gpu_pipeline.hpp"
#endif

namespace spectra::engine {

using nlohmann::json;

namespace {

double ms_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

// Border rendered around a partial view so that spatial effects have context.
// A full render uses the exact support of every effect; the preview caps it
// (see DEVIATIONS.md, P-01).
constexpr int kViewApron = 128;

Image crop_rect(const Image& img, const ViewRect& r) {
    Image out(r.h, r.w, 3);
    parallel_for(r.h, [&](long a, long b) {
        for (long y = a; y < b; ++y)
            for (int x = 0; x < r.w; ++x)
                std::copy(img.px(static_cast<int>(y) + r.y, x + r.x), img.px(static_cast<int>(y) + r.y, x + r.x) + 3,
                          out.px(static_cast<int>(y), x));
    }, 16);
    return out;
}

// Deterministic test picture for the calibration: a wide exposure ramp with
// colour variation and fine detail, so every stage does representative work.
Image calibration_image(int w, int h) {
    Image img(h, w, 3);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            double u = (x + 0.5) / w, v = (y + 0.5) / h;
            double base = 0.184 * std::pow(2.0, 6.0 * (u - 0.5));
            uint32_t hsh = static_cast<uint32_t>(x) * 73856093u ^ static_cast<uint32_t>(y) * 19349663u;
            hsh ^= hsh >> 13;
            hsh *= 0x5bd1e995u;
            double n = 0.9 + 0.2 * ((hsh >> 8) & 0xffff) / 65535.0;
            img.at(y, x, 0) = base * n * (0.6 + 0.8 * v);
            img.at(y, x, 1) = base * n;
            img.at(y, x, 2) = base * n * (1.4 - 0.8 * v);
        }
    return img;
}

void check_cancel(const std::atomic<bool>* cancel) {
    if (cancel && cancel->load(std::memory_order_relaxed)) throw Cancelled{};
}

}  // namespace

bool is_cancel(const std::exception& e) {
    if (dynamic_cast<const Cancelled*>(&e)) return true;
#ifdef SPECTRA_HAVE_GPU
    if (dynamic_cast<const gpu::Cancelled*>(&e)) return true;
#endif
    return false;
}

// ---- calibration (de)serialisation ----------------------------------------------

json Calibration::to_json() const {
    return json{{"version", version},
                {"device", device},
                {"driver", driver},
                {"vendor_id", vendor_id},
                {"device_id", device_id},
                {"software", software},
                {"preview_long_edge", preview_long_edge},
                {"draft_long_edge", draft_long_edge},
                {"batch_max_dispatches", batch_max_dispatches},
                {"batch_max_work_pixels", batch_max_work_pixels},
                {"memory_budget_bytes", memory_budget_bytes},
                {"tile_size_60mp", tile_size_60mp},
                {"ms_per_megapixel", ms_per_megapixel},
                {"fixed_ms", fixed_ms},
                {"warmup_ms", warmup_ms},
                {"log", log}};
}

Calibration Calibration::from_json(const json& j) {
    Calibration c;
    c.version = j.value("version", 0);
    c.device = j.value("device", "");
    c.driver = j.value("driver", "");
    c.vendor_id = j.value("vendor_id", 0u);
    c.device_id = j.value("device_id", 0u);
    c.software = j.value("software", false);
    c.preview_long_edge = std::clamp(j.value("preview_long_edge", 1600), 256, 4096);
    c.draft_long_edge = std::clamp(j.value("draft_long_edge", 512), 128, 2048);
    c.batch_max_dispatches = std::clamp<uint32_t>(j.value("batch_max_dispatches", 48u), 1, 256);
    c.batch_max_work_pixels = std::max<uint64_t>(j.value("batch_max_work_pixels", uint64_t(48) << 20), 1 << 20);
    c.memory_budget_bytes = j.value("memory_budget_bytes", uint64_t(0));
    c.tile_size_60mp = j.value("tile_size_60mp", 0);
    c.ms_per_megapixel = j.value("ms_per_megapixel", 0.0);
    c.fixed_ms = j.value("fixed_ms", 0.0);
    c.warmup_ms = j.value("warmup_ms", 0.0);
    if (j.contains("log")) c.log = j.at("log").get<std::vector<std::string>>();
    return c;
}

// ---- engine -----------------------------------------------------------------------

struct Engine::Impl {
    struct LevelState {
        Image src;            // prepared source: crop (+ apron), reduced
        ViewRect view;        // requested rect (image pixels), empty = whole image
        ViewRect padded;      // rect actually rendered
        int edge = 0;
        uint64_t image_id = 0;
        double scale = 1.0;   // src pixels per image pixel
#ifdef SPECTRA_HAVE_GPU
        gpu::StageCache cache;
#endif
    };
    LevelState level[2];
    uint64_t image_id = 0;
    Image meter;
    std::string note, boost_key;
    double boost_max = 0.0;
#ifdef SPECTRA_HAVE_GPU
    std::shared_ptr<gpu::Context> ctx;
    // The pipeline object (baked tables, GPU table buffers) of the last
    // parameter set: the draft and the refined pass of one change share it.
    std::unique_ptr<gpu::GpuPipeline> gp;
#endif
    std::unique_ptr<Pipeline> cpu;
    std::string pipeline_key;
    void drop_caches() {
#ifdef SPECTRA_HAVE_GPU
        gp.reset();
#endif
        cpu.reset();
        pipeline_key.clear();
#ifdef SPECTRA_HAVE_GPU
        if (ctx)
            for (auto& l : level) l.cache.clear(*ctx);
#endif
        for (auto& l : level) {
            l.src = Image{};
            l.image_id = 0;
        }
    }
};

Engine::Engine(std::shared_ptr<const Pack> pack, std::shared_ptr<const ColourData> colour, const EngineConfig& config)
    : impl_(std::make_unique<Impl>()), pack_(std::move(pack)), cd_(std::move(colour)), cfg_(config) {
#ifdef SPECTRA_HAVE_GPU
    if (!cfg_.force_cpu) {
        try {
            gpu::ContextOptions co;
            co.device = cfg_.gpu_device;
            impl_->ctx = std::make_shared<gpu::Context>(co);
        } catch (const std::exception& e) {
            impl_->note = std::string("No usable Vulkan device (") + e.what() + "); rendering on the CPU.";
        }
    } else {
        impl_->note = "GPU rendering is switched off; rendering on the CPU.";
    }
#else
    impl_->note = "This build has no GPU backend; rendering on the CPU.";
#endif
    if (!has_gpu()) {
        calib_.preview_long_edge = 640;
        calib_.draft_long_edge = 256;
    }
    if (cfg_.calibration) set_calibration(*cfg_.calibration);
}

Engine::~Engine() { impl_->drop_caches(); }

bool Engine::has_gpu() const {
#ifdef SPECTRA_HAVE_GPU
    return impl_->ctx != nullptr;
#else
    return false;
#endif
}

std::string Engine::device_name() const {
#ifdef SPECTRA_HAVE_GPU
    if (impl_->ctx) return impl_->ctx->info().name;
#endif
    return "CPU (reference)";
}

std::string Engine::device_id() const {
#ifdef SPECTRA_HAVE_GPU
    if (impl_->ctx) return impl_->ctx->info().name + " | " + impl_->ctx->info().driver;
#endif
    return "cpu";
}

std::string Engine::backend_note() const { return impl_->note; }

uint64_t Engine::gpu_allocated_bytes() const {
#ifdef SPECTRA_HAVE_GPU
    if (impl_->ctx) return impl_->ctx->allocated_bytes();
#endif
    return 0;
}

uint64_t Engine::gpu_budget_bytes() const {
#ifdef SPECTRA_HAVE_GPU
    if (impl_->ctx) {
        if (cfg_.memory_budget_bytes) return cfg_.memory_budget_bytes;
        if (calib_.memory_budget_bytes) return calib_.memory_budget_bytes;
        return std::min<uint64_t>(4ull << 30, impl_->ctx->query_budget_bytes() * 6 / 10);
    }
#endif
    return 0;
}

void Engine::set_overrides(bool match_python, uint64_t memory_budget_bytes, int tile_size) {
    if (cfg_.match_python != match_python) {
        impl_->drop_caches();
        if (image_) impl_->meter = match_python ? metering_preview(*image_) : metering_preview(downsample_area(*image_, 1024));
    }
    cfg_.match_python = match_python;
    cfg_.memory_budget_bytes = memory_budget_bytes;
    cfg_.tile_size = tile_size;
}

void Engine::set_calibration(const Calibration& c) {
    calib_ = c;
#ifdef SPECTRA_HAVE_GPU
    if (impl_->ctx) {
        gpu::BatchOptions b;
        b.max_dispatches = c.batch_max_dispatches;
        b.max_work_pixels = c.batch_max_work_pixels;
        impl_->ctx->set_batching(b);
    }
#endif
    impl_->drop_caches();
}

void Engine::set_image(std::shared_ptr<const Image> image, const std::string& input_color_space, bool proxy) {
    impl_->drop_caches();
    if (image && image->c != 3) {
        // Keep the first three channels (alpha is dropped, as the reference does).
        if (image->c < 3) throw std::runtime_error("the image must have at least 3 channels");
        auto rgb = std::make_shared<Image>(image->h, image->w, 3);
        for (size_t i = 0; i < image->pixels(); ++i)
            for (int k = 0; k < 3; ++k) rgb->v[i * 3 + k] = image->v[i * image->c + k];
        image = rgb;
    }
    image_ = std::move(image);
    input_space_ = input_color_space;
    proxy_ = proxy;
    ++impl_->image_id;
    impl_->boost_key.clear();
    // The reference meters on a 256 px preview it reduces from the full frame
    // with a Gaussian anti-aliasing filter, which takes seconds at 20 MP. The
    // app reduces by area to 1024 px first (DEVIATIONS.md D-12); "Match Python
    // exactly" keeps the reference's path.
    impl_->meter = Image{};
    if (image_) impl_->meter = cfg_.match_python ? metering_preview(*image_) : metering_preview(downsample_area(*image_, 1024));
}

void Engine::set_viewport(int long_edge_px) { viewport_ = std::clamp(long_edge_px, 256, 8192); }

int Engine::preview_long_edge(Level level) const {
    int preview = std::min(calib_.preview_long_edge, (viewport_ + 63) / 64 * 64);
    if (level == Level::Preview) return preview;
    return std::min(calib_.draft_long_edge, std::max(128, preview / 2));
}

Params Engine::digest(const Params& user) const {
    Params p = user;
    if (!input_space_.empty()) {
        p.io.input_color_space = input_space_;
        p.io.input_cctf_decoding = false;
    }
    return digest_params(*pack_, p, false);
}

namespace {

// Prepares (and caches) the source pixels of a preview level.
template <class LevelState>
LevelState& prepare_level(LevelState& ls, const Image& image, uint64_t image_id, const ViewRect& view_in, int edge,
                          bool& changed) {
    ViewRect view = view_in;
    if (!view.empty()) {
        view.x = std::clamp(view.x, 0, image.w - 1);
        view.y = std::clamp(view.y, 0, image.h - 1);
        view.w = std::clamp(view.w, 1, image.w - view.x);
        view.h = std::clamp(view.h, 1, image.h - view.y);
        if (view.x == 0 && view.y == 0 && view.w == image.w && view.h == image.h) view = ViewRect{};
    }
    changed = !(ls.image_id == image_id && ls.view == view && ls.edge == edge && !ls.src.v.empty());
    if (!changed) return ls;
    ls.image_id = image_id;
    ls.view = view;
    ls.edge = edge;
    if (view.empty()) {
        ls.padded = ViewRect{0, 0, image.w, image.h};
        ls.src = downsample_area(image, edge);
    } else {
        double s = std::min(1.0, static_cast<double>(edge) / std::max(view.w, view.h));
        int a = static_cast<int>(std::ceil(kViewApron / s));
        ViewRect p;
        p.x = std::max(0, view.x - a);
        p.y = std::max(0, view.y - a);
        p.w = std::min(image.w, view.x + view.w + a) - p.x;
        p.h = std::min(image.h, view.y + view.h + a) - p.y;
        ls.padded = p;
        Image c = crop_rect(image, p);
        int target = static_cast<int>(std::lround(std::max(p.w, p.h) * s));
        ls.src = s < 1.0 ? downsample_area(c, std::max(target, 16)) : std::move(c);
    }
    ls.scale = static_cast<double>(ls.src.w) / ls.padded.w;
    return ls;
}

// Cuts the requested view out of a render of the padded source.
template <class LevelState>
Image cut_view(const LevelState& ls, const Image& rendered) {
    if (ls.view.empty()) return rendered;
    int x0 = static_cast<int>(std::lround((ls.view.x - ls.padded.x) * ls.scale));
    int y0 = static_cast<int>(std::lround((ls.view.y - ls.padded.y) * ls.scale));
    int w = std::max(1, static_cast<int>(std::lround(ls.view.w * ls.scale)));
    int h = std::max(1, static_cast<int>(std::lround(ls.view.h * ls.scale)));
    x0 = std::clamp(x0, 0, rendered.w - 1);
    y0 = std::clamp(y0, 0, rendered.h - 1);
    w = std::min(w, rendered.w - x0);
    h = std::min(h, rendered.h - y0);
    Image out(h, w, rendered.c);
    for (int y = 0; y < h; ++y)
        std::copy(rendered.px(y + y0, x0), rendered.px(y + y0, x0) + static_cast<size_t>(w) * rendered.c, out.px(y, 0));
    return out;
}

}  // namespace

RenderResult Engine::render_preview(const Params& user, Level level, const ViewRect& view,
                                    const std::atomic<bool>* cancel) {
    if (!image_) throw std::runtime_error("no image loaded");
    auto t0 = std::chrono::steady_clock::now();
    check_cancel(cancel);
    Params p = user;
    // The preview is for an sRGB display whatever the export settings are.
    p.io.output_color_space = "sRGB";
    p.io.output_cctf_encoding = true;
    p.extended.hdr_output.active = false;
    p.io.crop = false;
    p.io.upscale_factor = 1.0;
    Params d = digest(p);

    auto& ls = impl_->level[level == Level::Draft ? 0 : 1];
    bool changed = false;
    // A partial view is shown at up to 1:1: its refined pass renders the view's
    // own pixels, the draft pass the calibrated preview size.
    int edge = preview_long_edge(level);
    if (!view.empty()) edge = level == Level::Preview ? std::max(view.w, view.h) : preview_long_edge(Level::Preview);
    prepare_level(ls, *image_, impl_->image_id, view, edge, changed);
    check_cancel(cancel);

    RenderOptions ro;
    ro.match_python = cfg_.match_python;
    ro.seed = cfg_.seed;
    ro.full_long_edge_px = std::max(image_->w, image_->h) * ls.scale;
    ro.origin_x = static_cast<uint32_t>(std::lround(ls.padded.x * ls.scale));
    ro.origin_y = static_cast<uint32_t>(std::lround(ls.padded.y * ls.scale));

    RenderResult r;
    r.rect = ls.view;
#ifdef SPECTRA_HAVE_GPU
    if (impl_->ctx) {
        gpu::Context& ctx = *impl_->ctx;
        if (changed) ls.cache.clear(ctx);
        gpu::GpuOptions go;
        go.memory_budget_bytes = gpu_budget_bytes();
        std::string key = params_to_json(d).dump() + (cfg_.match_python ? "|py|" : "|") + std::to_string(go.memory_budget_bytes);
        if (!impl_->gp || impl_->pipeline_key != key) {
            impl_->gp.reset();
            impl_->gp = std::make_unique<gpu::GpuPipeline>(impl_->ctx, cd_, d, ro, go);
            impl_->pipeline_key = key;
        }
        gpu::GpuPipeline& gp = *impl_->gp;
        ro.autoexposure_ev = gp.cpu().meter_autoexposure_ev(impl_->meter);
        gp.set_options(ro);
        if (d.film_render.halation.boost_ev != 0) {
            // Whole-image maximum, measured once per exposure setting on the loaded image.
            json k = {d.film, d.camera.color_filter, d.io.input_color_space, d.camera.exposure_compensation_ev,
                      *ro.autoexposure_ev, d.settings.rgb_to_raw_method, impl_->image_id};
            if (impl_->boost_key != k.dump()) {
                impl_->boost_max = gp.measure_boost_max_raw(*image_);
                impl_->boost_key = k.dump();
            }
            ro.boost_max_raw = impl_->boost_max;
            gp.set_options(ro);
        }
        gpu::GpuRenderInfo info;
        ctx.set_cancel_flag(cancel);
        Image out;
        try {
            out = gp.process(ls.src, Tap::RgbOut, &info, &ls.cache);
        } catch (...) {
            ctx.set_cancel_flag(nullptr);
            throw;
        }
        ctx.set_cancel_flag(nullptr);
        r.image = cut_view(ls, out);
        r.gpu = true;
        r.first_stage = info.first_stage_run;
        r.peak_gpu_bytes = info.peak_gpu_bytes;
        r.stage_ms = info.stage_ms;
        for (auto& f : info.cpu_fallbacks) r.notes.push_back("ran on the CPU: " + f);
        r.ms = ms_since(t0);
        return r;
    }
#endif
    std::string key = params_to_json(d).dump() + (cfg_.match_python ? "|py" : "|");
    if (!impl_->cpu || impl_->pipeline_key != key) {
        impl_->cpu = std::make_unique<Pipeline>(cd_, d, ro);
        impl_->pipeline_key = key;
    }
    Pipeline& pipe = *impl_->cpu;
    ro.autoexposure_ev = pipe.meter_autoexposure_ev(impl_->meter);
    pipe.set_options(ro);
    check_cancel(cancel);
    Image out = pipe.process(ls.src);
    check_cancel(cancel);
    r.image = cut_view(ls, out);
    r.ms = ms_since(t0);
    return r;
}

Image Engine::render_input(Level level, const ViewRect& view) {
    if (!image_) throw std::runtime_error("no image loaded");
    Impl::LevelState tmp;
    bool changed = false;
    auto& ls = impl_->level[level == Level::Draft ? 0 : 1];
    const Impl::LevelState* use = &ls;
    if (!(ls.image_id == impl_->image_id && ls.view == view && !ls.src.v.empty())) {
        prepare_level(tmp, *image_, impl_->image_id, view, preview_long_edge(level), changed);
        use = &tmp;
    }
    Image src = cut_view(*use, use->src);
    Params def;
    const std::string space = input_space_.empty() ? def.io.input_color_space : input_space_;
    Mat3 m = cd_->rgb_to_rgb(cd_->space(space), cd_->space("sRGB"));
    Image out(src.h, src.w, 3);
    parallel_for(static_cast<long>(src.pixels()), [&](long a, long b) {
        for (long i = a; i < b; ++i) {
            const double* s = &src.v[static_cast<size_t>(i) * 3];
            for (int k = 0; k < 3; ++k) {
                double v = m[k * 3] * s[0] + m[k * 3 + 1] * s[1] + m[k * 3 + 2] * s[2];
                out.v[static_cast<size_t>(i) * 3 + k] = cctf_encode("srgb", std::clamp(v, 0.0, 1.0));
            }
        }
    }, 1 << 14);
    return out;
}

std::string Engine::full_render_notice(const Params& user) {
    if (!image_) return {};
#ifdef SPECTRA_HAVE_GPU
    if (impl_->ctx) {
        Params d = digest(user);
        gpu::GpuOptions go;
        go.memory_budget_bytes = gpu_budget_bytes();
        go.tile_size = cfg_.tile_size;
        gpu::GpuPipeline gp(impl_->ctx, cd_, d, RenderOptions{}, go);
        if (!gp.would_tile(image_->w, image_->h)) return {};
        if (d.camera.diffusion_filter.active || d.enlarger.diffusion_filter.active)
            return "This image is too large for the GPU memory budget and will be rendered in tiles. The diffusion "
                   "filter is not available on tiled renders in this version: switch it off to export, or raise "
                   "the GPU memory budget in Settings.";
        return "This image exceeds the GPU memory budget and will be rendered in tiles.";
    }
#endif
    return {};
}

RenderResult Engine::render_full(const Params& user, const std::function<void(int, int)>& progress,
                                 const std::atomic<bool>* cancel) {
    if (!image_) throw std::runtime_error("no image loaded");
    if (proxy_) throw std::runtime_error("the full-resolution image is still loading");
    auto t0 = std::chrono::steady_clock::now();
    check_cancel(cancel);
    Params d = digest(user);
    RenderOptions ro;
    ro.match_python = cfg_.match_python;
    ro.seed = cfg_.seed;
    RenderResult r;
#ifdef SPECTRA_HAVE_GPU
    if (impl_->ctx) {
        gpu::Context& ctx = *impl_->ctx;
        // Hand the preview's GPU memory to the full render.
        impl_->drop_caches();
        ctx.trim_pool();
        gpu::GpuOptions go;
        go.memory_budget_bytes = gpu_budget_bytes();
        go.tile_size = cfg_.tile_size;
        gpu::GpuPipeline gp(impl_->ctx, cd_, d, ro, go);
        ro.autoexposure_ev = gp.cpu().meter_autoexposure_ev(impl_->meter);
        gp.set_options(ro);
        gp.on_progress = progress;
        gpu::GpuRenderInfo info;
        ctx.set_cancel_flag(cancel);
        try {
            r.image = gp.process(*image_, Tap::RgbOut, &info);
        } catch (...) {
            ctx.set_cancel_flag(nullptr);
            ctx.trim_pool();
            throw;
        }
        ctx.set_cancel_flag(nullptr);
        ctx.trim_pool();
        r.gpu = true;
        r.tiles = info.tiles;
        r.peak_gpu_bytes = info.peak_gpu_bytes;
        r.stage_ms = info.stage_ms;
        for (auto& f : info.cpu_fallbacks) r.notes.push_back("ran on the CPU: " + f);
        if (info.tiled)
            r.notes.push_back("rendered in " + std::to_string(info.tiles) + " tiles of " +
                              std::to_string(info.tile_size) + " px");
        r.ms = ms_since(t0);
        return r;
    }
#endif
    Pipeline pipe(cd_, d, ro);
    ro.autoexposure_ev = pipe.meter_autoexposure_ev(impl_->meter);
    pipe.set_options(ro);
    if (progress) progress(0, 1);
    r.image = pipe.process(*image_);
    check_cancel(cancel);
    if (progress) progress(1, 1);
    r.ms = ms_since(t0);
    return r;
}

// ---- calibration --------------------------------------------------------------------

Calibration Engine::calibrate(const std::function<void(const std::string&)>& progress, const std::atomic<bool>* cancel) {
    Calibration c;
    auto say = [&](const std::string& s) {
        c.log.push_back(s);
        if (progress) progress(s);
    };
#ifdef SPECTRA_HAVE_GPU
    if (impl_->ctx) {
        gpu::Context& ctx = *impl_->ctx;
        const gpu::DeviceInfo& di = ctx.info();
        c.device = di.name;
        c.driver = di.driver;
        c.vendor_id = di.vendor_id;
        c.device_id = di.device_id;
        c.software = di.is_software();
        impl_->drop_caches();
        ctx.trim_pool();
        char buf[256];

        auto t0 = std::chrono::steady_clock::now();
        ctx.warm_up();
        c.warmup_ms = ms_since(t0);
        std::snprintf(buf, sizeof buf, "%s: kernels compiled in %.0f ms", di.name.c_str(), c.warmup_ms);
        say(buf);

        Params d = digest_params(*pack_, Params{}, true);
        RenderOptions ro;
        ro.autoexposure_ev = 0.0;
        int base = c.software ? 384 : 1024;
        Image big = calibration_image(base, base * 2 / 3), small = calibration_image(base / 2, base / 3);
        double mp_big = big.pixels() / 1e6, mp_small = small.pixels() / 1e6;
        uint64_t dispatches = 0;
        auto run = [&](const Image& img, const gpu::BatchOptions& b, int reps) {
            ctx.set_batching(b);
            double best = 1e30;
            for (int i = 0; i < reps; ++i) {
                check_cancel(cancel);
                gpu::GpuPipeline gp(impl_->ctx, cd_, d, ro);
                gpu::GpuRenderInfo info;
                auto t = std::chrono::steady_clock::now();
                gp.process(img, Tap::RgbOut, nullptr);
                best = std::min(best, ms_since(t));
                (void)info;
                dispatches = ctx.dispatch_count();
            }
            return best;
        };
        // Warm caches, then try the batching candidates.
        gpu::BatchOptions one;
        one.max_dispatches = 1;
        run(small, one, 1);
        std::vector<gpu::BatchOptions> candidates;
        candidates.push_back(one);
        if (!c.software) candidates.push_back({8, 16ull << 20});
        candidates.push_back({48, 48ull << 20});
        if (!c.software) candidates.push_back({256, 256ull << 20});
        int reps = c.software ? 1 : 3;
        gpu::BatchOptions best_b = candidates.back();
        double best_ms = 1e30;
        for (auto& b : candidates) {
            double t = run(big, b, reps);
            std::snprintf(buf, sizeof buf, "batch of up to %u dispatches: %.1f ms for %.2f MP", b.max_dispatches, t, mp_big);
            say(buf);
            if (t < best_ms) {
                best_ms = t;
                best_b = b;
            }
        }
        uint64_t d0 = ctx.dispatch_count();
        double t_big = run(big, best_b, reps);
        uint64_t per_render = (ctx.dispatch_count() - d0) / static_cast<uint64_t>(reps);
        double t_small = run(small, best_b, reps);
        c.ms_per_megapixel = std::max((t_big - t_small) / (mp_big - mp_small), 1e-3);
        c.fixed_ms = std::max(t_big - c.ms_per_megapixel * mp_big, 0.0);
        std::snprintf(buf, sizeof buf, "complete pipeline: %.1f ms fixed + %.1f ms per megapixel (%llu dispatches)",
                      c.fixed_ms, c.ms_per_megapixel, static_cast<unsigned long long>(per_render));
        say(buf);

        // A submission should take about 100 ms: short enough to cancel quickly
        // and far below the operating system's GPU watchdog.
        double ms_per_work_mp = t_big / (std::max<uint64_t>(per_render, 1) * mp_big);
        double cap_mp = std::clamp(100.0 / std::max(ms_per_work_mp, 1e-6), 4.0, 1024.0);
        c.batch_max_dispatches = best_b.max_dispatches;
        c.batch_max_work_pixels = best_b.max_dispatches == 1 ? best_b.max_work_pixels
                                                             : static_cast<uint64_t>(cap_mp * 1048576.0);
        auto predicted = [&](int edge) { return c.fixed_ms + c.ms_per_megapixel * (edge * (edge * 2.0 / 3.0)) / 1e6; };
        // Refined preview: a complete render within about 150 ms, which puts
        // the usual slider change (later stages only) well under 50 ms.
        c.preview_long_edge = 768;
        for (int e : {1024, 1280, 1600, 1920, 2560})
            if (predicted(e) <= 150.0) c.preview_long_edge = e;
        c.draft_long_edge = 256;
        for (int e : {384, 512, 640, 800})
            if (predicted(e) <= 40.0 && e <= c.preview_long_edge / 2) c.draft_long_edge = e;
        c.memory_budget_bytes = std::min<uint64_t>(4ull << 30, ctx.query_budget_bytes() * 6 / 10);
        {
            gpu::GpuOptions go;
            go.memory_budget_bytes = c.memory_budget_bytes;
            gpu::GpuPipeline gp(impl_->ctx, cd_, d, ro, go);
            if (gp.would_tile(9488, 6320)) {
                double bytes_per_px = static_cast<double>(gp.estimate_bytes(1024, 1024)) / (1024.0 * 1024.0);
                int apron = gp.apron_px(d.camera.film_format_mm * 1000.0 / 9488.0);
                c.tile_size_60mp = std::max(0, static_cast<int>(std::sqrt(c.memory_budget_bytes / bytes_per_px)) - 2 * apron);
            }
        }
        std::snprintf(buf, sizeof buf,
                      "chosen: preview %d px (%.0f ms), draft %d px (%.0f ms), batches of %u dispatches / %.0f MP, "
                      "GPU budget %.1f GiB%s",
                      c.preview_long_edge, predicted(c.preview_long_edge), c.draft_long_edge,
                      predicted(c.draft_long_edge), c.batch_max_dispatches, c.batch_max_work_pixels / 1048576.0,
                      c.memory_budget_bytes / 1073741824.0,
                      c.tile_size_60mp ? (", 60 MP in tiles of " + std::to_string(c.tile_size_60mp) + " px").c_str()
                                       : ", 60 MP untiled");
        say(buf);
        ctx.trim_pool();
        set_calibration(c);
        return c;
    }
#endif
    (void)cancel;
    c.device = "cpu";
    c.software = true;
    c.preview_long_edge = 640;
    c.draft_long_edge = 256;
    say("no GPU: CPU reference rendering with a 640 px preview");
    set_calibration(c);
    return c;
}

Image resize_long_edge(const Image& img, int long_edge) {
    if (long_edge <= 0 || long_edge >= std::max(img.w, img.h)) return img;
    return downsample_area(img, long_edge);
}

}  // namespace spectra::engine
