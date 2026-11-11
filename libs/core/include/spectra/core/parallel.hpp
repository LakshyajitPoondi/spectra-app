// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <functional>
#include <thread>
#include <vector>

namespace spectra {

// Number of worker threads used by parallel_for. Set to 1 for deterministic
// single-threaded runs.
int& thread_count();

// Run fn(begin, end) over [0, n) split into contiguous chunks, one per thread.
// Results must not depend on the chunking (all kernels write disjoint outputs).
template <class Fn>
void parallel_for(long n, Fn&& fn, long min_chunk = 1) {
    int nt = std::max(1, thread_count());
    if (n <= 0) return;
    long chunks = std::min<long>(nt, std::max<long>(1, n / std::max<long>(1, min_chunk)));
    if (chunks <= 1) {
        fn(0L, n);
        return;
    }
    std::vector<std::thread> th;
    th.reserve(static_cast<size_t>(chunks));
    long step = (n + chunks - 1) / chunks;
    for (long b = 0; b < n; b += step) {
        long e = std::min(n, b + step);
        th.emplace_back([&fn, b, e] { fn(b, e); });
    }
    for (auto& t : th) t.join();
}

}  // namespace spectra
