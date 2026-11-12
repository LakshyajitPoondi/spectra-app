// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string>

#include "spectra/core/image.hpp"

namespace spectra {

// Raster image files. Reading returns float64 pixel values: integer formats
// are normalised to [0, 1] (as the reference's load_image_oiio does), float
// formats are returned unchanged. Alpha is dropped.
struct ImageFileInfo {
    std::string format;  // "tiff", "png", "jpeg", "exr", "npy"
    int bits = 0;        // bits per sample as stored
    bool is_float = false;
    int channels = 0;
};

Image read_image(const std::string& path, ImageFileInfo* info = nullptr);

struct WriteOptions {
    int bits = 16;          // 8 or 16 for TIFF/PNG, 8 for JPEG, 16 (half) or 32 for EXR, 32 for float TIFF
    bool is_float = false;  // TIFF only: 32-bit float samples
    int jpeg_quality = 95;
};
// Format from the extension: .tif/.tiff, .png, .jpg/.jpeg, .exr, .npy.
// Integer output is clipped to [0, 1] and rounded.
void write_image(const std::string& path, const Image& img, const WriteOptions& opt = {});

}  // namespace spectra
