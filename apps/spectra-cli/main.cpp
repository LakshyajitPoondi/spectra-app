// SPDX-License-Identifier: GPL-3.0-or-later
// spectra-cli: headless tools for the CPU reference pipeline.
#include <chrono>
#include <cstdio>
#include <algorithm>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>

#include "parity.hpp"
#include "spectra/core/npy.hpp"
#include "spectra/core/parallel.hpp"
#include "spectra/pipeline/pipeline.hpp"
#ifdef SPECTRA_HAVE_GPU
#include "spectra/gpu/gpu_pipeline.hpp"
#endif
#ifdef SPECTRA_HAVE_IO
#include "spectra/io/image_io.hpp"
#include "spectra/io/raw.hpp"
#endif

using namespace spectra;
using nlohmann::json;

namespace {

std::string default_pack_path(const char* argv0) {
    if (const char* env = std::getenv("SPECTRA_PACK")) return env;
    namespace fs = std::filesystem;
    for (fs::path base : {fs::current_path(), fs::absolute(argv0).parent_path()})
        for (int up = 0; up < 5; ++up) {
            fs::path p = base / "data" / "spektrafilm.sfpk";
            if (fs::exists(p)) return p.string();
            if (!base.has_parent_path()) break;
            base = base.parent_path();
        }
    return "data/spektrafilm.sfpk";
}

int usage() {
    std::puts(
        "spectra-cli <command> [options]\n"
        "  info                         show data-pack provenance and the profile list\n"
        "  pack-verify                  check every array of the data pack against its checksum\n"
        "  render  --in FILE --out FILE [--film F] [--print P] [--params p.json]\n"
        "          [--post p.json] [--options o.json] [--inject tap] [--collect tap]\n"
        "          FILE: .npy, .tif, .png, .jpg, .exr, or a camera RAW file as input\n"
        "          RAW:  [--wb as_shot|daylight|tungsten|custom] [--temperature K] [--tint x] [--half 1]\n"
        "          out:  [--bits 8|16|32] [--float 1]\n"
        "  convert --in FILE --out FILE        decode only (RAW -> linear ACES2065-1), no simulation\n"
        "  digest  [--film F] [--print P] [--params p.json]     print the digested parameters\n"
        "  bench-open --in RAW [--preview 1600] [--out FILE]   time RAW open to first preview\n"
        "  parity  cases.json [--filter s] [--report r.json] [--dump dir] [-v]\n"
        "  gpu-info [--gpu NAME|INDEX]         list Vulkan devices\n"
        "GPU: add --backend gpu to render / parity; [--gpu NAME|INDEX] [--tile N] [--budget-mb N]\n"
        "common options: --pack path/to/spektrafilm.sfpk   --threads N");
    return 2;
}

// Reads any supported image. RAW files come back as linear ACES2065-1 and
// set `color_space` accordingly.
Image load_input(const ColourData& cd, const std::string& path, std::map<std::string, std::string>& opt,
                 std::string* color_space) {
#ifdef SPECTRA_HAVE_IO
    RawLoadOptions ro;
    std::string wb = opt.count("wb") ? opt["wb"] : "as_shot";
    if (wb == "as_shot") ro.white_balance = RawWhiteBalance::AsShot;
    else if (wb == "daylight") ro.white_balance = RawWhiteBalance::Daylight;
    else if (wb == "tungsten") ro.white_balance = RawWhiteBalance::Tungsten;
    else if (wb == "custom") ro.white_balance = RawWhiteBalance::Custom;
    else throw std::runtime_error("unknown --wb value '" + wb + "'");
    if (opt.count("temperature")) ro.temperature = std::stod(opt["temperature"]);
    if (opt.count("tint")) ro.tint = std::stod(opt["tint"]);
    ro.half_size = opt.count("half") && opt["half"] != "0";
    RawInfo info;
    Image img = load_any(cd, path, ro, color_space, &info);
    if (!info.make.empty())
        std::printf("raw: %s %s, %dx%d, flip %d%s, ISO %.0f, LibRaw %s; open %.0f ms, unpack %.0f ms, "
                    "process %.0f ms, convert %.0f ms\n",
                    info.make.c_str(), info.model.c_str(), info.width, info.height, info.flip,
                    info.xtrans ? ", X-Trans" : "", info.iso, info.libraw_version.c_str(), info.ms_open,
                    info.ms_unpack, info.ms_process, info.ms_convert);
    return img;
#else
    (void)cd;
    (void)opt;
    if (color_space) color_space->clear();
    return npy_load_image(path);
#endif
}

void save_output(const std::string& path, const Image& img, std::map<std::string, std::string>& opt) {
#ifdef SPECTRA_HAVE_IO
    WriteOptions wo;
    if (opt.count("bits")) wo.bits = std::stoi(opt["bits"]);
    wo.is_float = opt.count("float") && opt["float"] != "0";
    write_image(path, img, wo);
#else
    (void)opt;
    npy_save_image(path, img);
#endif
}

json read_json(const std::string& path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("cannot open " + path);
    return json::parse(f);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) return usage();
    std::string cmd = argv[1];
    std::map<std::string, std::string> opt;
    std::vector<std::string> positional;
    bool verbose = false;
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-v") verbose = true;
        else if (a.rfind("--", 0) == 0 && i + 1 < argc) opt[a.substr(2)] = argv[++i];
        else positional.push_back(a);
    }
    try {
        if (opt.count("threads")) thread_count() = std::max(1, std::stoi(opt["threads"]));
#ifdef SPECTRA_HAVE_GPU
        if (cmd == "gpu-info") {
            auto devices = gpu::Context::enumerate();
            for (size_t i = 0; i < devices.size(); ++i) {
                const auto& d = devices[i];
                std::printf("[%zu] %s  (vendor 0x%04x, driver %s, Vulkan %u.%u.%u, %s, heap %.1f GiB%s)\n", i,
                            d.name.c_str(), d.vendor_id, d.driver.c_str(), VK_VERSION_MAJOR(d.api_version),
                            VK_VERSION_MINOR(d.api_version), VK_VERSION_PATCH(d.api_version),
                            d.is_software() ? "software" : (d.type == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? "integrated" : "discrete/other"),
                            d.heap_bytes / 1073741824.0, d.unified_memory ? ", unified memory" : "");
            }
            gpu::ContextOptions co;
            if (opt.count("gpu")) co.device = opt["gpu"];
            gpu::Context ctx(co);
            std::printf("selected: %s (memory budget %.1f GiB)\n", ctx.info().name.c_str(),
                        ctx.query_budget_bytes() / 1073741824.0);
            return 0;
        }
#endif
        std::string pack_path = opt.count("pack") ? opt["pack"] : default_pack_path(argv[0]);
        auto pack = Pack::load(pack_path);
        if (cmd == "pack-verify") {
            auto bad = pack->verify();
            for (auto& n : bad) std::printf("CORRUPT %s\n", n.c_str());
            std::printf("%s: %zu arrays, %zu corrupt\n", pack_path.c_str(), pack->array_names().size(), bad.size());
            return bad.empty() ? 0 : 1;
        }
        if (cmd == "info") {
            const json& prov = pack->meta().at("provenance");
            std::printf("data pack: %s\n", pack_path.c_str());
            for (auto it = prov.begin(); it != prov.end(); ++it)
                std::printf("  %-26s %s\n", it.key().c_str(), it.value().dump().c_str());
            std::printf("profiles:\n");
            for (auto& s : list_profiles(*pack)) {
                Profile p = load_profile(*pack, s);
                std::printf("  %-34s %-8s %-5s %-5s %s\n", s.c_str(), p.type.c_str(), p.support.c_str(),
                            p.channel_model.c_str(), p.stage.c_str());
            }
            return 0;
        }
        auto colour = std::make_shared<const ColourData>(pack);
        if (cmd == "convert") {
            // Decode a file (RAW or raster) and write it unprocessed: linear data as loaded.
            if (!opt.count("in") || !opt.count("out")) return usage();
            std::string space;
            Image img = load_input(*colour, opt["in"], opt, &space);
            save_output(opt["out"], img, opt);
            std::printf("converted %dx%dx%d%s -> %s\n", img.w, img.h, img.c,
                        space.empty() ? "" : (" (" + space + ")").c_str(), opt["out"].c_str());
            return 0;
        }
#ifdef SPECTRA_HAVE_IO
        if (cmd == "bench-open") {
            // "Open to first preview": half-size RAW decode, reduce to the preview
            // size, build the pipeline tables and render the preview.
            if (!opt.count("in")) return usage();
            int preview_px = opt.count("preview") ? std::stoi(opt["preview"]) : 1600;
            auto now = [] { return std::chrono::steady_clock::now(); };
            auto ms = [](std::chrono::steady_clock::time_point a) {
                return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count();
            };
            auto t_all = now();
            RawLoadOptions ro;
            ro.half_size = true;
            RawInfo info;
            std::string space;
            Image img = load_any(*colour, opt["in"], ro, &space, &info);
            double t_decode = ms(t_all);
            auto t = now();
            Image small = downsample_area(img, preview_px);
            double t_resize = ms(t);
            t = now();
            Params p;
            if (opt.count("film")) p.film = opt["film"];
            if (opt.count("print")) p.print = opt["print"];
            if (!space.empty()) p.io.input_color_space = space;
            Params d = digest_params(*pack, p);
            Pipeline pipe(colour, d, RenderOptions{});
            double t_tables = ms(t);
            t = now();
            Image out = pipe.process(small);
            double t_render = ms(t);
            double total = ms(t_all);
            std::printf("open-to-preview (%s backend) %s %s: %dx%d -> preview %dx%d\n"
                        "  decode (half-size) %7.0f ms   (unpack %.0f, process %.0f, convert %.0f)\n"
                        "  reduce             %7.0f ms\n  tables             %7.0f ms\n"
                        "  render             %7.0f ms\n  total              %7.0f ms\n",
                        "cpu-reference", info.make.c_str(), info.model.c_str(), info.width, info.height,
                        out.w, out.h, t_decode, info.ms_unpack, info.ms_process, info.ms_convert, t_resize,
                        t_tables, t_render, total);
            if (opt.count("out")) save_output(opt["out"], out, opt);
            return 0;
        }
#endif
        if (cmd == "parity") {
            if (positional.empty()) return usage();
            ParityOptions po;
            po.cases_path = positional[0];
            po.filter = opt.count("filter") ? opt["filter"] : "";
            po.report_path = opt.count("report") ? opt["report"] : "";
            po.dump_dir = opt.count("dump") ? opt["dump"] : "";
            po.verbose = verbose;
            if (opt.count("backend")) po.backend = opt["backend"];
            if (opt.count("gpu")) po.gpu_device = opt["gpu"];
            if (opt.count("atol")) po.gpu_atol = std::stod(opt["atol"]);
            if (opt.count("rtol")) po.gpu_rtol = std::stod(opt["rtol"]);
            if (opt.count("tile")) po.gpu_tile_size = std::stoi(opt["tile"]);
            return run_parity(colour, po) == 0 ? 0 : 1;
        }
        if (cmd == "render" || cmd == "digest") {
            Params p;
            if (opt.count("film")) p.film = opt["film"];
            if (opt.count("print")) p.print = opt["print"];
            if (opt.count("params")) apply_params_patch(p, read_json(opt["params"]));
            Params d = digest_params(*pack, p);
            if (opt.count("post")) apply_params_patch(d, read_json(opt["post"]));
            if (cmd == "digest") {
                std::cout << params_to_json(d).dump(1) << "\n";
                return 0;
            }
            RenderOptions ro;
            if (opt.count("options")) apply_render_options_patch(ro, read_json(opt["options"]));
            if (!opt.count("in") || !opt.count("out")) return usage();
            std::string in_space;
            Image in = load_input(*colour, opt["in"], opt, &in_space);
            if (!in_space.empty()) d.io.input_color_space = in_space;  // RAW: linear ACES2065-1
            Image out;
            Tap collect = tap_from_string(opt.count("collect") ? opt["collect"] : "rgb_out");
            if (opt.count("backend") && opt["backend"] == "gpu") {
#ifdef SPECTRA_HAVE_GPU
                gpu::ContextOptions co;
                if (opt.count("gpu")) co.device = opt["gpu"];
                auto ctx = std::make_shared<gpu::Context>(co);
                gpu::GpuOptions go;
                if (opt.count("tile")) go.tile_size = std::stoi(opt["tile"]);
                if (opt.count("budget-mb")) go.memory_budget_bytes = std::stoull(opt["budget-mb"]) << 20;
                gpu::GpuRenderInfo gi;
                out = gpu::GpuPipeline(ctx, colour, d, ro, go).process(in, collect, &gi);
                std::printf("gpu: %s, %.0f ms, peak GPU memory %.0f MiB, %d tile(s)\n", ctx->info().name.c_str(),
                            gi.total_ms, gi.peak_gpu_bytes / 1048576.0, gi.tiles);
                for (auto& f : gi.cpu_fallbacks) std::printf("  ran on the CPU: %s\n", f.c_str());
#else
                throw std::runtime_error("this build has no GPU backend");
#endif
            } else {
                Pipeline pipe(colour, d, ro);
                out = pipe.process(in, tap_from_string(opt.count("inject") ? opt["inject"] : "rgb_in"), collect);
            }
            save_output(opt["out"], out, opt);
            std::printf("rendered %dx%dx%d -> %s\n", out.w, out.h, out.c, opt["out"].c_str());
            return 0;
        }
        return usage();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
