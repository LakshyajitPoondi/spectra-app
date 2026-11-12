// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <memory>
#include <string>

#include "spectra/model/colour.hpp"

namespace spectra {

struct ParityOptions {
    std::string cases_path;
    std::string filter;       // substring of the case id; empty = all
    std::string report_path;  // optional JSON report
    std::string dump_dir;     // optional: write our outputs as .npy
    bool verbose = false;
    // "cpu" (reference) or "gpu" (Vulkan). The GPU backend works in fp32 and
    // is compared with the tolerances below instead of the per-case ones.
    std::string backend = "cpu";
    std::string gpu_device;
    double gpu_atol = 2e-3, gpu_rtol = 2e-3;
    int gpu_tile_size = 0;
    double gpu_approx_rmse = 0.003, gpu_approx_p99 = 0.01;
};

// Runs every case of a cases.json file and compares with the stored Python
// goldens. Returns the number of failed cases.
int run_parity(std::shared_ptr<const ColourData> colour, const ParityOptions& opt);

}  // namespace spectra
