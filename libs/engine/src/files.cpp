// SPDX-License-Identifier: GPL-3.0-or-later
#include "spectra/engine/engine.hpp"

#ifdef SPECTRA_HAVE_IO
#include <algorithm>

#include "spectra/io/image_io.hpp"
#include "spectra/io/raw.hpp"

namespace spectra::engine {

void write_result(const std::string& path, const Image& img, const ExportOptions& opt) {
    WriteOptions wo;
    wo.bits = opt.bits;
    wo.is_float = opt.is_float;
    wo.jpeg_quality = opt.jpeg_quality;
    std::string ext = path.substr(path.find_last_of('.') + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (ext == "jpg" || ext == "jpeg") {
        wo.bits = 8;
        wo.is_float = false;
    } else if (ext == "png") {
        wo.is_float = false;
        if (wo.bits != 8) wo.bits = 16;
    } else if (ext == "exr") {
        if (wo.bits != 32) wo.bits = 16;
    }
    write_image(path, resize_long_edge(img, opt.long_edge), wo);
}

RenderResult process_file(Engine& engine, const Params& user, const FileJob& job,
                          const std::function<void(int, int)>& progress, const std::atomic<bool>* cancel) {
    RawLoadOptions ro;
    if (job.white_balance == "daylight") ro.white_balance = RawWhiteBalance::Daylight;
    else if (job.white_balance == "tungsten") ro.white_balance = RawWhiteBalance::Tungsten;
    else if (job.white_balance == "custom") ro.white_balance = RawWhiteBalance::Custom;
    else if (job.white_balance != "as_shot") throw std::runtime_error("unknown white balance '" + job.white_balance + "'");
    ro.temperature = job.temperature;
    ro.tint = job.tint;
    std::string space;
    auto img = std::make_shared<Image>(load_any(engine.colour(), job.in_path, ro, &space, nullptr));
    engine.set_image(img, space, false);
    img.reset();
    RenderResult r = engine.render_full(user, progress, cancel);
    write_result(job.out_path, r.image, job.out);
    engine.set_image(nullptr, "", false);
    return r;
}

}  // namespace spectra::engine
#endif
