// SPDX-License-Identifier: GPL-3.0-or-later
#include <cmath>
#include <cstdio>
#include <fstream>
#include <stdexcept>

#include "spectra/engine/engine.hpp"
#include "spectra/engine/product.hpp"

namespace spectra::engine {

namespace {

// ACEScct (S-2016-001) to linear AP1.
double acescct_to_linear(double v) {
    return v > 0.155251141552511 ? std::pow(2.0, v * 17.52 - 9.72) : (v - 0.0729055341958355) / 10.5402377416545;
}
// AP1 (ACEScg) to AP0 (ACES2065-1), both D60: the standard ACES matrix.
const double kAp1ToAp0[9] = {0.6954522414, 0.1406786965, 0.1638690622, 0.0447945634, 0.8596711185,
                             0.0955343182, -0.0055258826, 0.0040252103, 1.0015006723};

}  // namespace

Image render_lut_lattice(const Pack& pack, std::shared_ptr<const ColourData> colour, const Params& user,
                         const LutOptions& opt) {
    int n = opt.size;
    if (n < 2 || n > 129) throw std::runtime_error("LUT size must be between 2 and 129");
    Params p = user;
    p.debug.lut_mode = true;  // no spatial or stochastic effects, no autoexposure
    p.io.crop = false;
    p.io.upscale_factor = 1.0;
    Image in(n * n, n, 3);
    for (int b = 0; b < n; ++b)
        for (int g = 0; g < n; ++g)
            for (int r = 0; r < n; ++r) {
                double v[3] = {r / (n - 1.0), g / (n - 1.0), b / (n - 1.0)};
                double* px = in.px(g + n * b, r);
                if (opt.input == "acescct") {
                    double lin[3];
                    for (int k = 0; k < 3; ++k) lin[k] = acescct_to_linear(v[k]);
                    for (int k = 0; k < 3; ++k)
                        px[k] = kAp1ToAp0[k * 3] * lin[0] + kAp1ToAp0[k * 3 + 1] * lin[1] + kAp1ToAp0[k * 3 + 2] * lin[2];
                } else if (opt.input == "rec709") {
                    for (int k = 0; k < 3; ++k) px[k] = std::pow(v[k], 2.4);
                } else {
                    for (int k = 0; k < 3; ++k) px[k] = v[k];
                }
            }
    if (opt.input == "acescct") {
        p.io.input_color_space = "ACES2065-1";
        p.io.input_cctf_decoding = false;
    } else if (opt.input == "rec709") {
        p.io.input_color_space = "sRGB";
        p.io.input_cctf_decoding = false;
    } else if (opt.input == "srgb") {
        p.io.input_color_space = "sRGB";
        p.io.input_cctf_decoding = true;
    } else if (opt.input == "display_p3") {
        p.io.input_color_space = "Display P3";
        p.io.input_cctf_decoding = true;
    } else {
        throw std::runtime_error("unknown LUT input encoding '" + opt.input + "'");
    }
    Params d = digest_params(pack, p);
    Pipeline pipe(std::move(colour), d, RenderOptions{});
    return pipe.process(in);
}

void export_cube_lut(const Pack& pack, std::shared_ptr<const ColourData> colour, const Params& user,
                     const LutOptions& opt, const std::string& path) {
    Image lut = render_lut_lattice(pack, std::move(colour), user, opt);
    std::ofstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot write " + path);
    std::string title = opt.title.empty() ? user.film + " / " + (user.io.scan_film ? "scan" : user.print) : opt.title;
    for (auto& ch : title)
        if (ch == '"') ch = '\'';
    f << "# Created by " << product::kName << " " << product::kVersion << "\n"
      << "# Film model and profiles: spektrafilm by Andrea Volpato (code GPL-3.0, profiles CC BY-SA 4.0)\n"
      << "# Input: " << opt.input << "; output: " << user.io.output_color_space
      << (user.io.output_cctf_encoding ? " (encoded)" : " (linear)") << "\n"
      << "# Grain, halation, blur, glare and autoexposure are not part of a LUT.\n"
      << "TITLE \"" << title << "\"\n"
      << "LUT_3D_SIZE " << opt.size << "\nDOMAIN_MIN 0.0 0.0 0.0\nDOMAIN_MAX 1.0 1.0 1.0\n";
    char line[96];
    for (size_t i = 0; i < lut.pixels(); ++i) {
        double v[3];
        for (int k = 0; k < 3; ++k) {
            v[k] = lut.v[i * 3 + k];
            if (!std::isfinite(v[k])) v[k] = 0.0;
        }
        std::snprintf(line, sizeof line, "%.6f %.6f %.6f\n", v[0], v[1], v[2]);
        f << line;
    }
    if (!f) throw std::runtime_error("error writing " + path);
}

}  // namespace spectra::engine
