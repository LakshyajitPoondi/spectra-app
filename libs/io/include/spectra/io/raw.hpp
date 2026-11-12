// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string>

#include "spectra/core/image.hpp"
#include "spectra/model/colour.hpp"

namespace spectra {

// RAW decoding through LibRaw; a port of
// spektrafilm.utils.raw_file_processor.load_and_process_raw_file. The output
// is linear ACES2065-1. Nothing here is specific to a camera: colour matrices,
// white balance, black / white levels and orientation come from LibRaw's
// per-camera metadata.
enum class RawWhiteBalance { AsShot, Daylight, Tungsten, Custom };

struct RawLoadOptions {
    RawWhiteBalance white_balance = RawWhiteBalance::AsShot;
    double temperature = 6504.0;  // Custom only
    double tint = 1.0;            // Custom only: multiplier on green
    // Fast preview decode: LibRaw half-size (one output pixel per 2x2 cell, no
    // demosaic interpolation). About 4x fewer pixels and several times faster.
    bool half_size = false;
};

struct RawInfo {
    std::string make, model, lens;
    int raw_width = 0, raw_height = 0;  // sensor area
    int width = 0, height = 0;          // decoded image (after orientation and half_size)
    int flip = 0;                       // LibRaw orientation code
    bool xtrans = false;
    double iso = 0, shutter = 0, aperture = 0, focal_length = 0;
    double cam_mul[4] = {0, 0, 0, 0};   // as-shot white balance multipliers
    double black = 0, white = 0;
    std::string libraw_version;
    // timings in milliseconds
    double ms_open = 0, ms_unpack = 0, ms_process = 0, ms_convert = 0;
};

bool is_raw_file(const std::string& path);  // by extension
Image load_raw(const ColourData& cd, const std::string& path, const RawLoadOptions& opt = {},
               RawInfo* info = nullptr);

// Loads any supported file as linear RGB: RAW files through load_raw (ACES2065-1),
// everything else through read_image. `color_space` receives the colour space the
// pixel data should be interpreted in ("ACES2065-1" for RAW, empty otherwise).
Image load_any(const ColourData& cd, const std::string& path, const RawLoadOptions& opt,
               std::string* color_space = nullptr, RawInfo* info = nullptr);

}  // namespace spectra
