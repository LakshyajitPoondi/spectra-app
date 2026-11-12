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
        self.default_subsample = None

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
                 pre=None, post=None, inject=None, collect=None, atol=1e-9, rtol=1e-9, seed=None,
                 subsample=None):
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
        subsample = subsample or self.default_subsample
        if subsample and subsample > 1 and out.shape[0] * out.shape[1] > 1024:
            # keep the stored golden small: compare every k-th pixel of the full render
            case["subsample"] = subsample
            self.add(case, out[::subsample, ::subsample])
        else:
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

    # --- io.upscale_factor resampling (skimage rescale, order 3)
    from skimage.transform import rescale as _rescale
    for factor in [1.5, 2.0, 1.37, 0.5, 0.73]:
        s.unit(f"rescale_{factor}", "rescale", _rescale(img, factor, channel_axis=2, order=3), input=iin,
               factor=factor, atol=1e-11)

    # --- RAW white balance (the float32 rounding of the reference limits the accuracy)
    import colour as _colour
    from spektrafilm.utils import raw_file_processor as rfp
    for T in [1800.0, 2222.0, 2850.0, 3999.0, 4000.0, 5003.0, 6504.0, 7000.0, 7001.0, 12000.0]:
        method = 'CIE Illuminant D Series' if T >= 4000.0 else 'Kang 2002'
        s.unit(f"cct_to_xy_{T}", "cct_to_xy", _colour.CCT_to_xy(np.float64(T), method=method), temperature=T)
    aces = f32(rng.uniform(0, 1.2, (9, 11, 3)))
    ain = s.input("aces_9x11", aces)
    for T, tint in [(2850.0, 1.0), (4300.0, 1.08), (9000.0, 0.9), (6504.0, 1.2)]:
        _, adapt, tint_mult = rfp._postprocess_params('custom', T, tint)
        out = aces.astype(np.float32)
        if adapt is not None:
            out = rfp._apply_white_balance_adaptation(out, *adapt)
        out = rfp._apply_tint_adjustment(out, tint_mult)
        s.unit(f"raw_white_balance_{T}_{tint}", "raw_white_balance", out, input=ain, temperature=T, tint=tint,
               atol=3e-7, rtol=3e-7)

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


def small_scene():
    """200 x 133 portrait-orientation reduction of the upstream test photograph."""
    return block_reduce(portrait_full(), 5)


def detail_scene():
    """96 x 144 landscape crop of the photograph at full resolution (fine detail)."""
    return f32(portrait_full()[400:496, 250:394])


# film_format_mm chosen so the small test images have a realistic ~6 um pixel pitch.
def fmt(image, um=6.0):
    return {"camera": {"film_format_mm": max(image.shape[:2]) * um / 1000.0}}


