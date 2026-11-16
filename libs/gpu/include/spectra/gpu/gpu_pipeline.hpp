// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "spectra/gpu/context.hpp"
#include "spectra/pipeline/pipeline.hpp"

namespace spectra::gpu {

struct GpuOptions {
    // GPU working-set budget in bytes. 0 = automatic: 60 % of the device's
    // memory budget, capped at 4 GiB. Images whose working set exceeds it are
    // rendered in tiles.
    uint64_t memory_budget_bytes = 0;
    // Manual override: > 0 forces tiling with this core tile size (pixels).
    int tile_size = 0;
};

// A float image on the GPU: w * h * c values, row-major, channels interleaved.
struct Plane {
    Buffer b;
    int w = 0, h = 0, c = 0;
    size_t floats() const { return static_cast<size_t>(w) * h * c; }
};

// Stage outputs kept on the GPU between renders of one image at one size, so
// that changing a parameter re-runs only the stages downstream of it:
//   0 film exposure -> 1 film development -> 2 grain -> 3 print -> 4 scan/output.
// The cache belongs to the caller and outlives the GpuPipeline objects (one is
// built per parameter set). Call clear() when the input image or view changes
// and before the Context is destroyed.
struct StageCache {
    static constexpr int kStages = 5;
    std::array<std::string, kStages> key;
    std::array<Plane, kStages - 1> plane;
    Image result;
    RenderStats stats;
    double pixel_size_um = 0.0;
    int first_stage_run = 0;  // of the last render: 0 = everything, 5 = nothing (cache hit)
    void clear(Context& ctx);
    uint64_t bytes() const;
};

struct GpuRenderInfo {
    double pixel_size_um = 0.0;
    RenderStats stats;
    bool tiled = false;
    int tiles = 0, tile_size = 0, apron = 0;
    uint64_t peak_gpu_bytes = 0, budget_bytes = 0, estimated_bytes = 0;
    std::map<std::string, double> stage_ms;     // wall time per pipeline stage
    std::vector<std::string> cpu_fallbacks;     // stages that ran on the CPU
    double total_ms = 0.0;
    int first_stage_run = 0;                    // with a StageCache: first stage that was re-rendered
    uint64_t submits = 0, dispatches = 0;       // queue submissions / kernel dispatches of this render
};

// Vulkan compute implementation of the pipeline. All parameter-dependent
// tables come from the CPU reference Pipeline, so both backends share one
// source of truth; only the per-pixel and spatial work runs on the GPU.
class GpuPipeline {
public:
    GpuPipeline(std::shared_ptr<Context> ctx, std::shared_ptr<const ColourData> colour, const Params& digested,
                const RenderOptions& options = {}, const GpuOptions& gpu_options = {});
    ~GpuPipeline();

    // Renders from rgb_in to `collect`. With a StageCache (collect must be
    // RgbOut) the image is rendered untiled and only the stages whose inputs
    // changed since the cache was filled are run.
    Image process(const Image& image, Tap collect = Tap::RgbOut, GpuRenderInfo* info = nullptr,
                  StageCache* cache = nullptr);
    // Called after every tile of a tiled render with (tiles done, tiles total).
    std::function<void(int, int)> on_progress;

    // Cache keys of the five stages for these (digested) parameters: key[k]
    // changes exactly when the output of stage k may change.
    static std::array<std::string, StageCache::kStages> stage_keys(const Params& digested, const RenderOptions& o);
    // True when a render of w x h would be tiled with the current budget.
    bool would_tile(int w, int h) const;

    const Pipeline& cpu() const { return *cpu_; }
    void set_options(const RenderOptions& o) {
        opt_ = o;
        cpu_->set_options(o);
    }
    // Whole-image maximum of the film raw exposure (the highlight boost needs
    // it). Measure it on the full image and pass it to previews, crops and tiles
    // through RenderOptions::boost_max_raw. Needs options().autoexposure_ev.
    double measure_boost_max_raw(const Image& rgb);
    // Border (pixels) a crop or tile needs around it so that every active
    // spatial effect sees its full support.
    int apron_px(double pixel_size_um) const;

    // Test hooks for the stochastic kernels (used by the parity runner).
    // Draws w*h samples: Poisson(a) or Binomial(n = a, p = b).
    static std::vector<float> test_sampler(Context& ctx, bool poisson, double a, double b, bool fast, int w, int h,
                                           uint64_t seed);
    // One emulsion sub-layer at a flat density (sample_grain_layer on the CPU).
    static std::vector<float> test_grain_layer(Context& ctx, double density, double density_max, double n_particles,
                                               double uniformity, bool fast, int size, uint64_t seed, uint32_t stream);
    // Estimated GPU working set for an image of this size, in bytes.
    uint64_t estimate_bytes(int w, int h) const;

private:
    struct Region {
        RenderOptions opt;       // per-region options (statistics, origin)
        double pixel_size_um = 0;
        double scale = 1.0;      // pixel-unit parameter scale
    };
    Plane make(int w, int h, int c);
    void free(Plane& p);
    Plane upload(const Image& img);
    Image download(const Plane& p);
    const Buffer* params(const std::vector<float>& v);
    const Buffer* uparams(const std::vector<uint32_t>& v);
    void run(const std::string& kernel, const std::vector<const Buffer*>& b, const PushConstants& pc, int w, int h);
    void pointwise(int mode, const Plane& x, const Plane* y, Plane& out, const std::vector<float>& f = {},
                   const std::vector<float>& p = {});
    void combine(const Plane& x, const Plane* y, Plane& out, const Vec& a, const Vec& b, const Vec& add, const Vec& div);
    // dst (+)= coef[c] * Gaussian(src, sigma[c]).
    void gaussian(const Plane& src, Plane& dst, const Vec& sigma, const Vec& coef, bool accumulate);
    void exponential(const Plane& src, Plane& dst, const Vec& decay, const Vec& coef, bool accumulate);
    Vec reduce(const Plane& p, bool sum);
    Plane cpu_stage(const Plane& p, const std::string& what, const std::function<Image(const Image&)>& fn);

    Image render_region(const Image& rgb, Tap collect, const Region& region, GpuRenderInfo* info,
                        StageCache* cache = nullptr);
    Image render_region_impl(const Image& rgb, Tap collect, const Region& region, GpuRenderInfo* info,
                             StageCache* cache);
    Plane copy(const Plane& p);
    void detach(const Plane& p);   // the plane is now owned by a StageCache
    void release_live();           // after an error: give back every plane still held
    uint64_t budget() const;
    std::map<VkBuffer, Buffer> live_;
    Plane stage_expose(const Image& rgb, const Region& r, GpuRenderInfo* info, double ae_ev);
    Plane stage_develop(Plane& log_raw, const Region& r);
    Plane stage_grain(Plane& cmy, const Region& r);
    Plane stage_print(Plane& cmy, const Region& r);
    Plane stage_scan(Plane& density, const Region& r);

    std::shared_ptr<Context> ctx_;
    std::shared_ptr<const ColourData> cd_;
    std::unique_ptr<Pipeline> cpu_;
    Params p_;
    RenderOptions opt_;
    GpuOptions gopt_;
    Pipeline::StaticCorrections corr_;
    std::vector<Buffer> temp_;      // parameter buffers of the current render
    std::map<std::string, Buffer> tables_;  // long-lived tables
    const Buffer* table(const std::string& name, const std::vector<float>& v);
    std::vector<std::string>* fallbacks_ = nullptr;
};

}  // namespace spectra::gpu
