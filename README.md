# Spectra (working title)

A native, GPU-accelerated film simulation app for Windows, built as a faithful port of
[spektrafilm](https://github.com/andreavolpato/spektrafilm) by Andrea Volpato.
Film simulation powered by spektrafilm profiles (CC BY-SA 4.0).

**Status: phase 2 of 6.** The C++ CPU reference pipeline and the parity harness against the
Python reference are done. There is no GPU backend or user interface yet. See `PROGRESS.md`.

## Build and test

Requirements: CMake 3.24+, a C++20 compiler (GCC 13, Clang 18 or MSVC 2022). No other
dependencies; the two third-party headers are vendored.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
```

```bash
cmake --build build --config Release --parallel
```

```bash
ctest --test-dir build -C Release --output-on-failure
```

Building and testing needs no Python: the data pack and the goldens are in the repository.

## Command-line tool

```bash
build/spectra-cli info
```

```bash
build/spectra-cli render --in input.npy --out output.npy --film kodak_portra_400 --print kodak_portra_endura
```

```bash
build/spectra-cli parity tests/golden/t2_nonspatial/cases.json -v
```

Images are NumPy `.npy` arrays (H x W x 3, linear ProPhoto RGB by default) until the image
and RAW I/O layer lands.

## Documents

- `PROGRESS.md`: status, results, open issues.
- `DEVIATIONS.md`: every place this port knowingly differs from, or had to interpret, the reference.
- `ARCHITECTURE.md`, `FEATURE_MATRIX.md`, `RISKS.md`, `LICENSES.md`: phase 1 research and design.
- `tools/exporter/README.md`: regenerating the data pack and the goldens.

## Licence

Code: GPL-3.0-or-later (`LICENSE`). Profile data: CC BY-SA 4.0
(`data/SPEKTRAFILM_LICENSE.txt`). See `THIRD_PARTY_NOTICES.md`.
