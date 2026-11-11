// SPDX-License-Identifier: GPL-3.0-or-later
#include "spectra/core/npy.hpp"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace spectra {

NpyArray npy_load(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + path);
    char magic[6];
    f.read(magic, 6);
    if (std::memcmp(magic, "\x93NUMPY", 6) != 0) throw std::runtime_error("not a .npy file: " + path);
    uint8_t ver[2];
    f.read(reinterpret_cast<char*>(ver), 2);
    uint32_t hlen = 0;
    if (ver[0] == 1) {
        uint16_t l;
        f.read(reinterpret_cast<char*>(&l), 2);
        hlen = l;
    } else {
        f.read(reinterpret_cast<char*>(&hlen), 4);
    }
    std::string header(hlen, '\0');
    f.read(header.data(), hlen);
    auto find_val = [&](const std::string& key) {
        size_t p = header.find("'" + key + "'");
        if (p == std::string::npos) throw std::runtime_error("bad npy header: " + path);
        p = header.find(':', p);
        return p + 1;
    };
    size_t p = find_val("descr");
    size_t q1 = header.find('\'', p), q2 = header.find('\'', q1 + 1);
    std::string descr = header.substr(q1 + 1, q2 - q1 - 1);
    p = find_val("fortran_order");
    if (header.compare(header.find_first_not_of(' ', p), 4, "True") == 0)
        throw std::runtime_error("fortran-order npy is not supported: " + path);
    p = find_val("shape");
    size_t s1 = header.find('(', p), s2 = header.find(')', s1);
    std::string shp = header.substr(s1 + 1, s2 - s1 - 1);
    NpyArray a;
    std::stringstream ss(shp);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        size_t b = tok.find_first_not_of(' ');
        if (b == std::string::npos) continue;
        a.shape.push_back(std::stoull(tok.substr(b)));
    }
    size_t n = 1;
    for (size_t d : a.shape) n *= d;
    a.data.resize(n);
    if (descr == "<f8") {
        f.read(reinterpret_cast<char*>(a.data.data()), static_cast<std::streamsize>(n * 8));
    } else if (descr == "<f4") {
        std::vector<float> tmp(n);
        f.read(reinterpret_cast<char*>(tmp.data()), static_cast<std::streamsize>(n * 4));
        for (size_t i = 0; i < n; ++i) a.data[i] = tmp[i];
    } else {
        throw std::runtime_error("unsupported npy dtype " + descr + ": " + path);
    }
    if (!f) throw std::runtime_error("truncated npy file: " + path);
    return a;
}

void npy_save(const std::string& path, const std::vector<size_t>& shape,
              const std::vector<double>& data) {
    std::string h = "{'descr': '<f8', 'fortran_order': False, 'shape': (";
    for (size_t i = 0; i < shape.size(); ++i) {
        h += std::to_string(shape[i]);
        if (shape.size() == 1 || i + 1 < shape.size()) h += ",";
        if (i + 1 < shape.size()) h += " ";
    }
    h += "), }";
    size_t total = 10 + h.size() + 1;
    size_t pad = (64 - total % 64) % 64;
    h += std::string(pad, ' ');
    h += '\n';
    std::ofstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot write " + path);
    f.write("\x93NUMPY\x01\x00", 8);
    uint16_t l = static_cast<uint16_t>(h.size());
    f.write(reinterpret_cast<const char*>(&l), 2);
    f.write(h.data(), static_cast<std::streamsize>(h.size()));
    f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size() * 8));
}

Image npy_load_image(const std::string& path) {
    NpyArray a = npy_load(path);
    if (a.shape.size() != 2 && a.shape.size() != 3)
        throw std::runtime_error("expected a 2-D or 3-D array: " + path);
    Image img;
    img.h = static_cast<int>(a.shape[0]);
    img.w = static_cast<int>(a.shape[1]);
    img.c = a.shape.size() == 3 ? static_cast<int>(a.shape[2]) : 1;
    img.v = std::move(a.data);
    return img;
}

void npy_save_image(const std::string& path, const Image& img) {
    npy_save(path, {static_cast<size_t>(img.h), static_cast<size_t>(img.w), static_cast<size_t>(img.c)},
             img.v);
}

}  // namespace spectra
