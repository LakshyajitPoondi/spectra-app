// SPDX-License-Identifier: GPL-3.0-or-later
// RAW test set: every file in a folder must decode (fast and full), have the
// expected orientation, and render to a sane preview. The files are not in the
// repository (test_data/SOURCES.md); an empty or missing folder is a skip
// (exit code 77), not a failure.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "spectra/engine/engine.hpp"
#include "spectra/io/raw.hpp"

using namespace spectra;
namespace fs = std::filesystem;

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("usage: test_raw pack.sfpk folder\n");
        return 2;
    }
    std::vector<fs::path> files;
    std::error_code ec;
    if (fs::is_directory(argv[2], ec))
        for (auto& e : fs::directory_iterator(argv[2], ec))
            if (e.is_regular_file() && is_raw_file(e.path().string())) files.push_back(e.path());
    if (files.empty()) {
        std::printf("no RAW files in %s: skipped (see test_data/SOURCES.md)\n", argv[2]);
        return 77;
    }
    std::sort(files.begin(), files.end());
    auto pack = Pack::load(argv[1]);
    auto cd = std::make_shared<const ColourData>(pack);
    engine::EngineConfig cfg;
    cfg.force_cpu = true;  // decoding and colour are under test here, not the GPU
    engine::Engine eng(pack, cd, cfg);
    engine::Calibration c = eng.calibration();
    c.preview_long_edge = 256;
    c.draft_long_edge = 128;
    eng.set_calibration(c);
    Params p = engine::default_user_params(*pack, "kodak_portra_400", "kodak_portra_endura");
    int fail = 0;
    for (auto& f : files) {
        std::string name = f.filename().string();
        try {
            RawLoadOptions ro;
            ro.half_size = true;
            RawInfo hi, fi;
            std::string space;
            auto half = std::make_shared<Image>(load_any(*cd, f.string(), ro, &space, &hi));
            ro.half_size = false;
            Image full = load_any(*cd, f.string(), ro, &space, &fi);
            bool ok = true;
            auto bad = [&](const char* what) {
                std::printf("FAIL %s: %s\n", name.c_str(), what);
                ok = false;
            };
            if (space != "ACES2065-1") bad("RAW colour space is not ACES2065-1");
            if (full.w < 1000 || full.h < 1000) bad("implausible size");
            if (std::abs(half->w * 2 - full.w) > 8 || std::abs(half->h * 2 - full.h) > 8) bad("fast decode is not half size");
            if (name.find("portrait") != std::string::npos && full.h <= full.w) bad("portrait file is not upright");
            if (name.find("xtrans") != std::string::npos && !fi.xtrans) bad("X-Trans sensor not recognised");
            double mean = 0;
            for (double v : full.v) {
                if (!std::isfinite(v)) {
                    bad("non-finite pixel");
                    break;
                }
                mean += v;
            }
            mean /= static_cast<double>(full.v.size());
            if (!(mean > 0.002 && mean < 2.0)) bad("implausible exposure");
            // fast and full decode agree in the large
            double hm = 0;
            for (double v : half->v) hm += v;
            hm /= static_cast<double>(half->v.size());
            if (std::fabs(hm - mean) > 0.05 * mean + 1e-4) bad("fast and full decode differ in mean level");
            eng.set_image(half, space, true);
            engine::RenderResult r = eng.render_preview(p, engine::Level::Preview, {});
            double pm = 0;
            for (double v : r.image.v) {
                if (!std::isfinite(v)) {
                    bad("preview has non-finite pixels");
                    break;
                }
                pm += v;
            }
            pm /= static_cast<double>(r.image.v.size());
            if (!(pm > 0.08 && pm < 0.85)) bad("preview is nearly black or white");
            std::printf("%s %-34s %s %s  %dx%d  flip %d%s  decode %.0f ms (fast %.0f)  preview mean %.3f\n",
                        ok ? "ok  " : "FAIL", name.c_str(), fi.make.c_str(), fi.model.c_str(), full.w, full.h, fi.flip,
                        fi.xtrans ? " X-Trans" : "", fi.ms_open + fi.ms_unpack + fi.ms_process + fi.ms_convert,
                        hi.ms_open + hi.ms_unpack + hi.ms_process + hi.ms_convert, pm);
            if (!ok) ++fail;
        } catch (const std::exception& e) {
            std::printf("FAIL %s: %s\n", name.c_str(), e.what());
            ++fail;
        }
    }
    std::printf("%zu RAW file(s), %d failed\n", files.size(), fail);
    return fail ? 1 : 0;
}
