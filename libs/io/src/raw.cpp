// SPDX-License-Identifier: GPL-3.0-or-later
#include "spectra/io/raw.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>

#include <libraw/libraw.h>

#include "spectra/core/parallel.hpp"
#include "spectra/io/image_io.hpp"

namespace spectra {

namespace {
double ms_since(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}
std::string lower_ext(const std::string& path) {
    size_t p = path.find_last_of('.');
    std::string e = p == std::string::npos ? "" : path.substr(p + 1);
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return std::tolower(c); });
    return e;
}
}  // namespace

bool is_raw_file(const std::string& path) {
    static const char* exts[] = {"cr2", "cr3", "crw", "nef", "nrw", "arw", "srf", "sr2", "raf", "rw2",
                                 "rwl", "orf", "pef", "dng", "x3f", "erf", "kdc", "dcr", "mrw", "3fr",
                                 "fff", "iiq", "mos", "srw", "gpr", "raw"};
    std::string e = lower_ext(path);
    for (const char* x : exts)
        if (e == x) return true;
    return false;
}

Image load_raw(const ColourData& cd, const std::string& path, const RawLoadOptions& opt, RawInfo* info) {
    auto raw = std::make_unique<LibRaw>();
    libraw_output_params_t& p = raw->imgdata.params;
    // Same settings as rawpy.postprocess in the reference: linear 16-bit ACES,
    // no auto-brightening; everything else at LibRaw defaults.
    p.output_color = 6;  // ACES
    p.output_bps = 16;
    p.no_auto_bright = 1;
    p.gamm[0] = 1.0;
    p.gamm[1] = 1.0;
    p.use_camera_wb = opt.white_balance == RawWhiteBalance::AsShot ? 1 : 0;
    p.half_size = opt.half_size ? 1 : 0;

    auto t = std::chrono::steady_clock::now();
    int rc = raw->open_file(path.c_str());
    if (rc != LIBRAW_SUCCESS) throw std::runtime_error("cannot open RAW file " + path + ": " + libraw_strerror(rc));
    double ms_open = ms_since(t);
    t = std::chrono::steady_clock::now();
    rc = raw->unpack();
    if (rc != LIBRAW_SUCCESS) throw std::runtime_error("cannot unpack RAW file " + path + ": " + libraw_strerror(rc));
    double ms_unpack = ms_since(t);
    t = std::chrono::steady_clock::now();
    rc = raw->dcraw_process();
    if (rc != LIBRAW_SUCCESS) throw std::runtime_error("cannot process RAW file " + path + ": " + libraw_strerror(rc));
    libraw_processed_image_t* mem = raw->dcraw_make_mem_image(&rc);
    if (!mem) throw std::runtime_error("cannot render RAW file " + path + ": " + libraw_strerror(rc));
    double ms_process = ms_since(t);
    t = std::chrono::steady_clock::now();
    if (mem->type != LIBRAW_IMAGE_BITMAP || mem->colors != 3 || mem->bits != 16) {
        LibRaw::dcraw_clear_mem(mem);
        throw std::runtime_error("unexpected LibRaw output format for " + path);
    }
    int h = mem->height, w = mem->width;
    Image img(h, w, 3);
    Mat3 m = mat3_identity();
    bool adapt = false;
    if (opt.white_balance == RawWhiteBalance::Tungsten) {
        m = raw_white_balance_matrix(cd, 2850.0, 1.0);
        adapt = true;
    } else if (opt.white_balance == RawWhiteBalance::Custom) {
        m = raw_white_balance_matrix(cd, opt.temperature, opt.tint);
        adapt = true;
    }
    const unsigned char* data = mem->data;
    parallel_for(h, [&](long y0, long y1) {
        for (long y = y0; y < y1; ++y) {
            const unsigned char* row = data + static_cast<size_t>(y) * w * 6;
            double* out = img.px(static_cast<int>(y), 0);
            for (int x = 0; x < w; ++x) {
                uint16_t v[3];
                std::memcpy(v, row + static_cast<size_t>(x) * 6, 6);
                // The reference works in float32 at this point.
                Vec3 rgb{static_cast<double>(static_cast<float>(v[0]) / 65535.0f),
                         static_cast<double>(static_cast<float>(v[1]) / 65535.0f),
                         static_cast<double>(static_cast<float>(v[2]) / 65535.0f)};
                if (adapt) rgb = mat3_apply(m, rgb);
                out[x * 3] = rgb[0];
                out[x * 3 + 1] = rgb[1];
                out[x * 3 + 2] = rgb[2];
            }
        }
    });
    LibRaw::dcraw_clear_mem(mem);
    if (info) {
        const libraw_iparams_t& ip = raw->imgdata.idata;
        const libraw_image_sizes_t& s = raw->imgdata.sizes;
        const libraw_imgother_t& o = raw->imgdata.other;
        info->make = ip.make;
        info->model = ip.model;
        info->lens = raw->imgdata.lens.Lens;
        info->raw_width = s.raw_width;
        info->raw_height = s.raw_height;
        info->width = w;
        info->height = h;
        info->flip = s.flip;
        info->xtrans = ip.filters == 9;
        info->iso = o.iso_speed;
        info->shutter = o.shutter;
        info->aperture = o.aperture;
        info->focal_length = o.focal_len;
        for (int i = 0; i < 4; ++i) info->cam_mul[i] = raw->imgdata.color.cam_mul[i];
        info->black = raw->imgdata.color.black;
        info->white = raw->imgdata.color.maximum;
        info->libraw_version = LibRaw::version();
        info->ms_open = ms_open;
        info->ms_unpack = ms_unpack;
        info->ms_process = ms_process;
        info->ms_convert = ms_since(t);
    }
    raw->recycle();
    return img;
}

Image load_any(const ColourData& cd, const std::string& path, const RawLoadOptions& opt,
               std::string* color_space, RawInfo* info) {
    if (is_raw_file(path)) {
        if (color_space) *color_space = "ACES2065-1";
        return load_raw(cd, path, opt, info);
    }
    if (color_space) color_space->clear();
    return read_image(path);
}

}  // namespace spectra
