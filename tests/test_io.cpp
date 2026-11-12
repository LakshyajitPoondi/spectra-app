// SPDX-License-Identifier: GPL-3.0-or-later
// Round-trip tests of the raster image formats.
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>

#include "spectra/core/rng.hpp"
#include "spectra/io/image_io.hpp"

using namespace spectra;

static int failures = 0;

static double max_diff(const Image& a, const Image& b) {
    if (!a.same_shape(b)) return 1e9;
    double m = 0;
    for (size_t i = 0; i < a.v.size(); ++i) m = std::fmax(m, std::fabs(a.v[i] - b.v[i]));
    return m;
}

static void check(const std::string& what, double diff, double limit) {
    bool ok = diff <= limit;
    std::printf("%-4s %-28s max diff %.3e (limit %.1e)\n", ok ? "PASS" : "FAIL", what.c_str(), diff, limit);
    if (!ok) ++failures;
}

int main() {
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() / "spectra_test_io";
    fs::create_directories(dir);
    for (int c : {3, 1}) {
        // Odd size, values in [0, 1] plus HDR values for the float formats.
        Image img(37, 53, c), hdr(37, 53, c);
        for (int y = 0; y < img.h; ++y)
            for (int x = 0; x < img.w; ++x)
                for (int k = 0; k < c; ++k) {
                    PixelRng rng(1, static_cast<uint32_t>(k), static_cast<uint32_t>(x), static_cast<uint32_t>(y));
                    double u = rng.uniform();
                    img.at(y, x, k) = u;
                    hdr.at(y, x, k) = u * u * 40.0 - 0.5;
                }
        std::string tag = c == 3 ? "rgb" : "grey";
        auto path = [&](const char* name) { return (dir / (tag + "_" + name)).string(); };
        WriteOptions o8, o16, of, oh;
        o8.bits = 8;
        o16.bits = 16;
        of.is_float = true;
        of.bits = 32;
        oh.bits = 16;
        write_image(path("8.tif"), img, o8);
        check(tag + " tiff 8", max_diff(img, read_image(path("8.tif"))), 0.5 / 255 + 1e-12);
        write_image(path("16.tif"), img, o16);
        check(tag + " tiff 16", max_diff(img, read_image(path("16.tif"))), 0.5 / 65535 + 1e-12);
        write_image(path("f.tif"), hdr, of);
        check(tag + " tiff float", max_diff(hdr, read_image(path("f.tif"))), 4e-6);
        write_image(path("8.png"), img, o8);
        check(tag + " png 8", max_diff(img, read_image(path("8.png"))), 0.5 / 255 + 1e-12);
        write_image(path("16.png"), img, o16);
        check(tag + " png 16", max_diff(img, read_image(path("16.png"))), 0.5 / 65535 + 1e-12);
        write_image(path("f.exr"), hdr, of);
        check(tag + " exr float", max_diff(hdr, read_image(path("f.exr"))), 4e-6);
        write_image(path("h.exr"), hdr, oh);
        check(tag + " exr half", max_diff(hdr, read_image(path("h.exr"))), 0.02);
        // JPEG is lossy: use a smooth image.
        Image smooth(37, 53, c);
        for (int y = 0; y < smooth.h; ++y)
            for (int x = 0; x < smooth.w; ++x)
                for (int k = 0; k < c; ++k) smooth.at(y, x, k) = 0.2 + 0.6 * (x + y + 5.0 * k) / 100.0;
        write_image(path("q.jpg"), smooth, o8);
        check(tag + " jpeg", max_diff(smooth, read_image(path("q.jpg"))), 0.03);
        ImageFileInfo info;
        read_image(path("16.tif"), &info);
        if (info.bits != 16 || info.channels != c || info.is_float) {
            std::printf("FAIL %s tiff info\n", tag.c_str());
            ++failures;
        }
    }
    fs::remove_all(dir);
    std::printf(failures ? "%d I/O check(s) FAILED\n" : "all I/O checks passed\n", failures);
    return failures ? 1 : 0;
}
