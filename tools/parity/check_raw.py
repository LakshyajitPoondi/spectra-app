# SPDX-License-Identifier: GPL-3.0-or-later
"""Compare the C++ RAW decode (spectra-cli convert) with the Python reference.

The RAW test files are not in the repository (see test_data/SOURCES.md), so
this is a local check, not a ctest target:

    python tools/parity/check_raw.py [--cli build/spectra-cli] [files...]

For every file it decodes with as-shot white balance; the first Canon file is
also decoded with daylight, tungsten and a custom temperature / tint. The
reference decodes through rawpy, which bundles its own LibRaw; a different
LibRaw version on the C++ side shows up here as small demosaic differences.
"""
import argparse
import glob
import os
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[2]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cli", default=str(ROOT / "build" / "spectra-cli"))
    ap.add_argument("files", nargs="*")
    args = ap.parse_args()
    import rawpy
    from spektrafilm.utils.raw_file_processor import load_and_process_raw_file
    files = args.files or sorted(glob.glob(str(ROOT / "test_data" / "raw" / "*" / "*")))
    files = [f for f in files if not f.endswith((".md", ".txt"))]
    if not files:
        sys.exit("no RAW files found; run scripts/fetch_test_raws.py first")
    print(f"reference: rawpy {rawpy.__version__}, LibRaw {'.'.join(map(str, rawpy.libraw_version))}")
    print(f"{'file':34s} {'mode':10s} {'size':>11s} {'max abs':>9s} {'p99.9':>9s} {'mean abs':>9s} {'mean rel':>9s}")
    worst = 0.0
    first_canon_done = False
    for f in files:
        modes = [("as_shot", {}, ["--wb", "as_shot"])]
        if "canon" in os.path.basename(f).lower() and not first_canon_done:
            first_canon_done = True
            modes += [("daylight", {"white_balance": "daylight"}, ["--wb", "daylight"]),
                      ("tungsten", {"white_balance": "tungsten"}, ["--wb", "tungsten"]),
                      ("custom", {"white_balance": "custom", "temperature": 4300.0, "tint": 1.08},
                       ["--wb", "custom", "--temperature", "4300", "--tint", "1.08"])]
        for name, kwargs, cli_args in modes:
            ref = np.asarray(load_and_process_raw_file(f, **kwargs), dtype=np.float64)
            with tempfile.TemporaryDirectory() as tmp:
                out = os.path.join(tmp, "o.npy")
                r = subprocess.run([args.cli, "convert", "--in", f, "--out", out] + cli_args,
                                   capture_output=True, text=True)
                if r.returncode != 0:
                    print(f"{os.path.basename(f):34s} {name:10s} FAILED: {r.stderr.strip()}")
                    worst = float("inf")
                    continue
                ours = np.load(out)
            if ours.shape != ref.shape:
                print(f"{os.path.basename(f):34s} {name:10s} SHAPE {ours.shape} vs {ref.shape}")
                worst = float("inf")
                continue
            d = np.abs(ours - ref)
            rel = d.mean() / max(ref.mean(), 1e-12)
            print(f"{os.path.basename(f):34s} {name:10s} {ref.shape[1]:5d}x{ref.shape[0]:<5d} {d.max():9.2e} "
                  f"{np.quantile(d[::7, ::7], 0.999):9.2e} {d.mean():9.2e} {rel:9.2e}")
            worst = max(worst, rel)
    print(f"worst mean relative difference: {worst:.2e}")
    return 0 if worst < 0.01 else 1


if __name__ == "__main__":
    sys.exit(main())
