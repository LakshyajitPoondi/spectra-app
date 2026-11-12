// SPDX-License-Identifier: GPL-3.0-or-later
#include "spectra/io/image_io.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include <ImfChannelList.h>
#include <ImfFrameBuffer.h>
#include <ImfHeader.h>
#include <ImfInputFile.h>
#include <ImfOutputFile.h>
#include <jpeglib.h>
#include <png.h>
#include <tiffio.h>

#include "spectra/core/math.hpp"
#include "spectra/core/npy.hpp"

namespace spectra {

namespace {

std::string lower_ext(const std::string& path) {
    size_t p = path.find_last_of('.');
    std::string e = p == std::string::npos ? "" : path.substr(p + 1);
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return std::tolower(c); });
    return e;
}

inline uint32_t quantise(double v, uint32_t maxv) {
    if (!(v > 0.0)) return 0;
    if (v >= 1.0) return maxv;
    return static_cast<uint32_t>(v * maxv + 0.5);
}

// ---- TIFF -------------------------------------------------------------------

Image read_tiff(const std::string& path, ImageFileInfo* info) {
    TIFF* tif = TIFFOpen(path.c_str(), "r");
    if (!tif) throw std::runtime_error("cannot open TIFF " + path);
    uint32_t w = 0, h = 0;
    uint16_t spp = 1, bps = 8, fmt = SAMPLEFORMAT_UINT, planar = PLANARCONFIG_CONTIG, photo = 0;
    TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &w);
    TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &h);
    TIFFGetFieldDefaulted(tif, TIFFTAG_SAMPLESPERPIXEL, &spp);
    TIFFGetFieldDefaulted(tif, TIFFTAG_BITSPERSAMPLE, &bps);
    TIFFGetFieldDefaulted(tif, TIFFTAG_SAMPLEFORMAT, &fmt);
    TIFFGetFieldDefaulted(tif, TIFFTAG_PLANARCONFIG, &planar);
    TIFFGetField(tif, TIFFTAG_PHOTOMETRIC, &photo);
    bool is_float = fmt == SAMPLEFORMAT_IEEEFP;
    bool ok = planar == PLANARCONFIG_CONTIG && !TIFFIsTiled(tif) &&
              ((is_float && (bps == 32 || bps == 16)) || (!is_float && (bps == 8 || bps == 16))) &&
              (photo == PHOTOMETRIC_RGB || photo == PHOTOMETRIC_MINISBLACK);
    if (!ok) {
        // Anything unusual (tiles, palettes, YCbCr, CMYK): let libtiff convert to 8-bit RGBA.
        std::vector<uint32_t> rgba(static_cast<size_t>(w) * h);
        if (!TIFFReadRGBAImageOriented(tif, w, h, rgba.data(), ORIENTATION_TOPLEFT, 0)) {
            TIFFClose(tif);
            throw std::runtime_error("unsupported TIFF layout: " + path);
        }
        TIFFClose(tif);
        Image img(static_cast<int>(h), static_cast<int>(w), 3);
        for (size_t i = 0; i < rgba.size(); ++i) {
            img.v[i * 3] = TIFFGetR(rgba[i]) / 255.0;
            img.v[i * 3 + 1] = TIFFGetG(rgba[i]) / 255.0;
            img.v[i * 3 + 2] = TIFFGetB(rgba[i]) / 255.0;
        }
        if (info) *info = {"tiff", 8, false, 3};
        return img;
    }
    int c_out = spp >= 3 ? 3 : 1;
    Image img(static_cast<int>(h), static_cast<int>(w), c_out);
    std::vector<uint8_t> line(static_cast<size_t>(TIFFScanlineSize(tif)));
    for (uint32_t y = 0; y < h; ++y) {
        if (TIFFReadScanline(tif, line.data(), y) < 0) {
            TIFFClose(tif);
            throw std::runtime_error("error reading TIFF " + path);
        }
        double* out = img.px(static_cast<int>(y), 0);
        for (uint32_t x = 0; x < w; ++x)
            for (int k = 0; k < c_out; ++k) {
                size_t s = static_cast<size_t>(x) * spp + k;
                double v;
                if (is_float && bps == 32) {
                    float f;
                    std::memcpy(&f, &line[s * 4], 4);
                    v = f;
                } else if (is_float) {
                    uint16_t hv;
                    std::memcpy(&hv, &line[s * 2], 2);
                    v = half_to_double(hv);
                } else if (bps == 16) {
                    uint16_t u;
                    std::memcpy(&u, &line[s * 2], 2);
                    v = u / 65535.0;
                } else {
                    v = line[s] / 255.0;
                }
                out[static_cast<size_t>(x) * c_out + k] = v;
            }
    }
    TIFFClose(tif);
    if (info) *info = {"tiff", bps, is_float, spp};
    return img;
}

