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
};

// Runs every case of a cases.json file and compares with the stored Python
// goldens. Returns the number of failed cases.
int run_parity(std::shared_ptr<const ColourData> colour, const ParityOptions& opt);

}  // namespace spectra
