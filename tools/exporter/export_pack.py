# SPDX-License-Identifier: GPL-3.0-or-later
"""Export the spektrafilm reference data into one versioned binary pack (.sfpk).

Run inside a virtualenv that has spektrafilm installed at the pinned commit
(see tools/exporter/README.md). The pack holds every table the C++ app needs:
profiles, presets, filters, illuminants, colorimetry and colour-space data.
The values are taken from spektrafilm's own loaders and from colour-science,
so they are exactly what the Python reference uses.

Container layout (little-endian):
    "SFPK" | u32 version | u64 json_len | json (padded to 64) | u64 bin_len | bin
The JSON chunk lists every array (dtype, shape, offset, nbytes, crc32) plus a
free-form "meta" tree. Array offsets are relative to the start of the BIN chunk.
"""
from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import struct
import subprocess
import zlib
from pathlib import Path

import numpy as np

PACK_VERSION = 1
PINNED_COMMIT = "6cd00c8d4f30b5b550f50f4bbd3753c9f2a48507"
UPSTREAM_URL = "https://github.com/andreavolpato/spektrafilm"
UPSTREAM_BRANCH = "dev"
ALIGN = 64

COLOUR_SPACES = [
    "sRGB", "DCI-P3", "Display P3", "Adobe RGB (1998)", "ITU-R BT.2020",
    "ProPhoto RGB", "ACES2065-1",
]
CCTF_IDS = {
    "sRGB": "srgb", "Display P3": "srgb", "DCI-P3": "gamma:2.6",
    "Adobe RGB (1998)": "gamma:2.19921875", "ITU-R BT.2020": "bt2020",
    "ProPhoto RGB": "romm", "ACES2065-1": "linear",
}
ILLUMINANTS = ["D50", "D55", "D65", "D75", "A", "T", "K75P", "TH-KG3", "TH-KG3-L"]


class PackWriter:
    def __init__(self):
        self.arrays = {}
        self.blobs = []
        self.offset = 0

    def add(self, name, array, dtype=None):
        a = np.ascontiguousarray(np.asarray(array, dtype=dtype))
        if a.dtype not in (np.float64, np.float32, np.float16, np.int32):
            raise TypeError(f"{name}: unsupported dtype {a.dtype}")
        if name in self.arrays:
            raise KeyError(f"duplicate array {name}")
        data = a.astype(a.dtype.newbyteorder("<"), copy=False).tobytes()
        pad = (-self.offset) % ALIGN
        if pad:
            self.blobs.append(b"\0" * pad)
            self.offset += pad
        self.arrays[name] = {
            "dtype": a.dtype.name,
            "shape": list(a.shape),
            "offset": self.offset,
            "nbytes": len(data),
            "crc32": zlib.crc32(data) & 0xFFFFFFFF,
        }
        self.blobs.append(data)
        self.offset += len(data)

    def write(self, path, meta):
        bin_chunk = b"".join(self.blobs)
        doc = {"arrays": self.arrays, "meta": meta}
        js = json.dumps(doc, sort_keys=True, separators=(",", ":")).encode("utf-8")
        header_len = 4 + 4 + 8
        pad = (-(header_len + len(js) + 8)) % ALIGN
        js += b" " * pad
        with open(path, "wb") as f:
            f.write(b"SFPK")
            f.write(struct.pack("<I", PACK_VERSION))
            f.write(struct.pack("<Q", len(js)))
            f.write(js)
            f.write(struct.pack("<Q", len(bin_chunk)))
            f.write(bin_chunk)
        return hashlib.sha256(bin_chunk).hexdigest()


def _git_head(path):
    try:
        return subprocess.check_output(
            ["git", "-C", str(path), "rev-parse", "HEAD"], text=True).strip()
    except Exception:
        return None