def suite_t3_spatial():
    s = Suite("t3_spatial")
    s.default_subsample = 2
    scene = small_scene()
    detail = detail_scene()
    hi = highlights_image()
    base_scene = merge(NO_STOCHASTIC, fmt(scene))
    base_detail = merge(NO_STOCHASTIC, fmt(detail))
    base_hi = merge(NO_STOCHASTIC, fmt(hi))
    FFT = dict(atol=3e-4, rtol=3e-4)  # the reference convolves the diffusion PSF in float32

    for film, print_ in [("kodak_portra_400", "kodak_portra_endura"), ("kodak_gold_200", "kodak_supra_endura"),
                         ("kodak_vision3_500t", "kodak_2383"), ("kodak_doublex", "kodak_2302"),
                         ("fujifilm_c200", "fujifilm_crystal_archive_typeii")]:
        s.pipeline(f"default_{film}__{print_}", "scene", scene, film=film, print_=print_, pre=base_scene)
    for film in ["fujifilm_velvia_100", "kodak_kodachrome_64", "kodak_trix"]:
        s.pipeline(f"default_scanfilm_{film}", "scene", scene, film=film,
                   pre=merge(base_scene, {"io": {"scan_film": True}}))
    for tap in ["log_e_film", "cmy_film", "log_e_print", "cmy_print"]:
        s.pipeline(f"taps_portra400_{tap}", "detail", detail, pre=base_detail, collect=tap)
    s.pipeline("highlights_default", "highlights", hi, pre=base_hi)
    s.pipeline("highlights_log_e_film", "highlights", hi, pre=base_hi, collect="log_e_film")
    s.pipeline("highlights_weak_ah", "highlights", hi, film="kodak_gold_200", pre=base_hi)
    s.pipeline("highlights_boost", "highlights", hi,
               pre=merge(base_hi, {"film_render": {"halation": {"boost_ev": 4.0, "boost_range": 0.5, "protect_ev": 2.0}}}))
    s.pipeline("halation_off", "highlights", hi, pre=merge(base_hi, {"film_render": {"halation": {"active": False}}}))
    s.pipeline("halation_tuned", "highlights", hi, pre=base_hi,
               post={"film_render": {"halation": {"scatter_amount": 0.5, "scatter_spatial_scale": 3.0,
                                                  "halation_amount": 2.0, "halation_spatial_scale": 0.5,
                                                  "halation_strength": [0.3, 0.1, 0.015], "halation_n_bounces": 5,
                                                  "halation_bounce_decay": 0.8, "halation_renormalize": False}}})
    s.pipeline("camera_lens_blur", "detail", detail, pre=merge(base_detail, {"camera": {"lens_blur_um": 14.0}}))
    s.pipeline("camera_lens_blur_large", "detail", detail, pre=merge(base_detail, {"camera": {"lens_blur_um": 30.0}}))
    s.pipeline("scanner_blur", "detail", detail, pre=merge(base_detail, {"scanner": {"lens_blur": 1.2}}))
    s.pipeline("scanner_blur_iir", "detail", detail, pre=merge(base_detail, {"scanner": {"lens_blur": 3.5}}))
    s.pipeline("scanner_usm_strong", "detail", detail, pre=merge(base_detail, {"scanner": {"unsharp_mask": [1.5, 2.0]}}))
    s.pipeline("scanner_usm_off", "detail", detail, pre=merge(base_detail, {"scanner": {"unsharp_mask": [0.0, 0.0]}}))
    s.pipeline("dir_diffusion_tuned", "detail", detail, pre=base_detail,
               post={"film_render": {"dir_couplers": {"diffusion_size_um": 40.0, "diffusion_tail_um": 120.0,
                                                      "diffusion_tail_weight": 0.2, "amount": 1.5}}})
    s.pipeline("dir_diffusion_off", "detail", detail, pre=base_detail,
               post={"film_render": {"dir_couplers": {"diffusion_size_um": 0.0}}})
    s.pipeline("dir_diffusion_positive", "detail", detail, film="kodak_ektachrome_100",
               pre=merge(base_detail, {"io": {"scan_film": True}}))
    s.pipeline("format_120", "scene", scene, pre=merge(NO_STOCHASTIC, {"camera": {"film_format_mm": 2.4}}))
    s.pipeline("crop", "scene", scene,
               pre=merge(base_scene, {"io": {"crop": True, "crop_center": [0.4, 0.55], "crop_size": [0.5, 0.35]}}))
    s.pipeline("upscale_1.5", "detail", detail, pre=merge(base_detail, {"io": {"upscale_factor": 1.5}}))
    s.pipeline("upscale_0.6_crop", "scene", scene,
               pre=merge(base_scene, {"io": {"upscale_factor": 0.6, "crop": True, "crop_center": [0.5, 0.5],
                                             "crop_size": [0.5, 0.35]}}))
    s.pipeline("preview_mode", "scene", scene, pre=merge(base_scene, {"settings": {"preview_mode": True}}))
    for fam in ["glimmerglass", "black_pro_mist", "pro_mist", "cinebloom"]:
        for strength in [0.25, 2.0]:
            s.pipeline(f"camera_diffusion_{fam}_{strength}", "highlights", hi,
                       pre=merge(base_hi, {"camera": {"diffusion_filter": {"active": True, "filter_family": fam,
                                                                             "strength": strength}}}), **FFT)
    s.pipeline("camera_diffusion_tuned", "highlights", hi,
               pre=merge(base_hi, {"camera": {"diffusion_filter": {
                   "active": True, "filter_family": "pro_mist", "strength": 1.0, "spatial_scale": 0.5,
                   "halo_warmth": 0.9, "core_intensity": 0.5, "core_size": 2.0, "halo_intensity": 1.6,
                   "halo_size": 0.7, "bloom_intensity": 0.4, "bloom_size": 1.5}}}), **FFT)
    s.pipeline("camera_diffusion_bw", "highlights", hi, film="kodak_doublex", print_="kodak_2302",
               pre=merge(base_hi, {"camera": {"diffusion_filter": {"active": True, "strength": 1.0}}}), **FFT)
    s.pipeline("enlarger_diffusion", "highlights", hi,
               pre=merge(base_hi, {"enlarger": {"diffusion_filter": {"active": True, "filter_family": "cinebloom",
                                                                       "strength": 0.5}}}), **FFT)
    s.pipeline("all_spatial", "scene", scene,
               pre=merge(base_scene, {"camera": {"lens_blur_um": 8.0, "diffusion_filter": {"active": True, "strength": 0.5}},
                                      "enlarger": {"diffusion_filter": {"active": True, "strength": 0.125}},
                                      "scanner": {"lens_blur": 0.6, "unsharp_mask": [0.9, 0.5]},
                                      "film_render": {"halation": {"boost_ev": 2.0}}}), **FFT)
    s.finish()