void write_tiff(const std::string& path, const Image& img, const WriteOptions& opt) {
    TIFF* tif = TIFFOpen(path.c_str(), "w");
    if (!tif) throw std::runtime_error("cannot write TIFF " + path);
    int bps = opt.is_float ? 32 : (opt.bits == 8 ? 8 : 16);
    TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, static_cast<uint32_t>(img.w));
    TIFFSetField(tif, TIFFTAG_IMAGELENGTH, static_cast<uint32_t>(img.h));
    TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, img.c);
    TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, bps);
    TIFFSetField(tif, TIFFTAG_SAMPLEFORMAT, opt.is_float ? SAMPLEFORMAT_IEEEFP : SAMPLEFORMAT_UINT);
    TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
    TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, img.c >= 3 ? PHOTOMETRIC_RGB : PHOTOMETRIC_MINISBLACK);
    TIFFSetField(tif, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT);
    TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_ADOBE_DEFLATE);
    TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, TIFFDefaultStripSize(tif, 0));
    size_t n = static_cast<size_t>(img.w) * img.c;
    std::vector<uint8_t> line(n * (bps / 8));
    for (int y = 0; y < img.h; ++y) {
        const double* in = img.px(y, 0);
        for (size_t i = 0; i < n; ++i) {
            if (opt.is_float) {
                float f = static_cast<float>(in[i]);
                std::memcpy(&line[i * 4], &f, 4);
            } else if (bps == 16) {
                uint16_t u = static_cast<uint16_t>(quantise(in[i], 65535));
                std::memcpy(&line[i * 2], &u, 2);
            } else {
                line[i] = static_cast<uint8_t>(quantise(in[i], 255));
            }
        }
        if (TIFFWriteScanline(tif, line.data(), static_cast<uint32_t>(y)) < 0) {
            TIFFClose(tif);
            throw std::runtime_error("error writing TIFF " + path);
        }
    }
    TIFFClose(tif);
}

// ---- PNG --------------------------------------------------------------------

