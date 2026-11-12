# Test data sources

RAW files are **not committed**. `test_data/raw/` is git-ignored.

## Public samples (`test_data/raw/pixls/`)

Fetched by `python scripts/fetch_test_raws.py`, which verifies each SHA-256.
All files come from https://raw.pixls.us and are licensed **CC0 1.0 (public domain)**.

| Local name | Camera | Why it is in the set | Size | Source file on raw.pixls.us | File id | SHA-256 |
|---|---|---|---|---|---|---|
| `canon_r6.cr3` | Canon EOS R6 | Primary target camera, CR3, 20 MP | 5496×3670 | `Canon - EOS R6 - 3:2.CR3` | 4662 | `81fab349ae9080fd0172583b7fd5ea36806e95ffa434909d2ccc6527dde34ee6` |
| `canon_r6_craw.cr3` | Canon EOS R6 | Compressed C-RAW variant | 5496×3670 | `Canon - EOS R6 - 3:2.CR3` | 4660 | `4775df76433b1e5844bc7a42b95d93efc8438c0a4cd9681e120e23fe3e3007e8` |
| `sony_a7m2.arw` | Sony ILCE-7M2 | Sony, 14-bit compressed, 24 MP | 6024×4024 | `Sony - ILCE-7M2 - 14bit 14bit compressed (3:2).arw` | 967 | `891b098c6cad16f01af4cbbe229d89652008b0108ad343079a4fc688b320fa14` |
| `nikon_d750.nef` | Nikon D750 | Nikon, 14-bit lossless, 24 MP | 6032×4032 | `Nikon - D750 - 14bit 14bit compressed (Lossless) (3:2).NEF` | 898 | `f90deb2819863f5a6ca9913c255fdd83a94963f4284eb88df9d389da7a6c7665` |
| `fujifilm_xt3_xtrans.raf` | Fujifilm X-T3 | X-Trans sensor (6×6 pattern), 26 MP | 6246×4170 | `Fujifilm - X-T3 - 14bit 14bit compressed (3:2).RAF` | 2783 | `95b33021160b239ceb1a09d46a1b29cb60cb7dd47581feb126b337c219091754` |
| `nikon_df_portrait.nef` | Nikon Df | Portrait orientation (EXIF rotate), 16 MP | 3292×4940 | `Nikon - Df - 14bit 14bit compressed (Lossless) (3:2).NEF` | 1390 | `65b940bc1436e5df406bdfd83dcfeffcbed9da5b96db67fa03036ec840cecbf9` |
| `panasonic_lx100_square.rw2` | Panasonic DMC-LX100 | Odd size: 1:1 square, 9.6 MP | 3104×3104 | `Panasonic - DMC-LX100 - 1:1.RW2` | 1139 | `49aa3e4cac9fa0963fec783c6dde1e5d1a667fb9a43dc47643c33f0cf0a12412` |

Download URL pattern: `https://raw.pixls.us/getfile.php/<file id>/nice/<source file name>`.

## Owner's files (`test_data/raw/canon_r6/`)

Canon EOS R6 CR3 files supplied by the repository owner. Private, never committed, no licence
granted to anyone else. Wanted scenes: high contrast with clipped highlights; night with small
bright lights; high ISO (6400+); base-ISO portrait with skin tones; tungsten or mixed light;
saturated colours; one portrait-orientation frame; one flat mid-tone subject.

## Images in the repository

`tests/golden/*/inputs/` holds small synthetic arrays and reductions of
`img/test/portrait_leaves_32bit_linear_prophoto_rgb.tif` from the spektrafilm repository
(by Andrea Volpato, distributed with spektrafilm under GPL-3.0).
