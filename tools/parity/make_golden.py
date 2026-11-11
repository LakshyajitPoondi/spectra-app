# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate parity goldens from the pinned Python reference (spektrafilm dev).

Writes one directory per suite under tests/golden/:
    <suite>/cases.json      list of cases for `spectra-cli parity`
    <suite>/inputs/*.npy    input arrays (float64)
    <suite>/golden/*.npy    reference outputs (float64)

Each pipeline case is run as
    params = init_params(film, print); apply(pre); digest_params(); apply(post)
    Simulator(params).process(image)
which is exactly what the C++ runner mirrors (digest in C++, then the same
overrides). Run with the virtualenv described in tools/exporter/README.md:

    python tools/parity/make_golden.py [--suite name ...]
"""
from __future__ import annotations

import argparse
import dataclasses
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

import numpy as np

os.environ.setdefault("NUMBA_NUM_THREADS", "4")

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "tests" / "golden"
PINNED_COMMIT = "6cd00c8d4f30b5b550f50f4bbd3753c9f2a48507"

FILMS_COLOR_NEG = [
    "kodak_portra_160", "kodak_portra_400", "kodak_portra_800", "kodak_portra_800_push1",
    "kodak_portra_800_push2", "kodak_gold_200", "kodak_ultramax_400", "kodak_ektar_100",
    "fujifilm_pro_400h", "fujifilm_c200", "fujifilm_xtra_400", "kodak_vision3_50d",
    "kodak_vision3_250d", "kodak_verita_200d", "kodak_vision3_200t", "kodak_vision3_500t",
]
FILMS_COLOR_POS = ["kodak_ektachrome_100", "kodak_kodachrome_64", "fujifilm_velvia_100",
                   "fujifilm_provia_100f"]
FILMS_BW = ["kodak_doublex", "kodak_trix"]
FILMS = FILMS_COLOR_NEG + FILMS_COLOR_POS + FILMS_BW
PRINTS_COLOR = ["kodak_ultra_endura", "kodak_endura_premier", "kodak_ektacolor_edge",
                "kodak_supra_endura", "kodak_portra_endura", "fujifilm_crystal_archive_typeii",
                "kodak_2383", "kodak_2393"]
PRINTS = PRINTS_COLOR + ["kodak_2302"]

# Deterministic "LUT-like" configuration, but through the normal digest path.
NONSPATIAL = {"debug": {"deactivate_spatial_effects": True, "deactivate_stochastic_effects": True}}
NO_STOCHASTIC = {"debug": {"deactivate_stochastic_effects": True}}


# ----------------------------------------------------------------------------
# helpers


def f32(a):
    """Round to float32 and return as float64 (inputs are stored as float32)."""
    return np.asarray(a, dtype=np.float32).astype(np.float64)


def apply_patch(obj, patch):
    """Apply a nested dict to a dataclass tree; returns the (possibly new) object."""
    changes = {}
    for key, value in patch.items():
        if not hasattr(obj, key):
            raise AttributeError(f"{type(obj).__name__} has no field {key!r}")
        current = getattr(obj, key)
        if isinstance(value, dict) and dataclasses.is_dataclass(current):
            changes[key] = apply_patch(current, value)
        elif isinstance(value, list):
            changes[key] = tuple(value)
        else:
            changes[key] = value
    params = getattr(type(obj), "__dataclass_params__", None)
    if params is not None and params.frozen:
        return dataclasses.replace(obj, **changes)
    for key, value in changes.items():
        setattr(obj, key, value)
    return obj


def merge(*patches):
    out = {}
    for p in patches:
        for k, v in p.items():
            if isinstance(v, dict) and isinstance(out.get(k), dict):
                out[k] = merge(out[k], v)
            else:
                out[k] = json.loads(json.dumps(v))
    return out


def build_params(case):
    from spektrafilm.runtime.params_builder import digest_params, init_params
    params = init_params(case.get("film", "kodak_portra_400"), case.get("print", "kodak_portra_endura"))
    if "pre" in case:
        apply_patch(params, case["pre"])
    params = digest_params(params)
    if "post" in case:
        apply_patch(params, case["post"])
    return params


def params_to_jsonable(params):
    def conv(o):
        if dataclasses.is_dataclass(o):
            return {f.name: conv(getattr(o, f.name)) for f in dataclasses.fields(o)}
        if isinstance(o, (tuple, list)):
            return [conv(v) for v in o]
        if isinstance(o, (np.floating, np.integer)):
            return o.item()
        return o
    out = {}
    for f in dataclasses.fields(params):
        if f.name in ("film", "print"):
            out[f.name] = getattr(params, f.name).info.stock
        else:
            out[f.name] = conv(getattr(params, f.name))
    return out


class Suite:
    def __init__(self, name):
        self.name = name
        self.dir = OUT / name
        if self.dir.exists():
            shutil.rmtree(self.dir)
        (self.dir / "inputs").mkdir(parents=True)
        (self.dir / "golden").mkdir()
        self.cases = []
        self._inputs = {}

    def input(self, name, array):
        """Store an input array. Inputs must be float32-representable (see f32())
        so they can be stored at half the size without changing their values."""
        if name not in self._inputs:
            path = f"inputs/{name}.npy"
            a32 = np.ascontiguousarray(array, dtype=np.float32)
            if not np.array_equal(a32.astype(np.float64), np.asarray(array, dtype=np.float64)):
                raise ValueError(f"input {name} is not float32-representable; wrap it in f32()")
            np.save(self.dir / path, a32)
            self._inputs[name] = path
        return self._inputs[name]

    def add(self, case, golden):
        cid = case["id"]
        if any(c["id"] == cid for c in self.cases):
            raise KeyError(f"duplicate case id {cid}")
        if isinstance(golden, (dict, list)) and case.get("kind") == "digest":
            path = f"golden/{cid}.json"
            (self.dir / path).write_text(json.dumps(golden, indent=1))
        else:
            path = f"golden/{cid}.npy"
            arr = np.ascontiguousarray(np.asarray(golden, dtype=np.float64))
            if arr.ndim == 0:
                arr = arr.reshape(1)
            np.save(self.dir / path, arr)
        case["golden"] = path
        self.cases.append(case)

    def pipeline(self, cid, image_name, image, *, film="kodak_portra_400", print_="kodak_portra_endura",
                 pre=None, post=None, inject=None, collect=None, atol=1e-9, rtol=1e-9, seed=None):
        from spektrafilm.runtime.process import Simulator
        case = {"id": cid, "kind": "pipeline", "film": film, "print": print_,
                "input": self.input(image_name, image), "atol": atol, "rtol": rtol}
        if pre:
            case["pre"] = pre
        if post:
            case["post"] = post
        if inject:
            case["inject"] = inject
        if collect:
            case["collect"] = collect
        params = build_params(case)
        sim = Simulator(params)
        if seed is not None:
            np.random.seed(seed)
        out = sim._pipeline.process(np.array(image, dtype=np.float64), inject=inject, collect=collect)
        self.add(case, out)
        return out

    def unit(self, cid, unit, golden, *, atol=1e-12, rtol=1e-9, **fields):
        case = {"id": cid, "kind": "unit", "unit": unit, "atol": atol, "rtol": rtol}
        case.update(fields)
        self.add(case, golden)

    def finish(self):
        doc = {"suite": self.name, "reference_commit": PINNED_COMMIT, "cases": self.cases}
        (self.dir / "cases.json").write_text(json.dumps(doc, indent=1))
        size = sum(f.stat().st_size for f in self.dir.rglob("*") if f.is_file())
        print(f"suite {self.name}: {len(self.cases)} cases, {size / 1e6:.2f} MB")


# ----------------------------------------------------------------------------
# test images (linear ProPhoto RGB unless noted)


def patches_image():
    """12 x 16 synthetic chart: HDR grey ramp, saturated ramps, random colours."""
    rng = np.random.default_rng(1234)
    img = np.zeros((12, 16, 3))
    grey = np.concatenate([[0.0], np.geomspace(0.002, 6.0, 15)])
    img[0] = grey[:, None]
    img[1] = (0.184 * 2.0 ** np.linspace(-6, 5, 16))[:, None]
    prim = np.array([[1, 0, 0], [0, 1, 0], [0, 0, 1], [1, 1, 0], [0, 1, 1], [1, 0, 1], [1, 0.5, 0], [0.3, 0.6, 1]], float)
    for i, level in enumerate([0.05, 0.4]):
        img[2 + i, :8] = prim * level
        img[2 + i, 8:] = prim * level * 0.5 + level * 0.25
    img[4:8] = rng.uniform(0.0, 1.0, (4, 16, 3)) ** 2.2
    img[8:10] = rng.uniform(0.0, 0.3, (2, 16, 3))
    img[10] = rng.uniform(0.0, 4.0, (16, 3))
    img[11] = np.clip(rng.normal(0.18, 0.1, (16, 3)), 0.0, None)
    return f32(img)


def portrait_full():
    from spektrafilm.utils.io import load_image_oiio
    import spektrafilm
    path = Path(spektrafilm.__file__).resolve().parents[2] / "img" / "test" / \
        "portrait_leaves_32bit_linear_prophoto_rgb.tif"
    return np.asarray(load_image_oiio(str(path)), dtype=np.float64)


def block_reduce(img, f):
    h, w = (img.shape[0] // f) * f, (img.shape[1] // f) * f
    return f32(img[:h, :w].reshape(h // f, f, w // f, f, 3).mean(axis=(1, 3)))


def highlights_image(h=96, w=144):
    """Dark scene with small very bright sources: exercises halation and diffusion."""
    rng = np.random.default_rng(7)
    yy, xx = np.mgrid[0:h, 0:w]
    img = 0.02 + 0.05 * rng.uniform(size=(h, w, 3))
    img += 0.15 * (xx / w)[..., None] * np.array([1.0, 0.8, 0.6])
    for (cy, cx, r, col) in [(30, 40, 3, (40, 36, 30)), (60, 100, 1.5, (200, 180, 120)),
                             (20, 120, 5, (6, 7, 9)), (80, 20, 0.8, (500, 500, 500))]:
        m = ((yy - cy) ** 2 + (xx - cx) ** 2) <= r * r
        img[m] = np.array(col, float)
    return f32(img)


# ----------------------------------------------------------------------------
# suites


def suite_t1_units():
    import colour
    from spektrafilm.model.illuminants import standard_illuminant
    from spektrafilm.utils.spectral_upsampling import _illuminant_to_xy
    from spektrafilm.utils.fast_gaussian_filter import fast_gaussian_filter, fast_exponential_filter
    from spektrafilm.utils.autoexposure import measure_raw_autoexposure_ev
    from spektrafilm.utils.numba_boost_hightlights import boost_highlights
    from spektrafilm.utils.crop_resize import crop_image
    from spektrafilm.runtime.services.resize import _compute_small_preview
    from spektrafilm.model import diffusion as dif
    from spektrafilm.model.grain import _layer_grain_params
    from spektrafilm.model import couplers as cpl
    from spektrafilm.runtime.params_schema import DiffusionFilterParams, HalationParams
    from spektrafilm.runtime.process import Simulator

    s = Suite("t1_units")
    rng = np.random.default_rng(99)

    for name in ["D50", "D55", "D65", "T", "K75P", "TH-KG3", "TH-KG3-L", "BB3200", "BB3400", "BB6504"]:
        s.unit(f"illuminant_{name}", "illuminant", standard_illuminant(name), name=name)
        s.unit(f"illuminant_xy_{name}", "illuminant_xy", _illuminant_to_xy(name), name=name)

    x = f32(np.concatenate([np.linspace(-0.25, 1.5, 351), [0.0031308, 0.04045, 0.018, 0.0812, 1e-6]]))
    xin = s.input("cctf_x", x)
    for space in ["sRGB", "DCI-P3", "Display P3", "Adobe RGB (1998)", "ITU-R BT.2020", "ProPhoto RGB", "ACES2065-1"]:
        cs = colour.RGB_COLOURSPACES[space]
        key = space.replace(" ", "_").replace("(", "").replace(")", "")
        with np.errstate(all="ignore"):
            s.unit(f"cctf_decode_{key}", "cctf", cs.cctf_decoding(x), input=xin, space=space, direction="decode")
            s.unit(f"cctf_encode_{key}", "cctf", cs.cctf_encoding(x), input=xin, space=space, direction="encode")
        eye = np.eye(3)
        for illum, cat in [("D55", "CAT16"), ("T", "CAT16"), ("D50", "CAT02"), ("K75P", "CAT02")]:
            xy = _illuminant_to_xy(illum)
            m = colour.RGB_to_XYZ(eye, space, illuminant=xy, chromatic_adaptation_transform=cat).T
            s.unit(f"rgb_to_xyz_{key}_{illum}_{cat}", "rgb_to_xyz_matrix", m, space=space, illuminant=illum, cat=cat)
            m = colour.XYZ_to_RGB(eye, space, illuminant=xy, chromatic_adaptation_transform=cat).T
            s.unit(f"xyz_to_rgb_{key}_{illum}_{cat}", "xyz_to_rgb_matrix", m, space=space, illuminant=illum, cat=cat)

    img = f32(rng.uniform(0, 2, (37, 53, 3)) ** 3)
    iin = s.input("noise_37x53", img)
    for sig in [0.3, 0.7, 1.0, 2.5, 2.999, 3.0, 5.5, 12.3, 40.0]:
        s.unit(f"gaussian_sigma_{sig}", "gaussian_filter", fast_gaussian_filter(img.copy(), sig), input=iin,
               sigma=[sig], rtol=1e-9, atol=1e-11)
    sig3 = [0.9, 3.5, 7.25]
    s.unit("gaussian_per_channel", "gaussian_filter", fast_gaussian_filter(img.copy(), np.array(sig3)), input=iin,
           sigma=sig3, atol=1e-11)
    for dec in [[0.4], [2.0], [6.0], [1.5, 3.0, 9.0]]:
        d = dec[0] if len(dec) == 1 else np.array(dec)
        s.unit(f"exponential_{'_'.join(str(v) for v in dec)}", "exponential_filter",
               fast_exponential_filter(img.copy(), d), input=iin, decay=dec, atol=1e-11)

    for (h, w) in [(300, 451), (517, 260), (1030, 64), (257, 256), (200, 100)]:
        big = f32(rng.uniform(0, 1, (h, w, 3)) ** 2)
        bin_ = s.input(f"noise_{h}x{w}", big)
        s.unit(f"metering_preview_{h}x{w}", "metering_preview", _compute_small_preview(big), input=bin_, atol=1e-12)

    raw = f32(rng.uniform(0, 3, (64, 97, 3)) ** 2)
    rin = s.input("raw_64x97", raw)
    raw1 = f32(rng.uniform(0, 3, (41, 23, 1)))
    r1in = s.input("raw_41x23x1", raw1)
    for method in ["center_weighted", "matrix", "multi_zone", "partial", "highlight_weighted", "median", "average", "bogus"]:
        s.unit(f"autoexposure_{method}", "autoexposure_ev", measure_raw_autoexposure_ev(raw, method=method),
               input=rin, method=method)
        s.unit(f"autoexposure_bw_{method}", "autoexposure_ev", measure_raw_autoexposure_ev(raw1, method=method),
               input=r1in, method=method)
    tiny = f32(np.full((1, 1, 3), 0.7))
    tin = s.input("raw_1x1", tiny)
    for method in ["center_weighted", "matrix", "multi_zone", "partial", "highlight_weighted", "median", "average"]:
        s.unit(f"autoexposure_1x1_{method}", "autoexposure_ev", measure_raw_autoexposure_ev(tiny, method=method),
               input=tin, method=method)

    hi = highlights_image(40, 60)
    hin = s.input("highlights_40x60", hi)
    for (ev, rg, pr) in [(2.0, 0.3, 4.0), (6.0, 0.8, 1.0), (10.0, 0.0, 0.0), (1.0, 1.0, 3.0)]:
        s.unit(f"boost_{ev}_{rg}_{pr}", "boost_highlights", boost_highlights(hi.copy(), ev, rg, pr), input=hin,
               boost_ev=ev, boost_range=rg, protect_ev=pr, rtol=1e-10)

    for fam in dif.DIFFUSION_FILTER_FAMILIES:
        for extra in [{}, {"halo_warmth": -0.8, "core_intensity": 1.4, "bloom_size": 0.6, "spatial_scale": 1.7}]:
            dp = DiffusionFilterParams(active=True, filter_family=fam, strength=1.0, **extra)
            patch = {"active": True, "filter_family": fam, "strength": 1.0, **extra}
            tag = fam + ("_tuned" if extra else "")
            overrides = dif._overrides_from_params(dp)
            psf = dif.diffusion_filter_psf((41, 41), family=fam, spatial_scale=dp.spatial_scale, pixel_size_um=12.0,
                                           halo_warmth=dp.halo_warmth, overrides=overrides)
            s.unit(f"diffusion_psf_{tag}", "diffusion_psf", psf, diffusion=patch, size=41, pixel_size_um=12.0,
                   atol=1e-15)
            out = dif.apply_diffusion_filter_um(hi.copy(), dp, 30.0)
            # The reference convolves in float32; compare at single-precision accuracy.
            s.unit(f"diffusion_filter_{tag}", "diffusion_filter", out, diffusion=patch, input=hin,
                   pixel_size_um=30.0, atol=2e-4, rtol=2e-5)

    hp = HalationParams()
    s.unit("halation_default", "halation", dif.apply_halation_um(hi.copy(), hp, 6.0), input=hin, pixel_size_um=6.0,
           atol=1e-11)
    hp2 = HalationParams(scatter_amount=0.6, scatter_spatial_scale=2.0, halation_amount=1.5,
                         halation_strength=(0.3, 0.1, 0.015), halation_n_bounces=5, halation_bounce_decay=0.7,
                         halation_renormalize=False, halation_first_sigma_um=(65.0, 50.0, 40.0))
    s.unit("halation_tuned", "halation", dif.apply_halation_um(hi.copy(), hp2, 9.0), input=hin, pixel_size_um=9.0,
           halation={"scatter_amount": 0.6, "scatter_spatial_scale": 2.0, "halation_amount": 1.5,
                     "halation_strength": [0.3, 0.1, 0.015], "halation_n_bounces": 5, "halation_bounce_decay": 0.7,
                     "halation_renormalize": False, "halation_first_sigma_um": [65.0, 50.0, 40.0]}, atol=1e-11)

    for (center, size) in [((0.5, 0.5), (0.1, 0.1)), ((0.2, 0.8), (0.5, 0.3)), ((0.95, 0.05), (0.4, 0.4)),
                           ((0.31, 0.47), (0.33, 0.21))]:
        s.unit(f"crop_{center[0]}_{center[1]}_{size[0]}_{size[1]}", "crop", crop_image(img, center, size), input=iin,
               io={"crop_center": list(center), "crop_size": list(size)})

    # --- quantities that need a built pipeline
    def built(case):
        params = build_params(case)
        return params, Simulator(params)._pipeline

    tc_cases = [
        ("portra400", {"film": "kodak_portra_400"}),
        ("vision3_500t", {"film": "kodak_vision3_500t"}),
        ("velvia", {"film": "fujifilm_velvia_100"}),
        ("doublex", {"film": "kodak_doublex"}),
        ("portra400_no_window", {"film": "kodak_portra_400", "pre": {"settings": {"apply_hanatos2025_adaptation_window": False}}}),
        ("portra400_surface", {"film": "kodak_portra_400", "pre": {"settings": {"apply_hanatos2025_adaptation_surface": True}}}),
        ("portra400_blur", {"film": "kodak_portra_400", "pre": {"settings": {"spectral_gaussian_blur": 2.5}}}),
        ("portra400_gamut_off", {"film": "kodak_portra_400", "pre": {"io": {"input_gamut_compress": {"active": False}}}}),
        ("portra400_gamut_oklch", {"film": "kodak_portra_400", "pre": {"io": {"input_gamut_compress": {"algorithm": "oklch"}}}}),
        ("portra400_gamut_knee", {"film": "kodak_portra_400", "pre": {"io": {"input_gamut_compress": {"knee": [0.6, 1.1, 2.0]}}}}),
        ("doublex_filter_r1", {"film": "kodak_doublex", "pre": {"camera": {"color_filter": "hoya_r1"}}}),
        ("gold200_filter_y2", {"film": "kodak_gold_200", "pre": {"camera": {"color_filter": "hoya_y2"}}}),
    ]
    for tag, case in tc_cases:
        params, pipe = built(case)
        sens = np.nan_to_num(10 ** pipe.film.data.log_sensitivity)
        from spektrafilm.model.color_filters import color_filter_transmittance
        t = color_filter_transmittance(params.camera.color_filter)
        if t is not None:
            sens = sens * t[:, None]
        lut = pipe._lut_service.get_filming_tc_lut(sens)
        # oklch remap: tiny differences in the in-locus bisection are amplified near the locus.
        tol = dict(atol=1e-6, rtol=1e-6) if "oklch" in tag else dict(atol=1e-12, rtol=1e-9)
        s.unit(f"tc_lut_{tag}", "tc_lut", lut[::3, ::3], subsample=3, **case, **tol)
        s.unit(f"midgray_{tag}", "density_spectral_midgray",
               np.asarray(pipe._enlarger_service.density_spectral_midgray).reshape(-1), **case,
               atol=1e-6 if "oklch" in tag else 1e-10)

    chem_cases = [
        ("identity", {}),
        ("inactive", {"active": False}),
        ("gamma", {"gamma_factor": 1.3}),
        ("fast_slow", {"gamma_factor_fast": 0.8, "gamma_factor_slow": 1.25}),
        ("rgb", {"gamma_factor_red": 1.1, "gamma_factor_green": 0.9, "gamma_factor_blue": 1.2}),
        ("exhaustion", {"developer_exhaustion": 0.6}),
        ("all", {"gamma_factor": 0.85, "gamma_factor_fast": 1.2, "gamma_factor_slow": 0.9, "gamma_factor_red": 1.05,
                 "developer_exhaustion": 1.0}),
    ]
    for film in ["kodak_portra_400", "fujifilm_provia_100f", "kodak_doublex", "kodak_trix"]:
        for tag, chem in chem_cases:
            if film in ("kodak_doublex", "kodak_trix") and tag == "rgb":
                continue
            case = {"film": film, "pre": {"film_render": {"chemistry": chem}}}
            params, pipe = built(case)
            s.unit(f"film_curves_{film}_{tag}", "film_curves", pipe.film.data.density_curves, **case, atol=1e-11)
            s.unit(f"film_curves_layers_{film}_{tag}", "film_curves_layers", pipe.film.data.density_curves_layers,
                   **case, atol=1e-11)
    for dt in [4.0, 5.2, 9.0, 30.0]:
        case = {"film": "kodak_doublex", "pre": {"film_render": {"chemistry": {"development_time": dt}}}}
        params, pipe = built(case)
        s.unit(f"film_curves_doublex_dt{dt}", "film_curves", pipe.film.data.density_curves, **case, atol=1e-11)
    from spektrafilm.utils.morph_curves import apply_print_curves_morph
    for print_ in ["kodak_portra_endura", "kodak_2383", "kodak_2302"]:
        for tag, chem in chem_cases:
            if print_ == "kodak_2302" and tag == "rgb":
                continue
            case = {"print": print_, "pre": {"print_render": {"chemistry": chem}}}
            params, pipe = built(case)
            cv = apply_print_curves_morph(pipe.print.data.log_exposure, pipe.print.data.density_curves_model,
                                          params.print_render.chemistry, profile_type=pipe.print.info.type)
            s.unit(f"print_curves_{print_}_{tag}", "print_curves_morphed", cv, **case, atol=1e-11)

    for film in FILMS:
        case = {"film": film}
        params, pipe = built(case)
        curves = np.asarray(pipe.film.data.density_curves)
        norm = curves - np.nanmin(curves, axis=0)
        layers = pipe.film.data.density_curves_layers
        g = params.film_render.grain
        n_sub = layers.shape[1]
        _, _, _, npp = _layer_grain_params(np.nanmax(layers, axis=0), g.density_min, g.rms_granularity,
                                           list(g.particle_scale_sublayers)[:n_sub], g.uniformity, curves.shape[1],
                                           6.0, density_curves_layers=layers)
        s.unit(f"grain_particles_{film}", "grain_particles", npp, pixel_size_um=6.0, **case, rtol=1e-9)
        dc = params.film_render.dir_couplers
        n_ch = curves.shape[1]
        positive = pipe.film.info.type == "positive"
        m = cpl.compute_dir_couplers_matrix(dc, n_ch=n_ch)
        k, dref = cpl.langmuir_donor_params(norm, dc.langmuir_donor_k_rgb, n_ch=n_ch)
        if positive:
            kr, cref = cpl.receiver_langmuir_params(dref, m, dc.langmuir_receiver_k_rgb, n_ch=n_ch)
            donor_k = None
        else:
            kr = cref = None
            donor_k = k
        before = cpl.compute_density_curves_before_dir_couplers(norm, pipe.film.data.log_exposure, m * dc.amount,
                                                                donor_k, dref, receiver_kr=kr, receiver_c_ref=cref,
                                                                positive=positive)
        s.unit(f"dir_couplers_curves_{film}", "dir_couplers_curves", before, **case, atol=1e-11)

    # digest: every film against two prints, plus switches
    for film in FILMS:
        for print_ in ["kodak_portra_endura", "kodak_2383", "kodak_2302"]:
            case = {"id": f"digest_{film}_{print_}", "kind": "digest", "film": film, "print": print_, "atol": 1e-12,
                    "rtol": 1e-12}
            s.add(case, params_to_jsonable(build_params(case)))
    for tag, pre in [("preview", {"settings": {"preview_mode": True}}), ("lut_mode", {"debug": {"lut_mode": True}}),
                     ("no_db", {"settings": {"neutral_print_filters_from_database": False}}),
                     ("nonspatial", NONSPATIAL)]:
        case = {"id": f"digest_switch_{tag}", "kind": "digest", "film": "kodak_vision3_250d", "print": "kodak_2383",
                "pre": pre, "atol": 1e-12, "rtol": 1e-12}
        s.add(case, params_to_jsonable(build_params(case)))
    s.finish()


def suite_t2_nonspatial():
    s = Suite("t2_nonspatial")
    img = patches_image()
    taps = ["log_e_film", "cmy_film", "log_e_print", "cmy_print", "rgb_out"]
    # every tap for representative pairs
    reps = [("kodak_portra_400", "kodak_portra_endura"), ("kodak_vision3_500t", "kodak_2383"),
            ("fujifilm_pro_400h", "fujifilm_crystal_archive_typeii"), ("kodak_doublex", "kodak_2302"),
            ("kodak_doublex", "kodak_portra_endura"), ("kodak_portra_400", "kodak_2302"),
            ("fujifilm_velvia_100", "kodak_portra_endura")]
    for film, print_ in reps:
        for tap in taps:
            s.pipeline(f"taps_{film}__{print_}__{tap}", "patches", img, film=film, print_=print_, pre=NONSPATIAL,
                       collect=tap)
    # full film x print matrix, final output
    for film in FILMS:
        for print_ in PRINTS:
            s.pipeline(f"pair_{film}__{print_}", "patches", img, film=film, print_=print_, pre=NONSPATIAL)
    # scan-film route for every film
    for film in FILMS:
        s.pipeline(f"scanfilm_{film}", "patches", img, film=film, pre=merge(NONSPATIAL, {"io": {"scan_film": True}}))
    s.finish()


SUITES = {
    "t1_units": suite_t1_units,
    "t2_nonspatial": suite_t2_nonspatial,
}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--suite", action="append", help="suite(s) to generate; default: all")
    ap.add_argument("--allow-unpinned", action="store_true")
    args = ap.parse_args()
    import spektrafilm
    src = Path(spektrafilm.__file__).resolve().parents[2]
    try:
        head = subprocess.check_output(["git", "-C", str(src), "rev-parse", "HEAD"], text=True).strip()
    except Exception:
        head = None
    if head != PINNED_COMMIT and not args.allow_unpinned:
        sys.exit(f"spektrafilm checkout is at {head}, expected {PINNED_COMMIT}")
    for name in args.suite or list(SUITES):
        SUITES[name]()


if __name__ == "__main__":
    main()
