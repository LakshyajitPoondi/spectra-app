// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "spectra/pipeline/pipeline.hpp"

// Rendering engine shared by the app and the command-line tool: owns the GPU
// context (or falls back to the CPU reference), the loaded image, its preview
// sizes, whole-image statistics and the per-stage caches. It has no UI
// dependency. One thread at a time may call into an Engine.
namespace spectra::engine {

// Result of the first-launch performance calibration for one GPU.
struct Calibration {
    static constexpr int kVersion = 2;
    int version = kVersion;
    std::string device, driver;
    uint32_t vendor_id = 0, device_id = 0;
    bool software = false;
    int preview_long_edge = 1600;   // refined preview
    int draft_long_edge = 512;      // first, low-resolution pass while a slider moves
    uint32_t batch_max_dispatches = 48;
    uint64_t batch_max_work_pixels = 48ull << 20;
    uint64_t memory_budget_bytes = 0;  // GPU working-set budget used for tiling decisions
    int tile_size_60mp = 0;            // informative: core tile size a 60 MP render would use (0 = untiled)
    double ms_per_megapixel = 0.0, fixed_ms = 0.0;  // complete pipeline: t = fixed + ms_per_mp * MP
    double warmup_ms = 0.0;            // kernel compilation
    std::vector<std::string> log;      // what was measured, for the report
    nlohmann::json to_json() const;
    static Calibration from_json(const nlohmann::json& j);
};

struct EngineConfig {
    std::string gpu_device;        // substring or index; empty = automatic
    bool force_cpu = false;
    bool match_python = false;     // "Match Python exactly": resolution-dependent like the reference
    uint64_t seed = 0;
    uint64_t memory_budget_bytes = 0;  // manual override of the GPU budget (0 = automatic)
    int tile_size = 0;                 // manual override: force tiles of this size (0 = automatic)
    std::optional<Calibration> calibration;
};

enum class Level { Draft, Preview };

// Part of the image to preview, in pixels of the loaded image. Empty = all.
struct ViewRect {
    int x = 0, y = 0, w = 0, h = 0;
    bool empty() const { return w <= 0 || h <= 0; }
    bool operator==(const ViewRect& o) const { return x == o.x && y == o.y && w == o.w && h == o.h; }
};

struct RenderResult {
    Image image;
    ViewRect rect;             // region of the loaded image the result shows (whole image when empty)
    double ms = 0.0;
    bool gpu = false;
    int first_stage = 0;       // 0 = everything was rendered ... 5 = cache hit
    int tiles = 1;
    uint64_t peak_gpu_bytes = 0;
    std::map<std::string, double> stage_ms;
    std::vector<std::string> notes;  // stages that ran on the CPU, approximations
};

struct ExportOptions {
    int bits = 16;             // 8 / 16 integer; 32 with is_float
    bool is_float = false;
    int jpeg_quality = 95;
    int long_edge = 0;         // resize so the long edge has this many pixels (0 = full size)
};

class Engine {
public:
    Engine(std::shared_ptr<const Pack> pack, std::shared_ptr<const ColourData> colour, const EngineConfig& config);
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    bool has_gpu() const;
    std::string device_name() const;       // GPU name, or "CPU (reference)"
    std::string device_id() const;         // name | driver, identifies a calibration
    std::string backend_note() const;      // why the CPU is used, when it is
    uint64_t gpu_allocated_bytes() const;
    uint64_t gpu_budget_bytes() const;
    const EngineConfig& config() const { return cfg_; }
    const ColourData& colour() const { return *cd_; }
    const Pack& pack() const { return *pack_; }
    void set_overrides(bool match_python, uint64_t memory_budget_bytes, int tile_size);

    // Performance calibration. calibrate() measures this GPU (a few seconds)
    // and applies the result; set_calibration() applies a cached one.
    const Calibration& calibration() const { return calib_; }
    void set_calibration(const Calibration& c);
    Calibration calibrate(const std::function<void(const std::string&)>& progress = {},
                          const std::atomic<bool>* cancel = nullptr);

