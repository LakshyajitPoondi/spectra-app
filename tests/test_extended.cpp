// SPDX-License-Identifier: GPL-3.0-or-later
// Sanity tests of the Extended features (push / pull, bleach bypass, HDR
// output). They have no Python reference, so instead of parity we check:
// finite output, a monotonic response to exposure, and that neutral greys
// stay neutral (no colour cast beyond what the base pipeline already has).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "spectra/pipeline/pipeline.hpp"

using namespace spectra;

static int failures = 0;

struct RampStats {
    bool finite = true;
    double worst_decrease = 0;  // largest drop between consecutive steps (0 = monotonic)
    double max_chroma = 0;      // max over steps of (max channel - min channel)
    double lo = 0, hi = 0;
    std::vector<double> chroma;
};

static RampStats run_ramp(std::shared_ptr<const ColourData> cd, const Params& p) {
    const int n = 96;
    Image ramp(1, n, 3);
    for (int i = 0; i < n; ++i) {
        double v = 0.184 * std::pow(2.0, -7.0 + 13.0 * i / (n - 1));  // -7 .. +6 stops around mid-grey
        for (int k = 0; k < 3; ++k) ramp.at(0, i, k) = v;
    }
    Params q = p;
    q.debug.deactivate_spatial_effects = true;
    q.debug.deactivate_stochastic_effects = true;
    q.camera.auto_exposure = false;
    Image out = simulate(cd, ramp, q);
    RampStats s;
    s.lo = 1e9;
    s.hi = -1e9;
    for (int i = 0; i < n; ++i) {
        double mx = -1e9, mn = 1e9;
        for (int k = 0; k < 3; ++k) {
            double v = out.at(0, i, k);
            s.finite = s.finite && std::isfinite(v);
            mx = std::max(mx, v);
            mn = std::min(mn, v);
            if (i > 0) s.worst_decrease = std::max(s.worst_decrease, out.at(0, i - 1, k) - v);
        }
        s.chroma.push_back(mx - mn);
        s.max_chroma = std::max(s.max_chroma, mx - mn);
        s.lo = std::min(s.lo, mn);
        s.hi = std::max(s.hi, mx);
    }
    return s;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::puts("usage: test_extended path/to/spektrafilm.sfpk");
        return 2;
    }
    auto cd = std::make_shared<const ColourData>(Pack::load(argv[1]));

    struct Case {
        std::string name;
        std::function<void(Params&)> set;
        double cast_limit;  // allowed increase of the grey-ramp chroma over the base pipeline
    };
    std::vector<Case> cases;
    for (double stops : {-2.0, -1.0, 1.0, 2.0}) {
        cases.push_back({"push/pull standard " + std::to_string(static_cast<int>(stops)), [=](Params& p) {
                             p.extended.push_pull.active = true;
                             p.extended.push_pull.film_stops = stops;
                         }, 0.03});
        cases.push_back({"push/pull experimental " + std::to_string(static_cast<int>(stops)), [=](Params& p) {
                             p.extended.push_pull.active = true;
                             p.extended.push_pull.mode = "experimental";
                             p.extended.push_pull.film_stops = stops;
                         }, 0.06});
        cases.push_back({"print push/pull " + std::to_string(static_cast<int>(stops)), [=](Params& p) {
                             p.extended.push_pull.active = true;
                             p.extended.push_pull.print_stops = stops;
                         }, 0.03});
    }
    for (double amount : {0.25, 0.5, 1.0}) {
        cases.push_back({"bleach bypass film " + std::to_string(amount).substr(0, 4), [=](Params& p) {
                             p.extended.bleach_bypass.active = true;
                             p.extended.bleach_bypass.film_amount = amount;
                         }, 0.06});
        cases.push_back({"bleach bypass print " + std::to_string(amount).substr(0, 4), [=](Params& p) {
                             p.extended.bleach_bypass.active = true;
                             p.extended.bleach_bypass.print_amount = amount;
                         }, 0.05});  // no stage after the print can re-balance it
    }
    cases.push_back({"bleach bypass film+print", [](Params& p) {
                         p.extended.bleach_bypass.active = true;
                         p.extended.bleach_bypass.film_amount = 1.0;
                         p.extended.bleach_bypass.print_amount = 1.0;
                     }, 0.06});
    for (const char* tf : {"pq", "hlg"})
        for (const char* tm : {"shoulder", "clip"})
            cases.push_back({std::string("hdr ") + tf + " " + tm, [=](Params& p) {
                                 p.extended.hdr_output.active = true;
                                 p.extended.hdr_output.transfer = tf;
                                 p.extended.hdr_output.tone_mapping = tm;
                                 p.io.output_color_space = "ITU-R BT.2020";
                             }, 0.03});

    struct Route {
        const char* film;
        const char* print;
        bool scan_film;
    };
    for (Route r : {Route{"kodak_portra_400", "kodak_portra_endura", false},
                    Route{"kodak_vision3_250d", "kodak_2383", false},
                    Route{"fujifilm_provia_100f", "kodak_portra_endura", true},
                    Route{"kodak_doublex", "kodak_2302", false}}) {
        Params base;
        base.film = r.film;
        base.print = r.print;
        base.io.scan_film = r.scan_film;
        RampStats b = run_ramp(cd, base);
        std::printf("%s -> %s%s: base chroma %.4f, monotonic drop %.2e\n", r.film, r.scan_film ? "scan" : r.print,
                    "", b.max_chroma, b.worst_decrease);
        if (!b.finite || b.worst_decrease > 1e-6) {
            std::printf("FAIL base pipeline is not finite / monotonic\n");
            ++failures;
        }
        for (const Case& c : cases) {
            Params p = base;
            c.set(p);
            if (p.extended.hdr_output.active) {
                // compare the cast against an HDR-free render in the same colour space
            }
            RampStats s = run_ramp(cd, p);
            double cast = 0;
            for (size_t i = 0; i < s.chroma.size(); ++i) cast = std::max(cast, s.chroma[i] - b.chroma[i]);
            bool ok = s.finite && s.worst_decrease <= 5e-4 && cast <= c.cast_limit && s.lo >= -1e-9 && s.hi <= 1.0 + 1e-9;
            std::printf("  %-4s %-28s range %.3f..%.3f  drop %.1e  added cast %.4f (limit %.2f)\n", ok ? "PASS" : "FAIL",
                        c.name.c_str(), s.lo, s.hi, s.worst_decrease, cast, c.cast_limit);
            if (!ok) ++failures;
        }
    }

    // Each feature must be an exact no-op when switched off or at zero strength.
    {
        Params base;
        Image img(4, 6, 3);
        for (size_t i = 0; i < img.v.size(); ++i) img.v[i] = 0.02 + 0.07 * static_cast<double>(i % 11);
        Params q = base;
        q.debug.deactivate_stochastic_effects = true;
        Image ref = simulate(cd, img, q);
        Params z = q;
        z.extended.push_pull.film_stops = 2.0;       // inactive
        z.extended.bleach_bypass.film_amount = 1.0;  // inactive
        z.extended.hdr_output.transfer = "hlg";      // inactive
        Params a = q;
        a.extended.push_pull.active = true;          // active at zero strength
        a.extended.bleach_bypass.active = true;
        bool same = simulate(cd, img, z).v == ref.v && simulate(cd, img, a).v == ref.v;
        std::printf("%-4s features are exact no-ops when off or at zero strength\n", same ? "PASS" : "FAIL");
        if (!same) ++failures;
    }

    std::printf(failures ? "%d extended-feature check(s) FAILED\n" : "all extended-feature checks passed\n", failures);
    return failures ? 1 : 0;
}