def export(out_dir: Path, allow_unpinned: bool):
    import colour
    import scipy
    import spektrafilm
    from spektrafilm.config import SPECTRAL_SHAPE, STANDARD_OBSERVER_CMFS, LOG_EXPOSURE
    from spektrafilm.data.profiles_loader import list_profiles, load_profile
    from spektrafilm.data.presets_loader import read_coupler_presets, read_grain_presets
    from spektrafilm.model import color_filters as cf
    from spektrafilm.model.illuminants import standard_illuminant
    from spektrafilm.utils.io import read_neutral_print_filters
    from spektrafilm.utils.spectral_upsampling import MALLETT2019_BASIS
    from spektrafilm.utils.gamut_compression import spectral_locus_xy
    import importlib.resources

    src_root = Path(spektrafilm.__file__).resolve().parents[2]
    head = _git_head(src_root)
    if head != PINNED_COMMIT and not allow_unpinned:
        raise SystemExit(
            f"spektrafilm checkout is at {head}, expected pinned {PINNED_COMMIT}. "
            "Check out the pinned commit or pass --allow-unpinned.")

    w = PackWriter()
    meta = {}

    # --- spectral grid and colorimetry
    wl = np.asarray(SPECTRAL_SHAPE.wavelengths, dtype=np.float64)
    assert wl.shape == (81,)
    w.add("spectral/wavelengths", wl)
    w.add("spectral/cmfs_xyz", np.asarray(STANDARD_OBSERVER_CMFS[:], dtype=np.float64))
    w.add("spectral/log_exposure", np.asarray(LOG_EXPOSURE, dtype=np.float64))
    w.add("spectral/mallett2019_basis", np.asarray(MALLETT2019_BASIS[:], dtype=np.float64))
    w.add("spectral/locus_xy", np.asarray(spectral_locus_xy(), dtype=np.float64))
    for name in ILLUMINANTS:
        w.add(f"illuminants/{name}", np.asarray(standard_illuminant(name), dtype=np.float64))
    import colour.colorimetry.blackbody as bb
    meta["planck"] = {"c2": float(bb.CONSTANT_C2), "n": float(bb.CONSTANT_N)}
    meta["illuminants"] = ILLUMINANTS

    # --- filters (already resampled to the 81-sample grid by upstream's Akima code)
    for brand, obj in [
        ("custom", cf.custom_dichroic_filters),
        ("thorlabs", cf.thorlabs_dichroic_filters),
        ("edmund_optics", cf.edmund_optics_dichroic_filters),
        ("durst_digital_light", cf.durst_digital_light_dicrhoic_filters),
    ]:
        w.add(f"filters/dichroic/{brand}", np.asarray(obj.filters, dtype=np.float64))
    w.add("filters/heat/KG1", cf.schott_kg1_heat_filter.transmittance, np.float64)
    w.add("filters/heat/KG3", cf.schott_kg3_heat_filter.transmittance, np.float64)
    w.add("filters/heat/KG5", cf.schott_kg5_heat_filter.transmittance, np.float64)
    w.add("filters/lens/generic", cf.generic_lens_transmission.transmittance, np.float64)
    camera_filters = []
    for spec in cf.COLOR_FILTER_LIBRARY:
        w.add(f"filters/camera/{spec.key}", cf.color_filter_transmittance(spec.key), np.float64)
        camera_filters.append({"key": spec.key, "label": spec.label})
    meta["camera_filters"] = camera_filters
    meta["neutral_print_filters"] = read_neutral_print_filters()

    # --- presets (TOML -> JSON; None padding becomes null)
    meta["grain_presets"] = json.loads(json.dumps(read_grain_presets()))
    meta["coupler_presets"] = json.loads(json.dumps(read_coupler_presets()))

    # --- spectral upsampling LUT, stored as upstream stores it (float16)
    lut_path = importlib.resources.files(
        "spektrafilm.data.luts.spectral_upsampling").joinpath("irradiance_xy_tc.npy")
    with lut_path.open("rb") as f:
        spectra_lut = np.load(f)
    assert spectra_lut.dtype == np.float16 and spectra_lut.shape[2] == 81
    w.add("upsampling/hanatos2025_spectra_lut", spectra_lut)

    # --- colour spaces and adaptation matrices
    spaces = {}
    for name in COLOUR_SPACES:
        cs = colour.RGB_COLOURSPACES[name]
        key = name
        w.add(f"colourspaces/{key}/rgb_to_xyz", np.asarray(cs.matrix_RGB_to_XYZ, np.float64))
        w.add(f"colourspaces/{key}/xyz_to_rgb", np.asarray(cs.matrix_XYZ_to_RGB, np.float64))
        w.add(f"colourspaces/{key}/whitepoint_xy", np.asarray(cs.whitepoint, np.float64))
        spaces[key] = {"cctf": CCTF_IDS[name], "whitepoint_name": cs.whitepoint_name}
    meta["colourspaces"] = spaces
    for cat in ("CAT02", "CAT16"):
        w.add(f"cat/{cat}", np.asarray(
            colour.adaptation.CHROMATIC_ADAPTATION_TRANSFORMS[cat], np.float64))
    import colour.models.oklab as ok
    w.add("oklab/m1_xyz_to_lms", np.asarray(ok.MATRIX_1_XYZ_TO_LMS, np.float64))
    w.add("oklab/m2_lms_to_lab", np.asarray(ok.MATRIX_2_LMS_TO_LAB, np.float64))
    w.add("oklab/m1_lms_to_xyz", np.asarray(ok.MATRIX_1_LMS_TO_XYZ, np.float64))
    w.add("oklab/m2_lab_to_lms", np.asarray(ok.MATRIX_2_LAB_TO_LMS, np.float64))
    import colour.models.jzazbz as jz
    w.add("jzazbz/xyz_to_lms", np.asarray(jz.MATRIX_JZAZBZ_XYZ_TO_LMS, np.float64))
    w.add("jzazbz/lms_to_xyz", np.asarray(jz.MATRIX_JZAZBZ_LMS_TO_XYZ, np.float64))
    w.add("jzazbz/lms_p_to_izazbz", np.asarray(
        jz.MATRIX_JZAZBZ_LMS_P_TO_IZAZBZ_SAFDAR2017, np.float64))
    w.add("jzazbz/izazbz_to_lms_p", np.asarray(
        jz.MATRIX_JZAZBZ_IZAZBZ_TO_LMS_P_SAFDAR2017, np.float64))
    meta["jzazbz_constants"] = {k: float(v) for k, v in jz.CONSTANTS_JZAZBZ_SAFDAR2017.items()}

    # --- profiles
    profiles = {}
    data_fields = [
        "wavelengths", "log_sensitivity", "channel_density", "base_density",
        "midscale_neutral_density", "log_exposure", "development_time",
        "hanatos2025_adaptation_window_params", "hanatos2025_adaptation_surface_params",
    ]
    for stock in list_profiles():
        p = load_profile(stock)
        info = p.info
        d = p.data
        if not np.array_equal(d.wavelengths, wl):
            raise SystemExit(f"{stock}: wavelength grid differs from the 81-sample grid")
        if not np.array_equal(d.log_exposure, np.asarray(LOG_EXPOSURE)):
            raise SystemExit(f"{stock}: log-exposure grid is not the uniform 256-sample grid")
        entry = {
            "stock": info.stock, "name": info.name, "type": info.type,
            "support": info.support, "stage": info.stage, "use": info.use,
            "antihalation": info.antihalation, "target_print": info.target_print,
            "channel_model": info.channel_model, "densitometer": info.densitometer,
            "log_sensitivity_density_over_min": info.log_sensitivity_density_over_min,
            "reference_illuminant": info.reference_illuminant,
            "viewing_illuminant": info.viewing_illuminant,
            "fields": [],
        }
        for field in data_fields:
            value = getattr(d, field)
            if value is None:
                continue
            w.add(f"profiles/{stock}/{field}", np.asarray(value, np.float64))
            entry["fields"].append(field)
        m = d.density_curves_model
        if m is None or m.centers is None:
            raise SystemExit(f"{stock}: profile has no density_curves_model")
        entry["model_type"] = m.model_type
        w.add(f"profiles/{stock}/model_centers", np.asarray(m.centers, np.float64))
        w.add(f"profiles/{stock}/model_amplitudes", np.asarray(m.amplitudes, np.float64))
        w.add(f"profiles/{stock}/model_sigmas", np.asarray(m.sigmas, np.float64))
        entry["model_has_alphas"] = m.alphas is not None
        if m.alphas is not None:
            w.add(f"profiles/{stock}/model_alphas", np.asarray(m.alphas, np.float64))
        for illum in (info.reference_illuminant, info.viewing_illuminant):
            if illum not in ILLUMINANTS and not str(illum).startswith("BB"):
                raise SystemExit(f"{stock}: illuminant {illum!r} is not exported")
        profiles[stock] = entry
    meta["profiles"] = profiles

    meta["provenance"] = {
        "upstream_url": UPSTREAM_URL,
        "upstream_branch": UPSTREAM_BRANCH,
        "upstream_commit": head,
        "pinned_commit": PINNED_COMMIT,
        "spektrafilm_version": getattr(spektrafilm, "__version__", "0.3.3"),
        "colour_science_version": colour.__version__,
        "numpy_version": np.__version__,
        "scipy_version": scipy.__version__,
        "exporter_version": 1,
        "pack_version": PACK_VERSION,
        "exported_utc": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%d"),
        "licence": "CC BY-SA 4.0",
        "attribution": "spektrafilm profiles by Andrea Volpato, "
                       "https://github.com/andreavolpato/spektrafilm, CC BY-SA 4.0",
    }

    out_dir.mkdir(parents=True, exist_ok=True)
    sha = w.write(out_dir / "spektrafilm.sfpk", meta)
    manifest = {
        "provenance": meta["provenance"],
        "bin_sha256": sha,
        "arrays": {k: {"dtype": v["dtype"], "shape": v["shape"], "crc32": v["crc32"]}
                   for k, v in sorted(w.arrays.items())},
        "profiles": sorted(profiles),
    }
    (out_dir / "pack_manifest.json").write_text(json.dumps(manifest, indent=1, sort_keys=True) + "\n")
    print(f"wrote {out_dir / 'spektrafilm.sfpk'}: {len(w.arrays)} arrays, "
          f"{w.offset / 1e6:.2f} MB, sha256 {sha[:16]}")


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", type=Path, default=Path(__file__).resolve().parents[2] / "data")
    ap.add_argument("--allow-unpinned", action="store_true")
    args = ap.parse_args()
    export(args.out, args.allow_unpinned)
