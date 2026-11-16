// SPDX-License-Identifier: GPL-3.0-or-later
// spectra-cli commands built on the engine: calibrate, bench-preview, lut, batch, presets.
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "spectra/core/parallel.hpp"
#include "spectra/engine/engine.hpp"
#include "spectra/engine/product.hpp"
#ifdef SPECTRA_HAVE_IO
#include "spectra/io/raw.hpp"
#endif

namespace spectra {

using nlohmann::json;
namespace fs = std::filesystem;

namespace {

json read_json_file(const std::string& path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("cannot open " + path);
    return json::parse(f);
}

engine::EngineConfig config_from(std::map<std::string, std::string>& opt) {
    engine::EngineConfig c;
    if (opt.count("gpu")) c.gpu_device = opt["gpu"];
    c.force_cpu = opt.count("backend") && opt["backend"] == "cpu";
    if (opt.count("budget-mb")) c.memory_budget_bytes = std::stoull(opt["budget-mb"]) << 20;
    if (opt.count("tile")) c.tile_size = std::stoi(opt["tile"]);
    if (opt.count("match-python")) c.match_python = opt["match-python"] != "0";
    if (opt.count("calibration")) c.calibration = engine::Calibration::from_json(read_json_file(opt["calibration"]));
    return c;
}

Params user_params(std::map<std::string, std::string>& opt) {
    Params p;
    if (opt.count("preset")) p = engine::params_from_diff(engine::preset_from_json(read_json_file(opt["preset"])).diff);
    if (opt.count("film")) p.film = opt["film"];
    if (opt.count("print")) p.print = opt["print"];
    if (opt.count("params")) apply_params_patch(p, read_json_file(opt["params"]));
    return p;
}

Image synthetic(int w, int h) {
    Image img(h, w, 3);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            double u = (x + 0.5) / w, v = (y + 0.5) / h;
            double base = 0.184 * std::pow(2.0, 5.0 * (u - 0.5));
            double tex = 0.9 + 0.2 * std::sin(x * 0.37) * std::cos(y * 0.23);
            img.at(y, x, 0) = base * tex * (0.7 + 0.6 * v);
            img.at(y, x, 1) = base * tex;
            img.at(y, x, 2) = base * tex * (1.3 - 0.6 * v);
        }
    return img;
}

}  // namespace

