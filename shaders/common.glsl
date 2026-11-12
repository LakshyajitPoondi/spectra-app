// SPDX-License-Identifier: GPL-3.0-or-later
// Shared declarations of the compute kernels. Image planes are storage
// buffers of 32-bit floats, interleaved H x W x C. All arithmetic is fp32.
layout(push_constant) uniform PushConstants {
    uint u[8];   // u[0] = width, u[1] = height, u[2] = channels; the rest is kernel specific
    float f[8];
} pc;

const float LOG2_10 = 3.3219280948873623;
const float PI = 3.14159265358979323846;

float pow10(float x) { return exp2(x * LOG2_10); }
float log10f(float x) { return log2(x) * 0.30102999566398120; }
float spow(float a, float p) { return sign(a) * pow(abs(a), p); }

// scipy.ndimage 'reflect' boundary: d c b a | a b c d | d c b a
int reflectIndex(int i, int n) {
    if (i >= 0 && i < n) return i;
    int period = 2 * n;
    i = i % period;
    if (i < 0) i += period;
    if (i >= n) i = period - 1 - i;
    return i;
}
