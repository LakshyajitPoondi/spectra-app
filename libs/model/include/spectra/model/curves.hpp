// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <optional>
#include <string>

#include "spectra/core/image.hpp"
#include "spectra/core/math.hpp"
#include "spectra/data/pack.hpp"

namespace spectra {

// Chemistry (s023 coupled-gamma morph) controls; shared by film and print.
struct ChemistryParams {
    std::optional<double> development_time;
    bool active = true;
    double gamma_factor = 1.0;
    double gamma_factor_fast = 1.0;
    double gamma_factor_slow = 1.0;
    double gamma_factor_red = 1.0;
    double gamma_factor_green = 1.0;
    double gamma_factor_blue = 1.0;
    double developer_exhaustion = 0.0;
};

// Sampled characteristic curves on the log-exposure grid.
struct Curves {
    int n_le = 0, n_ch = 0, n_layers = 0;
    Vec total;   // [n_le x n_ch]
    Vec layers;  // [n_le x n_layers x n_ch]; empty when there is no sub-layer structure
    bool has_layers() const { return !layers.empty(); }
    double t(int i, int ch) const { return total[static_cast<size_t>(i) * n_ch + ch]; }
    double l(int i, int layer, int ch) const {
        return layers[(static_cast<size_t>(i) * n_layers + layer) * n_ch + ch];
    }
    Vec column_max() const;  // nanmax over exposure, per channel
    Vec column_min() const;  // nanmin over exposure, per channel
};

// evaluate_density_curves / evaluate_density_curves_layers.
Curves evaluate_density_curves(const DensityCurvesModel& model, const Vec& log_exposure,
                               const std::string& profile_type);

// Index chosen by select_development_time (-1: no family, nothing to select).
int select_development_time_index(const Vec& times, int n_columns,
                                  const std::optional<double>& development_time);
// Keep only row `index` of a development-time family model.
DensityCurvesModel select_model_row(const DensityCurvesModel& model, int index);

// apply_print_curves_morph_with_layers: morphed total + per-layer curves.
Curves apply_curves_morph(const Vec& log_exposure, const DensityCurvesModel& model,
                          const ChemistryParams& chem, const std::string& profile_type);

// interpolate_exposure_to_density with unit gamma: per-channel fast_interp of
// log-exposure through the curves. `curves` is [n_le x n_ch].
Image interpolate_exposure_to_density(const Image& log_raw, const Vec& curves,
                                      const Vec& log_exposure);

}  // namespace spectra