def suite_t5_sweeps():
    s = Suite("t5_sweeps")
    img = patches_image()
    detail = detail_scene()
    pairs = [("kodak_portra_400", "kodak_portra_endura"), ("kodak_vision3_250d", "kodak_2383")]

    def chem(**kw):
        return kw

    pre_sweeps = {
        "ev_m2": {"camera": {"exposure_compensation_ev": -2.0}},
        "ev_p2": {"camera": {"exposure_compensation_ev": 2.0}},
        "ae_off": {"camera": {"auto_exposure": False}},
        "ae_off_ev1": {"camera": {"auto_exposure": False, "exposure_compensation_ev": 1.0}},
        **{f"ae_{m}": {"camera": {"auto_exposure_method": m}} for m in
           ["matrix", "multi_zone", "partial", "highlight_weighted", "median", "average"]},
        **{f"filter_{k}": {"camera": {"color_filter": k}} for k in ["hoya_x0", "hoya_x1", "hoya_y2", "hoya_ya3", "hoya_r1"]},
        "print_exposure_0.5": {"enlarger": {"print_exposure": 0.5}},
        "print_exposure_2": {"enlarger": {"print_exposure": 2.0}},
        "print_comp_off": {"enlarger": {"print_exposure_compensation": False}, "camera": {"exposure_compensation_ev": 1.0}},
        "print_norm_off": {"enlarger": {"normalize_print_exposure": False}, "camera": {"exposure_compensation_ev": 1.0}},
        "print_comp_norm_off": {"enlarger": {"normalize_print_exposure": False, "print_exposure_compensation": False,
                                             "print_exposure": 0.02}},
        "y_shift_p20": {"enlarger": {"y_filter_shift": 20.0}},
        "y_shift_m20": {"enlarger": {"y_filter_shift": -20.0}},
        "m_shift_p20": {"enlarger": {"m_filter_shift": 20.0}},
        "m_shift_m20": {"enlarger": {"m_filter_shift": -20.0}},
        "preflash": {"enlarger": {"preflash_exposure": 0.1}},
        "preflash_shift": {"enlarger": {"preflash_exposure": 0.3, "preflash_y_filter_shift": 15.0,
                                         "preflash_m_filter_shift": -10.0}},
        "illuminant_lens": {"enlarger": {"illuminant": "TH-KG3-L"}},
        "illuminant_bb": {"enlarger": {"illuminant": "BB3200"}},
        "no_db_filters": {"settings": {"neutral_print_filters_from_database": False}},
        "manual_filters": {"settings": {"neutral_print_filters_from_database": False},
                           "enlarger": {"c_filter_neutral": 10.0, "m_filter_neutral": 40.0, "y_filter_neutral": 80.0}},
        "white_corr": {"scanner": {"white_correction": True}},
        "black_corr": {"scanner": {"black_correction": True}},
        "bw_corr": {"scanner": {"white_correction": True, "black_correction": True}},
        "bw_corr_levels": {"scanner": {"white_correction": True, "black_correction": True, "white_level": 0.9,
                                       "black_level": 0.05}},
        **{f"in_{k}": {"io": {"input_color_space": v}} for k, v in
           [("srgb", "sRGB"), ("p3", "DCI-P3"), ("displayp3", "Display P3"), ("adobe", "Adobe RGB (1998)"),
            ("bt2020", "ITU-R BT.2020"), ("aces", "ACES2065-1")]},
        **{f"in_cctf_{k}": {"io": {"input_color_space": v, "input_cctf_decoding": True}} for k, v in
           [("srgb", "sRGB"), ("prophoto", "ProPhoto RGB"), ("bt2020", "ITU-R BT.2020"), ("adobe", "Adobe RGB (1998)")]},
        **{f"out_{k}": {"io": {"output_color_space": v}} for k, v in
           [("p3", "DCI-P3"), ("displayp3", "Display P3"), ("adobe", "Adobe RGB (1998)"), ("bt2020", "ITU-R BT.2020"),
            ("prophoto", "ProPhoto RGB"), ("aces", "ACES2065-1")]},
        "out_linear": {"io": {"output_cctf_encoding": False}},
        **{f"gamut_{a}": {"io": {"output_gamut_compress": {"algorithm": a}}} for a in ["off", "aces_rgc", "oklrab", "jzazbz"]},
        "gamut_knee": {"io": {"output_gamut_compress": {"knee": [0.5, 1.2, 1.5]}}},
        "gamut_cam16ucs": {"io": {"output_gamut_compress": {"algorithm": "cam16ucs"}}},
        "gamut_cam16_p3_nolight": {"io": {"output_color_space": "Display P3",
                                          "output_gamut_compress": {"algorithm": "cam16ucs", "lightness_compression": None}}},
        "gamut_cam16_prophoto": {"io": {"output_color_space": "ProPhoto RGB",
                                        "output_gamut_compress": {"algorithm": "cam16ucs", "knee": [0.4, 1.0, 3.0]}}},
        "gamut_no_lightness": {"io": {"output_gamut_compress": {"lightness_compression": None}}},
        "gamut_lightness": {"io": {"output_gamut_compress": {"lightness_compression": [0.3, 0.9, 4.0]}}},
        "gamut_jz_p3": {"io": {"output_color_space": "Display P3", "output_gamut_compress": {"algorithm": "jzazbz"}}},
        "gamut_oklrab_bt2020": {"io": {"output_color_space": "ITU-R BT.2020", "output_gamut_compress": {"algorithm": "oklrab"}}},
        "in_gamut_off": {"io": {"input_gamut_compress": {"active": False}}},
        "in_gamut_knee": {"io": {"input_gamut_compress": {"knee": [0.7, 1.05, 3.0]}}},
        "mallett": {"settings": {"rgb_to_raw_method": "mallett2019"}},
        "mallett_srgb": {"settings": {"rgb_to_raw_method": "mallett2019"}, "io": {"input_color_space": "sRGB",
                                                                                 "input_cctf_decoding": True}},
        "window_off": {"settings": {"apply_hanatos2025_adaptation_window": False}},
        "surface_on": {"settings": {"apply_hanatos2025_adaptation_surface": True}},
        "spectral_blur": {"settings": {"spectral_gaussian_blur": 3.0}},
        "film_chem_gamma": {"film_render": {"chemistry": chem(gamma_factor=1.25)}},
        "film_chem_mix": {"film_render": {"chemistry": chem(gamma_factor_fast=0.8, gamma_factor_slow=1.2,
                                                           gamma_factor_blue=1.1, developer_exhaustion=0.5)}},
        "film_chem_off": {"film_render": {"chemistry": chem(active=False)}},
        "print_chem_gamma": {"print_render": {"chemistry": chem(gamma_factor=0.8)}},
        "print_chem_mix": {"print_render": {"chemistry": chem(gamma_factor_fast=1.3, gamma_factor_slow=0.85,
                                                             gamma_factor_red=0.95, developer_exhaustion=0.8)}},
        "print_chem_off": {"print_render": {"chemistry": chem(active=False)}},
        "base_tuned": {"film_render": {"base": {"cyan": 0.8, "magenta": 1.2, "yellow": 0.6}}},
        "base_scale": {"film_render": {"base": {"scale": 0.5}}},
        "base_off": {"film_render": {"base": {"active": False}}},
        "scan_film": {"io": {"scan_film": True}},
        "scan_film_base_tuned": {"io": {"scan_film": True}, "film_render": {"base": {"cyan": 1.3, "scale": 0.7}}},
        "lut_mode": {"debug": {"lut_mode": True}},
    }
    post_sweeps = {
        "dir_off": {"film_render": {"dir_couplers": {"active": False}}},
        "dir_amount_2": {"film_render": {"dir_couplers": {"amount": 2.0}}},
        "dir_amount_4": {"film_render": {"dir_couplers": {"amount": 4.0}}},  # non-monotonic inversion (RISKS R-07)
        "dir_inhibition": {"film_render": {"dir_couplers": {"inhibition_samelayer": 0.3, "inhibition_interlayer": 2.0}}},
        "dir_langmuir": {"film_render": {"dir_couplers": {"langmuir_donor_k_rgb": [0.2, 3.0, 0.7]}}},
        "dir_gammas": {"film_render": {"dir_couplers": {"gamma_samelayer_rgb": [0.1, 0.9, 0.4],
                                                        "gamma_interlayer_r_to_gb": [0.0, 0.6],
                                                        "gamma_interlayer_g_to_rb": [0.5, 0.0],
                                                        "gamma_interlayer_b_to_rg": [0.3, 0.3]}}},
    }
    for film, print_ in pairs:
        short = film.split("_", 1)[1]
        for tag, pre in pre_sweeps.items():
            s.pipeline(f"{short}_{tag}", "patches", img, film=film, print_=print_, pre=merge(NONSPATIAL, pre))
        for tag, post in post_sweeps.items():
            s.pipeline(f"{short}_{tag}", "patches", img, film=film, print_=print_, pre=NONSPATIAL, post=post)
    # positive film: receiver-side Langmuir and black/white correction on a film scan
    pos = merge(NONSPATIAL, {"io": {"scan_film": True}})
    for film in ["fujifilm_provia_100f", "kodak_trix"]:
        short = film.split("_", 1)[1]
        s.pipeline(f"{short}_bw_corr", "patches", img, film=film,
                   pre=merge(pos, {"scanner": {"white_correction": True, "black_correction": True}}))
        s.pipeline(f"{short}_white_corr", "patches", img, film=film, pre=merge(pos, {"scanner": {"white_correction": True}}))
        s.pipeline(f"{short}_black_corr", "patches", img, film=film, pre=merge(pos, {"scanner": {"black_correction": True}}))
        s.pipeline(f"{short}_dir_receiver", "patches", img, film=film, pre=pos,
                   post={"film_render": {"dir_couplers": {"amount": 2.5, "langmuir_receiver_k_rgb": [0.3, 1.0, 2.0]}}})
        s.pipeline(f"{short}_printed", "patches", img, film=film, pre=NONSPATIAL)
    # B&W development-time families
    for dt in [4.0, 6.5, 12.0]:
        s.pipeline(f"doublex_dt{dt}", "patches", img, film="kodak_doublex", print_="kodak_2302",
                   pre=merge(NONSPATIAL, {"film_render": {"chemistry": {"development_time": dt}}}))
    for dt in [2.0, 9.0]:
        s.pipeline(f"2302_dt{dt}", "patches", img, film="kodak_doublex", print_="kodak_2302",
                   pre=merge(NONSPATIAL, {"print_render": {"chemistry": {"development_time": dt}}}))
    s.pipeline("doublex_bw_corr", "patches", img, film="kodak_doublex", print_="kodak_2302",
               pre=merge(NONSPATIAL, {"scanner": {"white_correction": True, "black_correction": True}}))
    # metering on a real image (uses the anti-aliased metering preview)
    scene = small_scene()
    big = f32(np.kron(scene, np.ones((2, 3, 1))))  # 400 x 399, larger than the 256 px metering preview
    for m in ["center_weighted", "matrix", "multi_zone", "partial", "highlight_weighted", "median", "average"]:
        s.pipeline(f"metering_big_{m}", "scene_400x399", big, pre=merge(NONSPATIAL, {"camera": {"auto_exposure_method": m}}),
                   collect="log_e_film", subsample=8)
    s.finish()