Image read_png(const std::string& path, ImageFileInfo* info) {
    png_image im;
    std::memset(&im, 0, sizeof im);
    im.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_file(&im, path.c_str())) throw std::runtime_error("cannot open PNG " + path);
    bool sixteen = (im.format & PNG_FORMAT_FLAG_LINEAR) != 0;
    bool colour = (im.format & PNG_FORMAT_FLAG_COLOR) != 0;
    int c = colour ? 3 : 1;
    Image img(static_cast<int>(im.height), static_cast<int>(im.width), c);
    size_t n = static_cast<size_t>(im.width) * im.height * c;
    // The simplified API would linearise 16-bit data; read the stored code values instead.
    png_image_free(&im);
    FILE* fp = std::fopen(path.c_str(), "rb");
    if (!fp) throw std::runtime_error("cannot open PNG " + path);
    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    png_infop pinfo = png_create_info_struct(png);
    if (setjmp(png_jmpbuf(png))) {
        png_destroy_read_struct(&png, &pinfo, nullptr);
        std::fclose(fp);
        throw std::runtime_error("error reading PNG " + path);
    }
    png_init_io(png, fp);
    png_read_info(png, pinfo);
    int depth = png_get_bit_depth(png, pinfo), ctype = png_get_color_type(png, pinfo);
    if (ctype == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
    if (ctype == PNG_COLOR_TYPE_GRAY && depth < 8) png_set_expand_gray_1_2_4_to_8(png);
    if (ctype & PNG_COLOR_MASK_ALPHA) png_set_strip_alpha(png);
    if (png_get_valid(png, pinfo, PNG_INFO_tRNS)) png_set_strip_alpha(png);
    if (depth == 16) png_set_swap(png);  // to little-endian
    png_read_update_info(png, pinfo);
    size_t rowbytes = png_get_rowbytes(png, pinfo);
    std::vector<uint8_t> row(rowbytes);
    int bytes = sixteen || depth == 16 ? 2 : 1;
    for (uint32_t y = 0; y < im.height; ++y) {
        png_read_row(png, row.data(), nullptr);
        double* out = img.px(static_cast<int>(y), 0);
        for (size_t i = 0; i < static_cast<size_t>(im.width) * c; ++i) {
            if (bytes == 2) {
                uint16_t u;
                std::memcpy(&u, &row[i * 2], 2);
                out[i] = u / 65535.0;
            } else {
                out[i] = row[i] / 255.0;
            }
        }
    }
    png_destroy_read_struct(&png, &pinfo, nullptr);
    std::fclose(fp);
    (void)n;
    if (info) *info = {"png", bytes * 8, false, c};
    return img;
}

void write_png(const std::string& path, const Image& img, const WriteOptions& opt) {
    int bps = opt.bits == 8 ? 8 : 16;
    FILE* fp = std::fopen(path.c_str(), "wb");
    if (!fp) throw std::runtime_error("cannot write PNG " + path);
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    png_infop pinfo = png_create_info_struct(png);
    if (setjmp(png_jmpbuf(png))) {
        png_destroy_write_struct(&png, &pinfo);
        std::fclose(fp);
        throw std::runtime_error("error writing PNG " + path);
    }
    png_init_io(png, fp);
    png_set_IHDR(png, pinfo, static_cast<uint32_t>(img.w), static_cast<uint32_t>(img.h), bps,
                 img.c >= 3 ? PNG_COLOR_TYPE_RGB : PNG_COLOR_TYPE_GRAY, PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, pinfo);
    if (bps == 16) png_set_swap(png);
    size_t n = static_cast<size_t>(img.w) * img.c;
    std::vector<uint8_t> row(n * (bps / 8));
    for (int y = 0; y < img.h; ++y) {
        const double* in = img.px(y, 0);
        for (size_t i = 0; i < n; ++i) {
            if (bps == 16) {
                uint16_t u = static_cast<uint16_t>(quantise(in[i], 65535));
                std::memcpy(&row[i * 2], &u, 2);
            } else {
                row[i] = static_cast<uint8_t>(quantise(in[i], 255));
            }
        }
        png_write_row(png, row.data());
    }
    png_write_end(png, nullptr);
    png_destroy_write_struct(&png, &pinfo);
    std::fclose(fp);
}

// ---- JPEG -------------------------------------------------------------------

struct JpegError {
    jpeg_error_mgr pub;
    jmp_buf jmp;
};
void jpeg_fail(j_common_ptr cinfo) { longjmp(reinterpret_cast<JpegError*>(cinfo->err)->jmp, 1); }

