// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <memory>

#include "spectra/core/image.hpp"
#include "spectra/model/colour.hpp"
#include "spectra/model/params.hpp"

namespace spectra {

// Reinhard knee on a normalised distance (ACES RGC v1.3 form).
double reinhard_knee(double d, double threshold, double limit, double power);

// compress_xy: input-side compression of CIE xy toward the spectral locus.
Xy compress_xy(const ColourData& cd, const Xy& xy, const Xy& white_xy,
               const InputGamutCompressSpec& spec);

// C_max(L, h) table of a polar perceptual space against an RGB cube or the locus.
struct CMaxTable {
    Vec l_grid, table;  // table [n_l x n_h]
    int n_l = 0, n_h = 0;
    double lookup(double L, double h) const;
};

struct Cam16View;

// Prepared output compressor for one (spec, output colour space) pair.
class OutputGamutCompressor {
public:
    OutputGamutCompressor(const ColourData& cd, const OutputGamutCompressSpec& spec,
                          const std::string& output_color_space);
    void apply(Image& rgb) const;  // in place, linear output RGB
    Vec3 apply(const Vec3& rgb) const;
    const CMaxTable& table() const { return table_; }

private:
    const ColourData& cd_;
    OutputGamutCompressSpec spec_;
    Mat3 rgb_to_xyz_{}, xyz_to_rgb_{};
    CMaxTable table_;
    double l_white_ = 1.0;
    std::shared_ptr<const Cam16View> cam16_;
    Mat3 jz_xyz_to_lms_{}, jz_lms_to_xyz_{}, jz_lmsp_to_iab_{}, jz_iab_to_lmsp_{};
    Vec3 to_polar_space(const Vec3& xyz) const;
    Vec3 from_polar_space(const Vec3& lab) const;
};

}  // namespace spectra
