#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Download the public RAW test files into test_data/raw/pixls/.

All files come from https://raw.pixls.us and are CC0 (public domain). They are
not committed to the repository; this script fetches them and verifies their
SHA-256 checksums. Python 3.8+, standard library only.

    python scripts/fetch_test_raws.py            # all files
    python scripts/fetch_test_raws.py sony nikon # only names containing these words
"""
import hashlib
import sys
import urllib.parse
import urllib.request
from pathlib import Path

# (local name, raw.pixls.us file id, original file name, sha256)
FILES = [
    ("canon_r6.cr3", 4662, "Canon - EOS R6 - 3:2.CR3",
     "81fab349ae9080fd0172583b7fd5ea36806e95ffa434909d2ccc6527dde34ee6"),
    ("canon_r6_craw.cr3", 4660, "Canon - EOS R6 - 3:2.CR3",
     "4775df76433b1e5844bc7a42b95d93efc8438c0a4cd9681e120e23fe3e3007e8"),
    ("sony_a7m2.arw", 967, "Sony - ILCE-7M2 - 14bit 14bit compressed (3:2).arw",
     "891b098c6cad16f01af4cbbe229d89652008b0108ad343079a4fc688b320fa14"),
    ("nikon_d750.nef", 898, "Nikon - D750 - 14bit 14bit compressed (Lossless) (3:2).NEF",
     "f90deb2819863f5a6ca9913c255fdd83a94963f4284eb88df9d389da7a6c7665"),
    ("fujifilm_xt3_xtrans.raf", 2783, "Fujifilm - X-T3 - 14bit 14bit compressed (3:2).RAF",
     "95b33021160b239ceb1a09d46a1b29cb60cb7dd47581feb126b337c219091754"),
    ("nikon_df_portrait.nef", 1390, "Nikon - Df - 14bit 14bit compressed (Lossless) (3:2).NEF",
     "65b940bc1436e5df406bdfd83dcfeffcbed9da5b96db67fa03036ec840cecbf9"),
    ("panasonic_lx100_square.rw2", 1139, "Panasonic - DMC-LX100 - 1:1.RW2",
     "49aa3e4cac9fa0963fec783c6dde1e5d1a667fb9a43dc47643c33f0cf0a12412"),
]

DEST = Path(__file__).resolve().parents[1] / "test_data" / "raw" / "pixls"


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def main():
    words = [w.lower() for w in sys.argv[1:]]
    DEST.mkdir(parents=True, exist_ok=True)
    failed = 0
    for name, file_id, original, digest in FILES:
        if words and not any(w in name for w in words):
            continue
        path = DEST / name
        if path.exists() and sha256(path) == digest:
            print(f"ok       {name}")
            continue
        url = f"https://raw.pixls.us/getfile.php/{file_id}/nice/{urllib.parse.quote(original)}"
        print(f"download {name} ...", flush=True)
        try:
            req = urllib.request.Request(url, headers={"User-Agent": "raw-test-fetch"})
            with urllib.request.urlopen(req, timeout=300) as r, open(path, "wb") as f:
                while True:
                    chunk = r.read(1 << 20)
                    if not chunk:
                        break
                    f.write(chunk)
        except Exception as e:  # noqa: BLE001
            print(f"FAILED   {name}: {e}")
            failed += 1
            continue
        got = sha256(path)
        if got != digest:
            print(f"FAILED   {name}: checksum mismatch ({got})")
            path.unlink()
            failed += 1
        else:
            print(f"ok       {name} ({path.stat().st_size / 1e6:.1f} MB)")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