GUI_FAST = {"settings": {"use_enlarger_lut": True, "use_scanner_lut": True, "lut_resolution": 17,
                         "use_fast_stats": True, "preview_mode": True}}


def suite_fast_preview():
    """The Python GUI's faster configuration: 17^3 enlarger / scanner LUTs,
    fast statistics and preview mode. Two comparisons per case:
      *_gui : C++ fast preview vs Python with the same settings (exact port);
      *_ref : C++ fast preview vs the Python reference without LUTs (looser,
              bounds the error the LUT acceleration introduces).
    """
    s = Suite("fast_preview")
    img = patches_image()
    scene = small_scene()
    pairs = [("kodak_portra_400", "kodak_portra_endura"), ("kodak_vision3_500t", "kodak_2383"),
             ("kodak_gold_200", "fujifilm_crystal_archive_typeii"), ("kodak_doublex", "kodak_2302"),
             ("kodak_doublex", "kodak_portra_endura"), ("kodak_portra_400", "kodak_2302")]
    from spektrafilm.runtime.process import Simulator
    for film, print_ in pairs:
        tag = f"{film}__{print_}"
        fast = merge(NO_STOCHASTIC, GUI_FAST)
        s.pipeline(f"patches_gui_{tag}", "patches", img, film=film, print_=print_, pre=fast)
        s.pipeline(f"scene_gui_{tag}", "scene", scene, film=film, print_=print_, pre=merge(fast, fmt(scene)),
                   subsample=2)
        # reference (no LUT) golden, rendered by the C++ side with the fast settings
        ref_pre = merge(NO_STOCHASTIC, {"settings": {"preview_mode": True}}, fmt(scene))
        case = {"film": film, "print": print_, "pre": ref_pre}
        ref = Simulator(build_params(case))._pipeline.process(np.array(scene))
        s.add({"id": f"scene_ref_{tag}", "kind": "pipeline", "film": film, "print": print_,
               "pre": merge(fast, fmt(scene)), "input": s.input("scene", scene), "atol": 0.015, "rtol": 0.0,
               "p99_max": 0.006, "rmse_max": 0.003, "subsample": 2}, ref[::2, ::2])
    for film in ["fujifilm_velvia_100", "kodak_trix"]:
        fast = merge(NO_STOCHASTIC, GUI_FAST, {"io": {"scan_film": True}})
        s.pipeline(f"patches_gui_scanfilm_{film}", "patches", img, film=film, pre=fast)
    s.finish()


