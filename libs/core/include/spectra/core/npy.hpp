// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string>
#include <vector>

#include "spectra/core/image.hpp"

namespace spectra {

// Minimal .npy reader/writer (little-endian float64/float32, C order).
struct NpyArray {
    std::vector<size_t> shape;
    std::vector<double> data;
    size_t size() const { return data.size(); }
};

NpyArray npy_load(const std::string& path);
void npy_save(const std::string& path, const std::vector<size_t>& shape,
              const std::vector<double>& data);

Image npy_load_image(const std::string& path);  // accepts HxW, HxWxC
void npy_save_image(const std::string& path, const Image& img);

}  // namespace spectra
