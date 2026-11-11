// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <vector>

namespace spectra {

using Vec = std::vector<double>;
using Vec3 = std::array<double, 3>;
using Mat3 = std::array<double, 9>;  // row-major

inline constexpr double kPi = 3.14159265358979323846;

// numpy.linspace(a, b, n) with endpoint.
Vec linspace(double a, double b, int n);

// Standard normal CDF, same branch structure as scipy.special.ndtr.
double ndtr(double x);

// numpy.interp for one query, including numpy's "guess" search so that results
// match NumPy even when xp is not monotonic (see RISKS R-07). `guess` carries
// the previous index between consecutive queries, as NumPy does.
double np_interp(double x, const double* xp, const double* fp, int n, int& guess);
double np_interp(double x, const Vec& xp, const Vec& fp);
Vec np_interp(const Vec& x, const Vec& xp, const Vec& fp);

// Port of spektrafilm.utils.fast_interp for one value: clamp to the end points,
// searchsorted(side='right'), linear interpolation with a strided table.
double fast_interp1(double x, const double* xa, const double* ya, int n, int xstride = 1,
                    int ystride = 1);

// scipy.optimize.brentq (same algorithm and default rtol).
double brentq(const std::function<double(double)>& f, double xa, double xb, double xtol = 2e-12,
              double rtol = 8.881784197001252e-16, int maxiter = 100);

// IEEE half -> double.
double half_to_double(uint16_t h);

Mat3 mat3_mul(const Mat3& a, const Mat3& b);
Mat3 mat3_inv(const Mat3& m);
Vec3 mat3_apply(const Mat3& m, const Vec3& v);
Mat3 mat3_identity();

// numpy.round (round half to even).
double round_half_even(double x);

uint32_t crc32(const uint8_t* data, size_t n);

}  // namespace spectra