Image read_jpeg(const std::string& path, ImageFileInfo* info) {
    FILE* fp = std::fopen(path.c_str(), "rb");
    if (!fp) throw std::runtime_error("cannot open JPEG " + path);
    jpeg_decompress_struct cinfo;
    JpegError err;
    cinfo.err = jpeg_std_error(&err.pub);
    err.pub.error_exit = jpeg_fail;
    if (setjmp(err.jmp)) {
        jpeg_destroy_decompress(&cinfo);
        std::fclose(fp);
        throw std::runtime_error("error reading JPEG " + path);
    }
    jpeg_create_decompress(&cinfo);
    jpeg_stdio_src(&cinfo, fp);
    jpeg_read_header(&cinfo, TRUE);
    cinfo.out_color_space = cinfo.num_components == 1 ? JCS_GRAYSCALE : JCS_RGB;
    jpeg_start_decompress(&cinfo);
    int c = cinfo.output_components;
    Image img(static_cast<int>(cinfo.output_height), static_cast<int>(cinfo.output_width), c);
    std::vector<uint8_t> row(static_cast<size_t>(cinfo.output_width) * c);
    while (cinfo.output_scanline < cinfo.output_height) {
        int y = static_cast<int>(cinfo.output_scanline);
        JSAMPROW r = row.data();
        jpeg_read_scanlines(&cinfo, &r, 1);
        double* out = img.px(y, 0);
        for (size_t i = 0; i < row.size(); ++i) out[i] = row[i] / 255.0;
    }
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    std::fclose(fp);
    if (info) *info = {"jpeg", 8, false, c};
    return img;
}

void write_jpeg(const std::string& path, const Image& img, const WriteOptions& opt) {
    FILE* fp = std::fopen(path.c_str(), "wb");
    if (!fp) throw std::runtime_error("cannot write JPEG " + path);
    jpeg_compress_struct cinfo;
    JpegError err;
    cinfo.err = jpeg_std_error(&err.pub);
    err.pub.error_exit = jpeg_fail;
    if (setjmp(err.jmp)) {
        jpeg_destroy_compress(&cinfo);
        std::fclose(fp);
        throw std::runtime_error("error writing JPEG " + path);
    }
    jpeg_create_compress(&cinfo);
    jpeg_stdio_dest(&cinfo, fp);
    cinfo.image_width = static_cast<JDIMENSION>(img.w);
    cinfo.image_height = static_cast<JDIMENSION>(img.h);
    cinfo.input_components = img.c >= 3 ? 3 : 1;
    cinfo.in_color_space = img.c >= 3 ? JCS_RGB : JCS_GRAYSCALE;
    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, opt.jpeg_quality, TRUE);
    jpeg_start_compress(&cinfo, TRUE);
    int c = cinfo.input_components;
    std::vector<uint8_t> row(static_cast<size_t>(img.w) * c);
    for (int y = 0; y < img.h; ++y) {
        const double* in = img.px(y, 0);
        for (int x = 0; x < img.w; ++x)
            for (int k = 0; k < c; ++k)
                row[static_cast<size_t>(x) * c + k] = static_cast<uint8_t>(quantise(in[static_cast<size_t>(x) * img.c + k], 255));
        JSAMPROW r = row.data();
        jpeg_write_scanlines(&cinfo, &r, 1);
    }
    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);
    std::fclose(fp);
}

// ---- OpenEXR ----------------------------------------------------------------

Image read_exr(const std::string& path, ImageFileInfo* info) {
    try {
        Imf::InputFile file(path.c_str());
        Imath::Box2i dw = file.header().dataWindow();
        int w = dw.max.x - dw.min.x + 1, h = dw.max.y - dw.min.y + 1;
        const Imf::ChannelList& ch = file.header().channels();
        bool rgb = ch.findChannel("R") && ch.findChannel("G") && ch.findChannel("B");
        const char* names_rgb[3] = {"R", "G", "B"};
        const char* names_y[1] = {"Y"};
        const char** names = rgb ? names_rgb : names_y;
        int c = rgb ? 3 : 1;
        if (!rgb && !ch.findChannel("Y")) throw std::runtime_error("EXR has neither RGB nor Y channels");
        std::vector<float> buf(static_cast<size_t>(w) * h * c);
        Imf::FrameBuffer fb;
        for (int k = 0; k < c; ++k) {
            char* base = reinterpret_cast<char*>(buf.data() + k) -
                         (static_cast<ptrdiff_t>(dw.min.x) + static_cast<ptrdiff_t>(dw.min.y) * w) * c * sizeof(float);
            fb.insert(names[k], Imf::Slice(Imf::FLOAT, base, sizeof(float) * c, sizeof(float) * c * w));
        }
        file.setFrameBuffer(fb);
        file.readPixels(dw.min.y, dw.max.y);
        Image img(h, w, c);
        for (size_t i = 0; i < buf.size(); ++i) img.v[i] = buf[i];
        if (info) {
            const Imf::Channel* r = ch.findChannel(names[0]);
            *info = {"exr", r && r->type == Imf::HALF ? 16 : 32, true, c};
        }
        return img;
    } catch (const std::exception& e) {
        throw std::runtime_error("cannot read EXR " + path + ": " + e.what());
    }
}