    // The image to work on, linear RGB in `input_color_space`. `proxy` marks a
    // reduced decode (shown until the full-resolution one arrives).
    void set_image(std::shared_ptr<const Image> image, const std::string& input_color_space, bool proxy);
    bool has_image() const { return image_ != nullptr; }
    bool is_proxy() const { return proxy_; }
    int width() const { return image_ ? image_->w : 0; }
    int height() const { return image_ ? image_->h : 0; }
    // Long edge (device pixels) of the widget the preview is shown in.
    void set_viewport(int long_edge_px);

    // Preview for display: always sRGB with its transfer function, whatever the
    // output settings are. Throws gpu::Cancelled (or engine::Cancelled on the
    // CPU) when `cancel` is raised.
    RenderResult render_preview(const Params& user, Level level, const ViewRect& view,
                                const std::atomic<bool>* cancel = nullptr);
    // Full-resolution render with the user's output settings.
    RenderResult render_full(const Params& user, const std::function<void(int, int)>& progress = {},
                             const std::atomic<bool>* cancel = nullptr);
    // Unprocessed view of the input (linear -> sRGB) for before / after.
    Image render_input(Level level, const ViewRect& view);

    // Whether a full render of the loaded image will be tiled, and what that
    // makes unavailable. Empty string: nothing to report.
    std::string full_render_notice(const Params& user);
    Params digest(const Params& user) const;
    int preview_long_edge(Level level) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::shared_ptr<const Pack> pack_;
    std::shared_ptr<const ColourData> cd_;
    EngineConfig cfg_;
    Calibration calib_;
    std::shared_ptr<const Image> image_;
    std::string input_space_;
    bool proxy_ = false;
    int viewport_ = 1600;
};

struct Cancelled : std::exception {
    const char* what() const noexcept override { return "render cancelled"; }
};
// True when the exception is a cancellation from either backend.
bool is_cancel(const std::exception& e);

// ---- presets ------------------------------------------------------------------
// A preset stores only what differs from the defaults.
nlohmann::json params_diff(const Params& p);
Params params_from_diff(const nlohmann::json& diff);
struct Preset {
    std::string name, description;
    nlohmann::json diff;  // patch for apply_params_patch on default Params
};
std::vector<Preset> builtin_presets();
nlohmann::json preset_to_json(const Preset& p);
Preset preset_from_json(const nlohmann::json& j);

// ---- 3-D LUT export -----------------------------------------------------------
struct LutOptions {
    int size = 33;                      // 17, 33 or 65
    // Encoding of the LUT input: "srgb", "rec709" (gamma 2.4), "display_p3", "acescct".
    std::string input = "srgb";
    std::string title;
};
// Writes an Adobe / Resolve .cube file of the look: the complete pipeline with
// spatial and stochastic effects (grain, halation, blur, glare) and
// autoexposure off, as the reference's lut_mode does. The output encoding
// follows the user's output settings.
void export_cube_lut(const Pack& pack, std::shared_ptr<const ColourData> colour, const Params& user,
                     const LutOptions& opt, const std::string& path);
// The LUT as an image (size^2 x size x 3, red fastest) for tests.
Image render_lut_lattice(const Pack& pack, std::shared_ptr<const ColourData> colour, const Params& user,
                         const LutOptions& opt);

// ---- files ----------------------------------------------------------------------
Image resize_long_edge(const Image& img, int long_edge);
#ifdef SPECTRA_HAVE_IO
struct FileJob {
    std::string in_path, out_path;
    std::string white_balance = "as_shot";  // as_shot | daylight | tungsten | custom
    double temperature = 6504.0, tint = 1.0;
    ExportOptions out;
};
// Decode, render at full resolution and write one file. Returns the render info.
RenderResult process_file(Engine& engine, const Params& user, const FileJob& job,
                          const std::function<void(int, int)>& progress = {},
                          const std::atomic<bool>* cancel = nullptr);
void write_result(const std::string& path, const Image& img, const ExportOptions& opt);
#endif

}  // namespace spectra::engine