def grain_stats(img, pixel_size_um):
    """Statistics vector shared with the C++ runner (apps/spectra-cli/parity.cpp)."""
    img = np.asarray(img, dtype=np.float64)
    h, w, c = img.shape
    out = {"mean": [], "std": [], "rms48": [], "spectrum": []}
    r = 24.0 / pixel_size_um
    k = int(np.floor(r))
    yy, xx = np.mgrid[-k:k + 1, -k:k + 1]
    disc = ((xx * xx + yy * yy) <= r * r).astype(np.float64)
    disc /= disc.sum()
    from scipy.signal import correlate2d
    for ch in range(c):
        p = img[:, :, ch]
        out["mean"].append(p.mean())
        out["std"].append(p.std())
        ap = correlate2d(p, disc, mode="valid")
        out["rms48"].append(ap.std() * 1000.0)
        f = np.fft.fft2(p - p.mean())
        power = (f.real ** 2 + f.imag ** 2)
        fy = np.fft.fftfreq(h) * h
        fx = np.fft.fftfreq(w) * w
        rad = np.sqrt(fy[:, None] ** 2 + fx[None, :] ** 2)
        edges = np.linspace(min(h, w) / 32.0, min(h, w) / 2.0, 9)
        bins = np.array([power[(rad >= edges[i]) & (rad < edges[i + 1])].mean() for i in range(8)])
        out["spectrum"].extend(bins / bins.sum())
    return {k2: np.array(v) for k2, v in out.items()}