void write_exr(const std::string& path, const Image& img, const WriteOptions& opt) {
    try {
        int c = img.c >= 3 ? 3 : 1;
        Imf::PixelType type = opt.bits == 32 ? Imf::FLOAT : Imf::HALF;
        Imf::Header header(img.w, img.h);
        const char* names_rgb[3] = {"R", "G", "B"};
        const char* names_y[1] = {"Y"};
        const char** names = c == 3 ? names_rgb : names_y;
        for (int k = 0; k < c; ++k) header.channels().insert(names[k], Imf::Channel(type));
        std::vector<float> buf(static_cast<size_t>(img.w) * img.h * c);
        for (size_t i = 0; i < img.pixels(); ++i)
            for (int k = 0; k < c; ++k) buf[i * c + k] = static_cast<float>(img.v[i * img.c + k]);
        Imf::OutputFile file(path.c_str(), header);
        Imf::FrameBuffer fb;
        // The library converts FLOAT slices to the file's HALF channels itself only
        // when the types match, so convert here for half output.
        std::vector<Imath::half> hbuf;
        if (type == Imf::HALF) {
            hbuf.resize(buf.size());
            for (size_t i = 0; i < buf.size(); ++i) hbuf[i] = Imath::half(buf[i]);
            for (int k = 0; k < c; ++k)
                fb.insert(names[k], Imf::Slice(Imf::HALF, reinterpret_cast<char*>(hbuf.data() + k),
                                               sizeof(Imath::half) * c, sizeof(Imath::half) * c * img.w));
        } else {
            for (int k = 0; k < c; ++k)
                fb.insert(names[k], Imf::Slice(Imf::FLOAT, reinterpret_cast<char*>(buf.data() + k),
                                               sizeof(float) * c, sizeof(float) * c * img.w));
        }
        file.setFrameBuffer(fb);
        file.writePixels(img.h);
    } catch (const std::exception& e) {
        throw std::runtime_error("cannot write EXR " + path + ": " + e.what());
    }
}

}  // namespace

Image read_image(const std::string& path, ImageFileInfo* info) {
    std::string e = lower_ext(path);
    if (e == "tif" || e == "tiff") return read_tiff(path, info);
    if (e == "png") return read_png(path, info);
    if (e == "jpg" || e == "jpeg") return read_jpeg(path, info);
    if (e == "exr") return read_exr(path, info);
    if (e == "npy") {
        Image img = npy_load_image(path);
        if (info) *info = {"npy", 64, true, img.c};
        return img;
    }
    throw std::runtime_error("unsupported image format: " + path);
}

void write_image(const std::string& path, const Image& img, const WriteOptions& opt) {
    std::string e = lower_ext(path);
    if (e == "tif" || e == "tiff") return write_tiff(path, img, opt);
    if (e == "png") return write_png(path, img, opt);
    if (e == "jpg" || e == "jpeg") return write_jpeg(path, img, opt);
    if (e == "exr") return write_exr(path, img, opt);
    if (e == "npy") return npy_save_image(path, img);
    throw std::runtime_error("unsupported image format: " + path);
}

}  // namespace spectra