// Returns -1 when `cmd` is not one of these tools.
int run_tool(const std::string& cmd, std::shared_ptr<const Pack> pack, std::shared_ptr<const ColourData> colour,
             std::map<std::string, std::string>& opt, const std::vector<std::string>& positional) {
    if (cmd == "calibrate") {
        engine::Engine eng(pack, colour, config_from(opt));
        engine::Calibration c = eng.calibrate([](const std::string& s) { std::printf("  %s\n", s.c_str()); });
        if (opt.count("out")) std::ofstream(opt["out"]) << c.to_json().dump(1) << "\n";
        std::printf("calibration for %s: preview %d px, draft %d px, batch %u dispatches / %.0f MP\n",
                    eng.device_name().c_str(), c.preview_long_edge, c.draft_long_edge, c.batch_max_dispatches,
                    c.batch_max_work_pixels / 1048576.0);
        return 0;
    }
    if (cmd == "presets") {
        for (auto& p : engine::builtin_presets())
            std::printf("%-40s %s\n", p.name.c_str(), p.diff.dump().c_str());
        return 0;
    }
    if (cmd == "lut") {
        if (!opt.count("out")) throw std::runtime_error("lut needs --out FILE.cube");
        engine::LutOptions lo;
        if (opt.count("size")) lo.size = std::stoi(opt["size"]);
        if (opt.count("input")) lo.input = opt["input"];
        if (opt.count("title")) lo.title = opt["title"];
        engine::export_cube_lut(*pack, colour, user_params(opt), lo, opt["out"]);
        std::printf("wrote %d^3 LUT (%s input) -> %s\n", lo.size, lo.input.c_str(), opt["out"].c_str());
        return 0;
    }
    if (cmd == "bench-preview") {
        // The interactive path as the app drives it: calibration, first preview,
        // then one slider change per pipeline stage through the stage cache.
        auto now = [] { return std::chrono::steady_clock::now(); };
        auto ms = [](std::chrono::steady_clock::time_point a) {
            return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count();
        };
        auto t_engine = now();
        engine::Engine eng(pack, colour, config_from(opt));
        std::printf("engine: %s%s (created in %.0f ms)\n", eng.device_name().c_str(),
                    eng.has_gpu() ? "" : (" - " + eng.backend_note()).c_str(), ms(t_engine));
        if (!opt.count("calibration")) {
            auto t = now();
            eng.calibrate([](const std::string& s) { std::printf("  calibration: %s\n", s.c_str()); });
            std::printf("calibration took %.1f s\n", ms(t) / 1000.0);
        }
        if (opt.count("preview")) {
            engine::Calibration c = eng.calibration();
            c.preview_long_edge = std::stoi(opt["preview"]);
            if (opt.count("draft")) c.draft_long_edge = std::stoi(opt["draft"]);
            eng.set_calibration(c);
        }
        eng.set_viewport(opt.count("viewport") ? std::stoi(opt["viewport"]) : 2560);
        std::string space;
        std::shared_ptr<const Image> img;
        double open_ms = 0;
        auto t_open = now();
#ifdef SPECTRA_HAVE_IO
        if (opt.count("in")) {
            RawLoadOptions ro;
            ro.half_size = true;  // what the app shows first
            RawInfo info;
            img = std::make_shared<Image>(load_any(*colour, opt["in"], ro, &space, &info));
            open_ms = ms(t_open);
            std::printf("input: %s (%s %s), fast decode %dx%d in %.0f ms\n", opt["in"].c_str(), info.make.c_str(),
                        info.model.c_str(), img->w, img->h, open_ms);
        }
#endif
        if (!img) {
            double mp = opt.count("mp") ? std::stod(opt["mp"]) : 20.0;
            int w = static_cast<int>(std::lround(std::sqrt(mp * 1e6 * 1.5))), h = static_cast<int>(std::lround(w / 1.5));
            img = std::make_shared<Image>(synthetic(w, h));
            std::printf("input: synthetic %dx%d\n", w, h);
        }
        auto t_first = now();
        eng.set_image(img, space, false);
        Params p = user_params(opt);
        engine::RenderResult d0 = eng.render_preview(p, engine::Level::Draft, {});
        double first_draft = ms(t_first);
        engine::RenderResult p0 = eng.render_preview(p, engine::Level::Preview, {});
        double first_full = ms(t_first);
        std::printf("first preview after decode: draft %dx%d after %.0f ms, refined %dx%d after %.0f ms\n", d0.image.w,
                    d0.image.h, first_draft, p0.image.w, p0.image.h, first_full);
        if (open_ms > 0)
            std::printf("RAW open to first preview: %.0f ms (draft), %.0f ms (refined)   [target < 1500 ms]\n",
                        open_ms + first_draft, open_ms + first_full);
        struct Change {
            const char* name;
            std::function<void(Params&, double)> apply;
        };
        std::vector<Change> changes = {
            {"scanner (unsharp mask)", [](Params& q, double v) { q.scanner.unsharp_mask[1] = 0.7 + v; }},
            {"print (exposure)", [](Params& q, double v) { q.enlarger.print_exposure = 1.0 + v; }},
            {"print (Y filter)", [](Params& q, double v) { q.enlarger.y_filter_shift = v * 10; }},
            {"grain (amount)", [](Params& q, double v) { q.film_render.grain.blur = 0.8 + v; }},
            {"film development (couplers)", [](Params& q, double v) { q.film_render.dir_couplers.amount = 1.0 + v; }},
            {"camera exposure", [](Params& q, double v) { q.camera.exposure_compensation_ev = v; }},
        };
        int reps = opt.count("repeats") ? std::stoi(opt["repeats"]) : 5;
        std::printf("slider change, draft / refined preview (ms, median of %d)   [target < 50 ms]\n", reps);
        double worst_late = 0;
        for (auto& c : changes) {
            std::vector<double> td, tp;
            for (int i = 0; i < reps; ++i) {
                Params q = p;
                c.apply(q, 0.01 * (i + 1));
                auto t = now();
                eng.render_preview(q, engine::Level::Draft, {});
                td.push_back(ms(t));
                t = now();
                eng.render_preview(q, engine::Level::Preview, {});
                tp.push_back(ms(t));
            }
            std::sort(td.begin(), td.end());
            std::sort(tp.begin(), tp.end());
            std::printf("  %-30s %8.1f / %8.1f\n", c.name, td[td.size() / 2], tp[tp.size() / 2]);
            worst_late = std::max(worst_late, td[td.size() / 2]);
        }
        // 100 % view
        engine::ViewRect view{img->w / 2 - 800, img->h / 2 - 500, 1600, 1000};
        auto t = now();
        engine::RenderResult v0 = eng.render_preview(p, engine::Level::Preview, view);
        std::printf("100%% view %dx%d: first %.0f ms", v0.image.w, v0.image.h, ms(t));
        Params q = p;
        q.scanner.unsharp_mask[1] = 0.9;
        t = now();
        eng.render_preview(q, engine::Level::Preview, view);
        std::printf(", scanner change %.0f ms\n", ms(t));
        std::printf("GPU memory held by the preview: %.0f MiB; peak process memory %.2f GiB\n",
                    eng.gpu_allocated_bytes() / 1048576.0, peak_process_memory_bytes() / 1073741824.0);
        return 0;
    }
#ifdef SPECTRA_HAVE_IO
    if (cmd == "batch") {
        // batch --out-dir DIR [--format tif|png|jpg|exr] [--bits N] [--long-edge N] files...
        if (!opt.count("out-dir") || positional.empty())
            throw std::runtime_error("batch needs --out-dir DIR and at least one input file");
        fs::create_directories(opt["out-dir"]);
        std::string format = opt.count("format") ? opt["format"] : "tif";
        engine::Engine eng(pack, colour, config_from(opt));
        Params p = user_params(opt);
        int failed = 0;
        for (auto& in : positional) {
            engine::FileJob job;
            job.in_path = in;
            job.out_path = (fs::path(opt["out-dir"]) / (fs::path(in).stem().string() + "." + format)).string();
            if (opt.count("wb")) job.white_balance = opt["wb"];
            if (opt.count("temperature")) job.temperature = std::stod(opt["temperature"]);
            if (opt.count("tint")) job.tint = std::stod(opt["tint"]);
            if (opt.count("bits")) job.out.bits = std::stoi(opt["bits"]);
            job.out.is_float = opt.count("float") && opt["float"] != "0";
            if (opt.count("long-edge")) job.out.long_edge = std::stoi(opt["long-edge"]);
            try {
                engine::RenderResult r = engine::process_file(eng, p, job);
                std::printf("%s -> %s  (%dx%d, %.1f s, %d tile(s)%s)\n", in.c_str(), job.out_path.c_str(), r.image.w,
                            r.image.h, r.ms / 1000.0, r.tiles, r.gpu ? "" : ", CPU");
            } catch (const std::exception& e) {
                std::printf("%s: FAILED: %s\n", in.c_str(), e.what());
                ++failed;
            }
        }
        std::printf("batch: %zu file(s), %d failed, peak process memory %.2f GiB\n", positional.size(), failed,
                    peak_process_memory_bytes() / 1073741824.0);
        return failed ? 1 : 0;
    }
#endif
    return -1;
}

}  // namespace spectra