def suite_t4_stochastic():
    """Statistical parity of the stochastic effects (grain, glare).

    The C++ random numbers cannot match NumPy's, so the comparison is on
    statistics of flat patches: mean, standard deviation, RMS granularity
    through a 48 um aperture, and the radial power spectrum.
    """
    import scipy.stats
    from spektrafilm.model.grain import layer_particle_model
    from spektrafilm.utils.fast_stats import fast_binomial, fast_poisson
    from spektrafilm.runtime.process import Simulator
    s = Suite("t4_stochastic")
    # 768^2 patches: ~9000 independent 48 um apertures, so the RMS-granularity
    # estimate has ~0.75 % noise per side and the 5 % threshold is > 4 sigma.
    N = 768
    um = 6.0
    flat_fmt = {"camera": {"film_format_mm": N * um / 1000.0, "auto_exposure": False}}

    # --- samplers: empirical distribution of the C++ exact samplers vs the analytic pmf
    for lam in [0.3, 2.5, 9.9, 10.0, 37.0, 400.0, 5000.0]:
        kmax = int(lam + 12 * np.sqrt(lam) + 12)
        s.unit(f"pmf_poisson_{lam}", "sampler_pmf", scipy.stats.poisson.pmf(np.arange(kmax + 1), lam),
               dist="poisson", lam=lam, kmax=kmax, fast=False, samples=2000000, atol=1.2e-3, rtol=0.0)
    for n, p in [(5, 0.3), (40, 0.02), (40, 0.5), (40, 0.97), (300, 0.01), (300, 0.4), (5000, 0.2), (5000, 0.999)]:
        s.unit(f"pmf_binomial_{n}_{p}", "sampler_pmf", scipy.stats.binom.pmf(np.arange(n + 1), n, p),
               dist="binomial", n=n, p=p, kmax=n, fast=False, samples=2000000, atol=1.2e-3, rtol=0.0)
    # --- fast samplers vs the reference's fast_stats (empirical on both sides)
    M = 4000000
    for lam in [2.5, 29.0, 31.0, 400.0]:
        kmax = int(lam + 12 * np.sqrt(lam) + 12)
        draws = fast_poisson(np.full(M, lam))
        pmf = np.bincount(np.clip(draws, 0, kmax), minlength=kmax + 1) / M
        s.unit(f"pmf_fast_poisson_{lam}", "sampler_pmf", pmf, dist="poisson", lam=lam, kmax=kmax, fast=True,
               samples=2000000, atol=1.5e-3, rtol=0.0)
    for n, p in [(10, 0.3), (40, 0.1), (40, 0.5), (300, 0.4)]:
        draws = fast_binomial(np.full(M, n, dtype=np.int64), np.full(M, p))
        pmf = np.bincount(draws, minlength=n + 1) / M
        s.unit(f"pmf_fast_binomial_{n}_{p}", "sampler_pmf", pmf, dist="binomial", n=n, p=p, kmax=n, fast=True,
               samples=2000000, atol=1.5e-3, rtol=0.0)

    # --- one emulsion sub-layer: mean / std of the sampled density vs density
    levels = [0.02, 0.1, 0.3, 0.6, 0.9, 1.2, 1.5, 1.75]
    for tag, dmax, npp, u in [("coarse", 1.8, 6.0, 0.97), ("medium", 1.8, 45.0, 0.97), ("fine", 1.8, 800.0, 0.6)]:
        for fast in [False, True]:
            np.random.seed(5)
            stats = []
            for d in levels:
                g = layer_particle_model(np.full((512, 512), d, dtype=np.float32), density_max=dmax,
                                         n_particles_per_pixel=npp, grain_uniformity=u, use_fast_stats=fast)
                stats.append([g.mean(), g.std()])
            stats = np.array(stats, dtype=np.float64)
            name = f"layer_{tag}_{'fast' if fast else 'exact'}"
            s.unit(name + "_mean", "grain_layer_stats", stats[:, 0], stat="mean", levels=levels, density_max=dmax,
                   n_particles=npp, uniformity=u, fast=fast, size=512, atol=0.004, rtol=0.01)
            s.unit(name + "_std", "grain_layer_stats", stats[:, 1], stat="std", levels=levels, density_max=dmax,
                   n_particles=npp, uniformity=u, fast=fast, size=512, atol=0.0, rtol=0.03)

    # --- full grain model on flat patches, per stock and exposure level
    stocks = ["kodak_portra_400", "kodak_vision3_500t", "kodak_gold_200", "fujifilm_velvia_100", "kodak_doublex",
              "kodak_trix"]
    evs = [-5, -3, -2, -1, 0, 1, 2, 3, 4, 6]
    for film in stocks:
        for fast in [False, True]:
            if fast and film not in ("kodak_portra_400", "kodak_doublex"):
                continue
            pre = merge(flat_fmt, {"settings": {"use_fast_stats": fast}})
            case = {"film": film, "pre": pre}
            sim = Simulator(build_params(case))
            for ev in evs:
                value = float(np.float32(0.184 * 2.0 ** ev))
                img = np.full((N, N, 3), value)
                out = sim._pipeline.process(img, collect="cmy_film")
                st = grain_stats(out, um)
                base = {"kind": "pipeline_stats", "film": film, "pre": pre, "collect": "cmy_film",
                        "flat": {"value": value, "size": N}, "pixel_size_um": um}
                tag = f"grain_{film}_{'fast' if fast else 'exact'}_ev{ev}"
                s.add({**base, "id": tag + "_mean", "stat": "mean", "atol": 0.005, "rtol": 0.0}, st["mean"])
                s.add({**base, "id": tag + "_std", "stat": "std", "atol": 2e-4, "rtol": 0.05}, st["std"])
                s.add({**base, "id": tag + "_rms48", "stat": "rms48", "atol": 0.05, "rtol": 0.05}, st["rms48"])
                s.add({**base, "id": tag + "_spectrum", "stat": "spectrum", "atol": 0.004, "rtol": 0.10},
                      st["spectrum"])
    # --- glare on a flat print (linear output so the statistic is not shaped by the CCTF)
    for tag, glare in [("default", {}), ("rough", {"percent": 0.2, "roughness": 1.5, "blur": 1.5})]:
        pre = merge(flat_fmt, {"film_render": {"grain": {"active": False}}, "print_render": {"glare": glare},
                               "io": {"output_cctf_encoding": False, "output_gamut_compress": {"algorithm": "off"}},
                               "scanner": {"unsharp_mask": [0.0, 0.0]}})
        case = {"film": "kodak_portra_400", "pre": pre}
        sim = Simulator(build_params(case))
        img = np.full((N, N, 3), float(np.float32(0.02)))
        outs = [grain_stats(sim._pipeline.process(img), um) for _ in range(3)]
        st = {k: np.mean([o[k] for o in outs], axis=0) for k in outs[0]}
        base = {"kind": "pipeline_stats", "film": "kodak_portra_400", "pre": pre, "flat": {"value": float(np.float32(0.02)), "size": N},
                "pixel_size_um": um}
        s.add({**base, "id": f"glare_{tag}_mean", "stat": "mean", "atol": 2e-5, "rtol": 0.002}, st["mean"])
        s.add({**base, "id": f"glare_{tag}_std", "stat": "std", "atol": 1e-7, "rtol": 0.05}, st["std"])
        s.add({**base, "id": f"glare_{tag}_spectrum", "stat": "spectrum", "atol": 0.004, "rtol": 0.10}, st["spectrum"])
    s.finish()


SUITES = {
    "t1_units": suite_t1_units,
    "t2_nonspatial": suite_t2_nonspatial,
    "t3_spatial": suite_t3_spatial,
    "t4_stochastic": suite_t4_stochastic,
    "t5_sweeps": suite_t5_sweeps,
    "fast_preview": suite_fast_preview,
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
