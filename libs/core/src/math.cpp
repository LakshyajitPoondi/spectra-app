// SPDX-License-Identifier: GPL-3.0-or-later
#include "spectra/core/math.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

#include "spectra/core/image.hpp"
#include "spectra/core/parallel.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#else
#include <cstdio>
#include <cstring>
#endif

namespace spectra {

uint64_t peak_process_memory_bytes() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) return static_cast<uint64_t>(pmc.PeakWorkingSetSize);
    return 0;
#else
    std::FILE* f = std::fopen("/proc/self/status", "r");
    if (!f) return 0;
    char line[256];
    uint64_t kb = 0;
    while (std::fgets(line, sizeof line, f))
        if (std::strncmp(line, "VmHWM:", 6) == 0) {
            unsigned long long v = 0;
            std::sscanf(line + 6, "%llu", &v);
            kb = v;
        }
    std::fclose(f);
    return kb * 1024;
#endif
}

int& thread_count() {
    static int n = static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
    return n;
}

std::vector<double> extract_channel(const Image& img, int k) {
    std::vector<double> p(img.pixels());
    for (size_t i = 0; i < p.size(); ++i) p[i] = img.v[i * img.c + k];
    return p;
}

void insert_channel(Image& img, int k, const std::vector<double>& plane) {
    for (size_t i = 0; i < plane.size(); ++i) img.v[i * img.c + k] = plane[i];
}

Vec linspace(double a, double b, int n) {
    Vec y(static_cast<size_t>(n));
    if (n == 1) {
        y[0] = a;
        return y;
    }
    double step = (b - a) / (n - 1);
    for (int i = 0; i < n; ++i) y[i] = a + i * step;
    y[n - 1] = b;
    return y;
}

double ndtr(double a) {
    if (std::isnan(a)) return a;
    double x = a * 0.70710678118654752440;
    double z = std::fabs(x);
    if (z < 1.0) return 0.5 + 0.5 * std::erf(x);
    double y = 0.5 * std::erfc(z);
    return x > 0 ? 1.0 - y : y;
}

namespace {
constexpr int kLikelyInCache = 8;

int binary_search_with_guess(double key, const double* arr, int len, int guess) {
    int imin = 0, imax = len;
    if (key > arr[len - 1]) return len;
    if (key < arr[0]) return -1;
    if (len <= 4) {
        int i = 1;
        for (; i < len && key >= arr[i]; ++i) {
        }
        return i - 1;
    }
    if (guess > len - 3) guess = len - 3;
    if (guess < 1) guess = 1;
    if (key < arr[guess]) {
        if (key < arr[guess - 1]) {
            imax = guess - 1;
            if (guess > kLikelyInCache && key >= arr[guess - kLikelyInCache])
                imin = guess - kLikelyInCache;
        } else {
            return guess - 1;
        }
    } else {
        if (key < arr[guess + 1]) return guess;
        if (key < arr[guess + 2]) return guess + 1;
        imin = guess + 2;
        if (guess < len - kLikelyInCache - 1 && key < arr[guess + kLikelyInCache])
            imax = guess + kLikelyInCache;
    }
    while (imin < imax) {
        int imid = imin + ((imax - imin) >> 1);
        if (key >= arr[imid])
            imin = imid + 1;
        else
            imax = imid;
    }
    return imin - 1;
}
}  // namespace

double np_interp(double x, const double* xp, const double* fp, int n, int& guess) {
    if (std::isnan(x)) return x;
    int j = binary_search_with_guess(x, xp, n, guess);
    guess = j;
    if (j == -1) return fp[0];
    if (j == n) return fp[n - 1];
    if (j == n - 1) return fp[j];
    if (xp[j] == x) return fp[j];
    double slope = (fp[j + 1] - fp[j]) / (xp[j + 1] - xp[j]);
    double r = slope * (x - xp[j]) + fp[j];
    if (std::isnan(r)) {
        r = slope * (x - xp[j + 1]) + fp[j + 1];
        if (std::isnan(r) && fp[j] == fp[j + 1]) r = fp[j];
    }
    return r;
}

double np_interp(double x, const Vec& xp, const Vec& fp) {
    int g = 0;
    return np_interp(x, xp.data(), fp.data(), static_cast<int>(xp.size()), g);
}

Vec np_interp(const Vec& x, const Vec& xp, const Vec& fp) {
    Vec out(x.size());
    int g = 0;
    for (size_t i = 0; i < x.size(); ++i)
        out[i] = np_interp(x[i], xp.data(), fp.data(), static_cast<int>(xp.size()), g);
    return out;
}

