# Spectra (working title)

A native, GPU-accelerated film simulation app for Windows, built as a faithful port of
[spektrafilm](https://github.com/andreavolpato/spektrafilm) by Andrea Volpato.
Film simulation powered by spektrafilm profiles (CC BY-SA 4.0).

**Status: phase 3 of 6.** The CPU reference pipeline, RAW and image file I/O, the Extended
features and the Vulkan compute backend are done and verified against the Python reference
(on the CPU, and on a software Vulkan renderer). There is no user interface yet. See
`PROGRESS.md`.

## Build and test

Requirements: CMake 3.24+, a C++20 compiler (GCC 13, Clang 18 or MSVC 2022).

**Windows:** see `docs/WINDOWS_SETUP.md`. One script builds, tests and benchmarks on your GPU:

```powershell
.\scripts\run_local_checks.ps1
```

**Linux (Ubuntu 24.04):**

```bash
sudo apt-get install -y libraw-dev libtiff-dev libpng-dev libjpeg-turbo8-dev libopenexr-dev libvulkan-dev mesa-vulkan-drivers glslc
```

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
```

```bash
cmake --build build --parallel
```

```bash
ctest --test-dir build --output-on-failure
```

Without the image libraries or Vulkan the core still builds; file I/O and the GPU backend are
switched off with a warning. Building and testing needs no Python: the data pack, the goldens
and the compiled shaders are in the repository.

## Command-line tool

```bash
build/spectra-cli info
```

```bash
build/spectra-cli render --in photo.CR3 --out photo.tif --film kodak_portra_400 --print kodak_portra_endura --backend gpu
```

```bash
build/spectra-cli bench --backend gpu --mp 20,30 --in photo.CR3
```

```bash
build/spectra-cli parity tests/golden/t2_nonspatial/cases.json --backend gpu -v
```

Inputs: camera RAW (through LibRaw), TIFF, PNG, JPEG, EXR, `.npy`. Outputs: TIFF (8 / 16 / 32-bit
float), PNG, JPEG, EXR, `.npy`. RAW test files are fetched with `python scripts/fetch_test_raws.py`
(sources and licences in `test_data/SOURCES.md`).

## Documents

- `PROGRESS.md`: status, results, open issues.
- `DEVIATIONS.md`: every place this port knowingly differs from, or had to interpret, the reference.
- `ARCHITECTURE.md`, `FEATURE_MATRIX.md`, `RISKS.md`, `LICENSES.md`: phase 1 research and design.
- `tools/exporter/README.md`: regenerating the data pack and the goldens.
- `docs/WINDOWS_SETUP.md`: what to install on Windows and how to run the local GPU checks.

## Licence

Code: GPL-3.0-or-later (`LICENSE`). Profile data: CC BY-SA 4.0
(`data/SPEKTRAFILM_LICENSE.txt`). See `THIRD_PARTY_NOTICES.md`.