double fast_interp1(double x, const double* xa, const double* ya, int n, int xs, int ys) {
    if (x <= xa[0]) return ya[0];
    if (x >= xa[static_cast<size_t>(n - 1) * xs]) return ya[static_cast<size_t>(n - 1) * ys];
    int lo = 0, hi = n;  // searchsorted(side='right')
    while (lo < hi) {
        int mid = lo + ((hi - lo) >> 1);
        if (x < xa[static_cast<size_t>(mid) * xs])
            hi = mid;
        else
            lo = mid + 1;
    }
    int low = lo - 1;
    if (low < 0) low = 0;
    if (low > n - 2) low = n - 2;
    double x0 = xa[static_cast<size_t>(low) * xs];
    double dx = xa[static_cast<size_t>(low + 1) * xs] - x0;
    double inv = dx != 0.0 ? 1.0 / dx : 0.0;
    double t = (x - x0) * inv;
    double y0 = ya[static_cast<size_t>(low) * ys];
    return y0 + t * (ya[static_cast<size_t>(low + 1) * ys] - y0);
}

double brentq(const std::function<double(double)>& f, double xa, double xb, double xtol,
              double rtol, int maxiter) {
    double xpre = xa, xcur = xb, xblk = 0., fblk = 0., spre = 0., scur = 0.;
    double fpre = f(xpre), fcur = f(xcur);
    if (fpre == 0) return xpre;
    if (fcur == 0) return xcur;
    if (std::signbit(fpre) == std::signbit(fcur))
        throw std::runtime_error("brentq: f(a) and f(b) must have different signs");
    for (int i = 0; i < maxiter; ++i) {
        if (fpre != 0 && fcur != 0 && (std::signbit(fpre) != std::signbit(fcur))) {
            xblk = xpre;
            fblk = fpre;
            spre = scur = xcur - xpre;
        }
        if (std::fabs(fblk) < std::fabs(fcur)) {
            xpre = xcur;
            xcur = xblk;
            xblk = xpre;
            fpre = fcur;
            fcur = fblk;
            fblk = fpre;
        }
        double delta = (xtol + rtol * std::fabs(xcur)) / 2;
        double sbis = (xblk - xcur) / 2;
        if (fcur == 0 || std::fabs(sbis) < delta) return xcur;
        if (std::fabs(spre) > delta && std::fabs(fcur) < std::fabs(fpre)) {
            double stry;
            if (xpre == xblk) {
                stry = -fcur * (xcur - xpre) / (fcur - fpre);
            } else {
                double dpre = (fpre - fcur) / (xpre - xcur);
                double dblk = (fblk - fcur) / (xblk - xcur);
                stry = -fcur * (fblk * dblk - fpre * dpre) / (dblk * dpre * (fblk - fpre));
            }
            if (2 * std::fabs(stry) < std::min(std::fabs(spre), 3 * std::fabs(sbis) - delta)) {
                spre = scur;
                scur = stry;
            } else {
                spre = sbis;
                scur = sbis;
            }
        } else {
            spre = sbis;
            scur = sbis;
        }
        xpre = xcur;
        fpre = fcur;
        if (std::fabs(scur) > delta)
            xcur += scur;
        else
            xcur += (sbis > 0 ? delta : -delta);
        fcur = f(xcur);
    }
    return xcur;
}

double half_to_double(uint16_t h) {
    int s = (h >> 15) & 1, e = (h >> 10) & 0x1f, m = h & 0x3ff;
    double v;
    if (e == 0)
        v = std::ldexp(static_cast<double>(m), -24);
    else if (e == 31)
        v = m ? std::numeric_limits<double>::quiet_NaN() : std::numeric_limits<double>::infinity();
    else
        v = std::ldexp(static_cast<double>(m + 1024), e - 25);
    return s ? -v : v;
}

Mat3 mat3_mul(const Mat3& a, const Mat3& b) {
    Mat3 r{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            double s = 0;
            for (int k = 0; k < 3; ++k) s += a[i * 3 + k] * b[k * 3 + j];
            r[i * 3 + j] = s;
        }
    return r;
}

Mat3 mat3_inv(const Mat3& m) {
    double a = m[0], b = m[1], c = m[2], d = m[3], e = m[4], f = m[5], g = m[6], h = m[7],
           i = m[8];
    double A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
    double det = a * A + b * B + c * C;
    Mat3 r{A, -(b * i - c * h), b * f - c * e, B, a * i - c * g, -(a * f - c * d),
           C, -(a * h - b * g), a * e - b * d};
    for (auto& x : r) x /= det;
    return r;
}

Vec3 mat3_apply(const Mat3& m, const Vec3& v) {
    return {m[0] * v[0] + m[1] * v[1] + m[2] * v[2], m[3] * v[0] + m[4] * v[1] + m[5] * v[2],
            m[6] * v[0] + m[7] * v[1] + m[8] * v[2]};
}

Mat3 mat3_identity() { return {1, 0, 0, 0, 1, 0, 0, 0, 1}; }

double round_half_even(double x) { return std::nearbyint(x); }

uint32_t crc32(const uint8_t* data, size_t n) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        init = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) c = table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

}  // namespace spectra
