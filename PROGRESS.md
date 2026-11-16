# Progress

| Phase | Scope | Status |
|---|---|---|
| 1 | Research and plan | Done, approved |
| 2 | CPU reference pipeline in C++ + parity harness against Python | Done, approved |
| 3 | RAW / image I/O, Extended features, reference gaps, Vulkan compute port, parity, benchmarks | Done, approved |
| 4 | UI + preview / final render system | **Done; GPU path verified on software Vulkan only** |
| 5 | Presets, batch, LUT export, polish | **Done** |
| 6 | Windows installer + README with build steps | **Done; the installer is a CI artifact** |

Phases 4 to 6 were done in one run without checkpoints, as instructed. What remains is one
local run on the Arc 140T (`scripts\run_local_checks.ps1`) and a tuning pass on its report.

Reference baseline: `andreavolpato/spektrafilm`, branch `dev`, commit
`6cd00c8d4f30b5b550f50f4bbd3753c9f2a48507`. The hash is recorded in the data pack
(`spectra-cli info`) and in every `cases.json`.

## Phases 4 to 6 results

### What exists now

- **Desktop app** (`apps/spectra-app`, Qt 6 Widgets): preview (fitted or 100 % with panning,
  before / after), histogram, image strip, parameter panels, export, batch export, LUT export,
  presets, settings, About with the attribution. Parameter labels, ranges, steps and tooltips
  are exported from the reference GUI's own manifest (107 fields in 14 groups), so every
  parameter the reference GUI offers is there, in basic / advanced tiers.
- **Render engine** (`libs/engine`, no UI dependency, shared by the app and the CLI): owns the
  Vulkan device, the loaded image, preview sizes, whole-image statistics, stage caches and the
  calibration; falls back to the CPU reference when there is no Vulkan device.
- **CLI additions**: `calibrate`, `bench-preview`, `batch`, `lut`, `presets`.
- **Installer**: NSIS installer and a portable zip from `cpack`; Qt is deployed with
  windeployqt, the vcpkg DLLs and the MSVC runtime are included. CI builds both, installs to a
  scratch folder, runs the installed app's self-test from a bare `PATH`, and uploads them as
  the artifact `spectra-app-windows-installer`.
- **LibRaw 0.22.0** is fetched and built at a pinned commit (the version rawpy uses).

### GPU optimisations applied without hardware numbers

| What | How | Evidence |
|---|---|---|
| Fewer submissions | Dispatches are recorded and submitted in batches, bounded by count and by pixels touched | 78 dispatches per preview go out in 16 submissions (was 78); results are bit-identical |
| Short submissions | The work bound keeps one submission to roughly 100 ms (calibrated), far from Windows' 2 s GPU watchdog, and is the granularity of cancellation | Calibration log |
| Persistent buffers | Planes are pooled and reused by size; kernel parameter buffers are host-visible | Peak GPU memory at 20 / 30 MP unchanged (below) |
| Stage cache | Film exposure, development, grain and print outputs stay on the GPU; a parameter change re-runs only what is downstream | Cached = fresh for 153 parameter changes, exactly |
| Table reuse | The spectral-upsampling LUT and the gamut boundary are memoised; the draft and refined pass share one pipeline object | About 20 ms less CPU time per slider change |
| Uploads overlap compute | A RAW file's full decode runs on a pool thread while the GPU renders the fast half-size decode; in tiled renders the next tile is cut and the previous tile stored on CPU threads while the GPU renders | — |
| Asynchronous preview | The UI thread only posts requests to the render thread. A newer request drops queued ones and cuts the refinement of the running one short at the next batch | App self-test; `engine` test (cancellation leaves no GPU memory behind) |
| Progressive preview | Every change renders a low-resolution draft first (never interrupted, so a moving slider always shows something), then the refined preview | App self-test counts both |

### Self-tuning

On first launch for a GPU (and from Settings > "Re-run performance calibration", or
`spectra-cli calibrate`) the app renders a test picture a few times and chooses:

- the **batching** (1, 8, 48 or 256 dispatches per submission, whichever is fastest) and the
  work bound that gives about 100 ms per submission;
- the **refined preview size**: the largest of 768 / 1024 / 1280 / 1600 / 1920 / 2560 px whose
  complete render is predicted under 150 ms (so the common slider change, which re-runs only
  later stages, lands well under 50 ms);
- the **draft size**: the largest of 256 to 800 px predicted under 40 ms;
- the **GPU memory budget** (60 % of what the driver reports, at most 4 GiB) and with it the
  tile size for images that do not fit.

The result is cached per device and driver version. Settings can override the preview size,
the budget and the tile size by hand.

### Parity and tests (all passing locally: 22 ctest targets, one skipped)

| Target | What | Result |
|---|---|---|
| `parity_*` (6) | CPU against the Python goldens, 1,270 cases | pass, unchanged |
| `gpu_parity_*` (5), `gpu_tiling`, `gpu_info` | GPU (lavapipe) against the Python goldens, tiling, Extended | pass, unchanged by batching, pooling and caching |
| `engine` | Stage cache against fresh renders for every parameter; cancellation; batching; previews at both levels; 100 % view against the full render; calibration; presets; LUT | pass. Cache: max difference 0 over 153 changes. 100 % view centre vs full render: 0. Draft vs refined: mean 0.0006 |
| `raw_pixls` | 7 RAW files (Canon R6 x2, Sony, Nikon x2 incl. portrait, Fujifilm X-Trans, square Panasonic): fast and full decode, orientation, preview | pass |
| `raw_canon_r6` | your own R6 files | **skipped** (folder empty), by design |
| `app_selftest`, `app_selftest_cpu` | The app, unattended and without a display: start, calibrate, preview, change a parameter, export an image and a LUT; on the Vulkan path and on the CPU fallback | pass |
| `io_roundtrip`, `sanity`, `extended_sanity`, `pack_verify` | as before | pass |

RAW decode against rawpy with LibRaw 0.22.0: every Bayer file is bit-identical; the X-Trans
file differs by 8.9e-6 mean relative (was about 2 %). See DEVIATIONS D-11.

### Numbers (Mesa lavapipe on 4 CPU cores; not representative of any GPU)

Interactive path, Canon EOS R6 sample (`spectra-cli bench-preview --in canon_r6.cr3`); the
calibration chose a 768 px preview and a 256 px draft on this renderer:

| | Draft | Refined | Target |
|---|---|---|---|
| RAW open to first preview | 0.54 s | 1.02 s | < 1.5 s |
| Slider: scanner (unsharp mask) | 17 ms | 84 ms | < 50 ms |
| Slider: print exposure / filters | 25 to 28 ms | 155 ms | < 50 ms |
| Slider: grain | 40 ms | 257 ms | < 50 ms |
| Slider: film development (couplers) | 47 ms | 328 ms | < 50 ms |
| Slider: camera exposure (everything) | 41 ms | 345 ms | < 50 ms |

Full resolution (`spectra-cli bench`):

| Image | Tiles | Peak GPU working set | Peak process memory |
|---|---|---|---|
| 20 MP (5472×3648) | 1 | 1.34 GiB | 1.90 GiB |
| 30 MP (6704×4464) | 1 | 2.01 GiB | 2.82 GiB |
| 60 MP (9488×6320) | 4 | 2.35 GiB | 6.54 GiB (limit 10 GB: OK) |

Full-render times on this renderer are as in phase 3 (about 22 s and 35 s at 20 and 30 MP).
The 60 MP process peak rose from 5.4 to 6.5 GiB because the next tile is now prepared while the
current one renders.

### Decisions taken in phases 4 to 6

| # | Decision | Why |
|---|---|---|
| 1 | Qt 6 **Widgets**, not QML; the preview is painted from an 8-bit image the render thread hands over | Simplest robust path; the GPU work is compute-only and has no swapchain to share. A zero-copy presentation path is a possible later optimisation |
| 2 | The engine is a plain C++ library without Qt | The CLI, the tests and the app drive exactly the same code |
| 3 | **Stock defaults follow the reference GUI**: choosing a film loads that stock's grain, halation and coupler presets into the editable parameters and sets "Scan film" for slide films; other adjustments are kept; later renders do not re-apply the presets | This is what `spektrafilm_gui` does (`digest_after_selection`, then `apply_stocks_specifics=False`) |
| 4 | Parameter panels are generated from the reference GUI manifest, exported by `tools/exporter/export_gui_manifest.py` | No hand-copied ranges or tooltips; the "basic" tier is the reference's |
| 5 | Preview is always sRGB; the export colour space, transfer function and HDR apply to exports | A preview in an export encoding would look wrong on the monitor |
| 6 | The draft pass is never cancelled; only the refinement is | Otherwise a fast-moving slider would show nothing until it stops |
| 7 | 100 % view renders the visible part plus a 128 px border (DEVIATIONS P-01) | The exact support of the DIR-coupler tail would triple the pixels rendered per view |
| 8 | App metering reduces by area to 1024 px before the reference's metering reduction (DEVIATIONS D-12) | The reference path costs about 5 s at 20 MP; measured difference at most 0.003 EV with the default method |
| 9 | LibRaw is **fetched at the pinned commit** by CMake rather than copied into the repository | Keeps 2 MB of third-party source out of the tree while still pinning the exact version; builds need network access on first configure |
| 10 | X-Trans: after pinning, the remaining difference is 8.9e-6 mean relative. Logged, not pursued | As instructed |
| 11 | Diffusion filter on tiled renders: unavailable in v1. The app shows a notice above the preview as soon as it applies and blocks the export with the explanation; the CLI errors | As instructed. Tileable version: v2 |
| 12 | Canon R6 test set: `raw_canon_r6` is a separate ctest target that reports "skipped" when the folder is empty | As instructed |
| 13 | Product name: one CMake variable, `SPECTRA_PRODUCT_NAME`; everything visible follows it. The executable and settings folder keep the neutral id `spectra-app` | As instructed |
| 14 | Presets store the difference from the defaults; loading starts from the stock defaults of the preset's film and print. Built-in presets are film / print pairings only | Presets stay valid when defaults or stock data change; no invented "looks" |
| 15 | Per-image edits are remembered in the user's application-data folder (keyed by file path), not in sidecar files | No files appear next to the user's photos |
| 16 | LUT export uses the reference's `lut_mode` (no spatial or stochastic effects, no autoexposure), with sRGB, Rec.709, Display P3 or ACEScct input | A LUT cannot hold grain or halation; ACEScct gives a scene-referred option |
| 17 | Installer: NSIS through CPack; unsigned | No signing certificate. SmartScreen will warn (README says how to proceed) |
| 18 | CI installs Qt with `jurplel/install-qt-action` (Qt 6.8.3); the local script can fetch the same with aqtinstall (`-InstallQt`) | Building Qt through vcpkg would take hours per cache miss |
| 19 | Windows CI runs the app's self-test on the CPU fallback (no Vulkan device there); Linux CI runs it on lavapipe | Both paths get exercised on every push |

### Not verified on real hardware

Nothing below can be judged from a software renderer:

1. **Every timing.** The 50 ms slider target, the 1.5 s open-to-preview target and the
   full-resolution times on the Arc 140T.
2. **What the calibration picks** on the Arc, and whether its choices feel right.
3. **Intel driver behaviour**: shader compilation of the eleven kernels, long dispatches (the
   recursive Gaussian runs a whole image line per invocation), device-lost handling, the
   `VK_EXT_memory_budget` figures on shared memory.
4. **Memory on shared-memory graphics**: GPU allocations of an integrated GPU come out of
   system RAM, so the 10 GB process limit has to be read together with the GPU figure.
5. **The UI on a real display**: high-DPI scaling, the look of the dark theme on Windows,
   panning smoothness, drag and drop from Explorer, file dialogs.
6. **The installer on a clean machine**: CI installs to a folder and runs the app from there
   with a bare `PATH`, but it does not run the NSIS installer itself, and the runner has no GPU.
7. **`scripts\run_local_checks.ps1`**: CI parses it and dry-runs it (without a GPU, so its GPU
   steps fail there by design). The Qt download path (`-InstallQt`) has not been run.

### The one local run

1. Install what `docs/WINDOWS_SETUP.md` lists (Visual Studio 2022 with the C++ workload, Git,
   a current Intel driver, Python).
2. Optionally copy your R6 `.CR3` files into `test_data\raw\canon_r6\`.
3. In "Developer PowerShell for VS 2022", in the repository:
   `Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass`, then
   `.\scripts\run_local_checks.ps1 -InstallQt`.
4. Send back `reports\local_check_<stamp>.txt` and `reports\calibration_<stamp>.json`.

## Open issues

1. **Unverified on real hardware**: the list above.
2. **Refined-preview latency depends on the GPU.** On the software renderer only the draft pass
   meets 50 ms. The design assumes the Arc is far faster than 4 CPU cores; if it is not, the
   tuning pass has more to do (candidates: half-float planes, fusing the pointwise kernels,
   presenting from the GPU instead of reading back).
3. **CPU work per slider change** is about 20 ms on this machine (rebuilding the pipeline
   tables). Fine for the draft, but it is a floor under every change; the remaining cost is
   mostly the film / print curve preparation and could be cached per stage as well.
4. **Diffusion filter**: runs on the CPU inside the GPU path (slow preview while it is on) and
   is unavailable on tiled renders.
5. **CPU fallbacks inside the GPU path**: `jzazbz` / `cam16ucs` gamut compression and
   `mallett2019` upsampling (none is a default).
6. **Exports carry no ICC profile and no EXIF.** Non-sRGB exports are untagged.
7. **No crop, rotate or straighten tool**, and no lens correction.
8. **The installer is unsigned.**
9. **X-Trans** decode: 8.9e-6 mean relative difference from rawpy remains; the full X-Trans
   demosaic takes about 11 s on this machine (the fast preview is unaffected).
10. **Tiled grain unsharp mask** uses per-tile sums (RMS 1e-4), as in phase 3.
11. Reduced previews show finer grain than the 100 % view (DEVIATIONS P-02).
12. First configure needs network access (vcpkg ports, LibRaw).

## Phase 3 results

### 3a. RAW and image file I/O

- `libs/io`: LibRaw decode to linear ACES2065-1 (a port of the reference's raw processor:
  as-shot, daylight, tungsten and custom temperature / tint white balance; colour matrix, black /
  white level and orientation from LibRaw's per-camera metadata), a half-size decode for fast
  previews, and TIFF (8 / 16 / 32-bit float), PNG (8 / 16), JPEG and EXR (half / float) read and
  write.
- `scripts/fetch_test_raws.py` downloads seven CC0 samples from raw.pixls.us with checksum
  verification; sources and licences are in `test_data/SOURCES.md`. Nothing large is committed.
- RAW decode compared with the Python reference (`tools/parity/check_raw.py`, full resolution):

  | File | Size | Pixels bit-equal | Mean relative difference |
  |---|---|---|---|
  | Canon EOS R6 (CR3) | 5496×3670 | > 99.9 % | 6e-7 |
  | Canon EOS R6 (C-RAW) | 5496×3670 | > 99.9 % | 4e-7 |
  | Sony ILCE-7M2 | 6024×4024 | > 99.9 % | 5e-7 |
  | Nikon D750 | 6032×4032 | > 99.9 % | 6e-7 |
  | Nikon Df, portrait orientation | 3292×4940 | > 99.9 % | 6e-7 |
  | Panasonic LX100, square | 3104×3104 | > 99.9 % | 1e-6 |
  | Fujifilm X-T3 (X-Trans) | 6246×4170 | **no** | **2e-2** (open issue 1) |

  The R6 file was also checked with daylight, tungsten and custom white balance (same result).
  The white-balance maths has its own goldens in `t1_units`.

### 3b. Extended features (CPU and GPU)

Push / pull (standard and experimental, film and print), bleach bypass (film and print, with
leuco-cyan loss) and HDR output (PQ and HLG, shoulder or clip tone map). Adapted from
spektrafilm-ofx; each has its own switch and is off by default. Sanity tests
(`tests/test_extended.cpp`) on four routes (colour negative, cine, slide scan, B&W), 22
settings each:

| Feature | Finite, in [0, 1] | Monotonic (worst drop) | Grey cast added (worst) | Exact no-op when off |
|---|---|---|---|---|
| Push / pull, standard | yes | 2e-8 | 0.020 | yes |
| Push / pull, experimental | yes | 1.4e-4 | 0.024 | yes |
| Print push / pull | yes | 3e-10 | 0.018 | yes |
| Bleach bypass, film | yes | 1e-7 | 0.029 | yes |
| Bleach bypass, print | yes | 0 | 0.038 | yes |
| HDR PQ / HLG | yes | 0 | 0.004 | yes |

"Grey cast" is the increase of max − min channel on a 13-stop grey ramp over the same render
without the feature. Two design points are explained in `DEVIATIONS.md` (B3): standard push
scales the exposure axis instead of using the reference's `gamma_factor`, and the print is
re-timed for mid-grey.

### 3c. Reference gaps closed

`cam16ucs` output gamut compression and `io.upscale_factor` (cubic B-spline resampling) are
ported and verified against Python. All six gamut algorithms now match.

### 3d. Vulkan compute backend

- `libs/gpu`: a vendor-neutral Vulkan 1.1 context (no vendor extensions; device chosen at run
  time; unified-memory devices map buffers directly, discrete ones go through a staging buffer)
  and eleven compute kernels in `shaders/`. Compiled SPIR-V is committed, so a build needs no
  Vulkan SDK.
- `GpuPipeline` shares every baked table with the CPU reference (one source of truth).
- Grain on the GPU is the reference model: per channel and sub-layer compound Poisson-Binomial
  counts from the same Philox generator as the CPU, dye-cloud blur, blur, mass-conserving
  unsharp mask. No noise overlay.
- Tiling: automatic from the memory budget (60 % of the device budget, at most 4 GiB), manual
  override (`--tile`, `--budget-mb`). Tiles are padded by the support of every active effect.
  Random streams are keyed by absolute pixel position, so grain does not change with tiling.
- Approximations and CPU fallbacks are listed with their evidence in `DEVIATIONS.md` (section B).

### Parity status

CPU reference vs Python (float64), all passing:

| Suite | Cases | Worst max abs | Threshold |
|---|---|---|---|
| `t1_units` | 391 | 1.5e-10 (2.5e-6 float32 FFT cases, 7e-8 float32 white balance) | 1e-9 (3e-4 / 3e-7) |
| `t2_nonspatial` | 255 | 1.5e-14 | 1e-9 |
| `t3_spatial` | 44 | 1.7e-13 (1.3e-6 FFT cases) | 1e-9 (3e-4) |
| `t5_sweeps` | 199 | 7.8e-13 | 1e-9 |
| `fast_preview` | 20 | 1.5e-14 vs GUI settings; 0.0034 vs reference | 1e-9; 0.015 |
| `t4_stochastic` | 361 | all inside the statistical limits (worst margin 0.32) | see phase 2 table below |

GPU backend vs Python (float32), on Mesa lavapipe (software Vulkan). **Your Arc 140T has not
run these yet.**

| Suite | Cases | Result | Worst max abs | Threshold |
|---|---|---|---|---|
| `t2_nonspatial` (every film × every print) | 255 | all pass | 9e-6 | 2e-3 |
| `t3_spatial` | 44 | 43 exact + 1 on the wide-Gaussian approximation (RMSE 0.0017) | 1.8e-3 on an intermediate density tap; 1.4e-4 on final images | 2e-3; approx: RMSE 0.003, p99 0.01 |
| `t5_sweeps` | 199 | all pass | 1.1e-5 | 2e-3 |
| `fast_preview` | 20 | all pass | 0.0042 | 0.015 |
| `t4_stochastic` (grain, glare, samplers) | 361 | all pass | worst margin 0.49 | as CPU |

Other GPU checks (`tests/test_gpu.cpp`): GPU vs CPU with deterministic effects 1.4e-4; the
Extended features on the GPU against their CPU implementation, at most 8.6e-4; tiled vs
untiled 6e-6 without grain, RMS 1e-4 with grain; a small budget switches to tiles and the peak
stays inside it; a crop rendered with its origin reproduces the full render's grain exactly.

### Benchmarks

Measured here on **Mesa lavapipe: the Vulkan code running on 4 CPU cores**. These numbers say
nothing about the Arc 140T; they are here for the memory figures and as a baseline. The
targets (preview < 50 ms, RAW open to preview < 1.5 s) can only be checked on your machine.

| Image | Full-resolution render | Tiles | Peak GPU working set | Peak process memory | Preview (1600 px, complete pipeline) |
|---|---|---|---|---|---|
| 20 MP (5472×3648) | 22.4 s | 1 | 1.34 GiB | 1.90 GiB | 1.78 s |
| 30 MP (6704×4464) | 36.0 s | 1 | 2.01 GiB | 2.82 GiB | 1.60 s |
| 60 MP (9488×6320) | 101 s | 4 | 2.25 GiB | 5.38 GiB | 1.71 s |

On this software renderer "GPU memory" is ordinary RAM, so the process figure already includes
it. 20 to 30 MP stays untiled and well under the 4 GiB target; 60 MP tiles automatically and
stays under the 10 GB limit (`ctest -L memory` and the local-check script fail above it). The
working set is about 74 bytes per pixel.

RAW open to first preview on this container (Canon EOS R6, 1600-px preview): half-size decode
about 200 ms warm (950 ms cold cache), reduce 12 ms, tables + render on software Vulkan 550 ms
at 800 px. On a real GPU the render term is the part that should shrink.

## Phase 2 results (CPU reference; unchanged)


### What exists

- `tools/exporter/export_pack.py`: writes `data/spektrafilm.sfpk` (378 arrays, 6.3 MB) from the
  pinned Python package: all 31 profiles, grain / coupler presets, neutral print filters, filter
  and illuminant spectra, the spectral upsampling LUT, colour-space and adaptation matrices.
  Every array carries a CRC32; `spectra-cli pack-verify` checks them.
- `libs/core`, `libs/data`, `libs/model`, `libs/pipeline`: the CPU reference pipeline in C++20,
  float64 throughout, no dependencies beyond two vendored headers. Ported one to one:
  parameter schema and digest (presets, neutral filters, preview and debug switches), density
  curve model, B&W development-time selection, film and print chemistry morph, Hanatos 2025
  upsampling (window, surface, spectral blur, input gamut compression) and Mallett 2019,
  auto-exposure (7 methods), highlight boost, diffusion filter (4 families), lens blur, scatter
  and halation, DIR couplers with Langmuir saturation, grain, print exposure (direct spectral,
  pre-flash, exposure normalisation), print development, scanning, base tuning, black / white
  correction, glare, output gamut compression, scanner blur and unsharp mask, the 3-D / 1-D LUT
  acceleration, cropping, and the tap system.
- `tools/parity/make_golden.py` + `spectra-cli parity`: the parity harness. Goldens are
  generated by the pinned Python reference and committed (27 MB), so the tests need no Python.
- `tests/test_sanity.cpp`: checks that need no reference.
- `.github/workflows/ci.yml`: builds and tests on Linux and Windows (MSVC); both pass.

### Parity status (all passing)

Measured on Linux x86-64, GCC 13; the same suites pass on Windows with MSVC in CI. "max abs" is the largest absolute difference over all values
of all cases in the group, in the units of the tap (encoded RGB for final outputs).

| Suite | Cases | What | Worst max abs | Threshold |
|---|---|---|---|---|
| `t1_units` | 372 | Illuminants, CCTFs, adaptation matrices, Gaussian / exponential filters, metering preview, auto-exposure, boost, diffusion PSF, halation, crop, `tc_lut`, curves and morphs, grain particle counts, DIR curve inversion, digest for every film | 1.5e-10 (2.5e-6 for the float32 FFT cases) | 1e-9 relative (3e-4 FFT) |
| `t2_nonspatial` | 255 | Every film × every print (22 × 9), scan-film for every film, every tap for 7 pairs, B&W and mixed colour / B&W | 1.5e-14 | 1e-9 |
| `t3_spatial` | 42 | Halation, scatter, boost, DIR diffusion, lens / scanner blur, unsharp mask, crop, camera and enlarger diffusion filters, portrait-orientation and odd-sized images | 1.7e-13 (1.3e-6 FFT) | 1e-9 (3e-4 FFT) |
| `t5_sweeps` | 193 | About 90 parameters one at a time on two stock pairs, positive film, B&W development times, metering on a real image | 7.8e-13 | 1e-9 |
| `fast_preview` | 20 | Python GUI settings (17³ LUTs, preview mode): vs Python with the same settings | 1.5e-14 | 1e-9 |
| | | Same C++ output vs the Python **reference** (no LUTs) | max 0.0034, p99 0.0021, RMSE 0.0006 | max 0.015, p99 0.006, RMSE 0.003 |
| `t4_stochastic` | 361 | See below | all within limits | see below |

Stochastic effects (random numbers cannot match NumPy's, so the comparison is statistical;
"margin" is the worst observed difference as a fraction of the allowed one):

| Check | Cases | Limit | Worst margin |
|---|---|---|---|
| Grain count distribution: exact Poisson / binomial samplers vs the analytic pmf, fast samplers vs Python's `fast_stats` | 23 | 1.2e-3 to 1.5e-3 per bin, 2 M draws | 0.32 |
| One sub-layer, mean and RMS vs density (8 densities × coarse / medium / fine grain × exact / fast) | 12 | mean 1 %, std 3 % | within |
| Full grain model on 768² flat patches, 6 stocks × 10 exposure levels (+ fast mode for 2 stocks): mean density | 80 | 0.005 D | 0.31 |
| Same: per-pixel standard deviation | 80 | 5 % | 0.15 |
| Same: RMS granularity through a 48 µm aperture | 80 | 5 % | 0.30 |
| Same: radial power spectrum, 8 bands | 80 | 10 % per band | 0.32 |
| Print glare: mean, std, spectrum | 6 | 0.2 % / 5 % / 10 % | within |

Sanity tests: finite output for colour negative, cine, slide and B&W routes; identical output
for the same seed; identical output with 1 or N threads; different grain for a different seed;
resolution independence (mean difference between a 1× and a 2× render of the same scene:
0.0023 by default, 0.0047 in match-Python mode); a crop rendered with the full image's
statistics equals the full render to 1e-12.

### Numbers

- CPU reference, 20 MP (5472×3648), all default effects including grain: **43.8 s, 3.8 GB peak**
  on 4 container cores. This is the unoptimised double-precision oracle, not the product path.
- Test suite: 8 ctest targets, 72 s.

### Decisions applied in this phase

- Decision 4 (fidelity): every effect is a port of the reference equations. No substitutes.
- Decision 5 (parity): reference defaults are the primary target; the fast-preview suite covers
  the GUI settings against both the GUI configuration and the reference.
- Decision 6 (resolution): `RenderOptions.match_python` (off by default, on in parity tests);
  pixel-unit parameters scale with pixel pitch otherwise. `RenderStats` / `RenderOptions` carry
  the auto-exposure EV and boost maximum from a full render into previews and crops.
- Decision 9 (camera-agnostic): nothing in the pipeline depends on image size or aspect.

## Differences from the phase 1 architecture

- The pack stores CRC32 per array instead of xxh3, and is read into memory instead of mmapped.
- nlohmann/json and pocketfft are vendored headers; LibRaw, the image libraries and Vulkan come
  from vcpkg on Windows and from distribution packages on Linux.
- GPU planes are storage buffers of floats rather than `VkImage`s, and there is no VMA: one
  allocation per buffer is enough at this stage.
- Wide Gaussians use the reference's recursive filter on the GPU instead of the FIR pyramid
  the architecture proposed; the diffusion filter still runs on the CPU.
- `libs/bake` is folded into `libs/model` and `libs/pipeline` for now.
- Colour-space coverage is the Python GUI list (7 spaces). The larger OFX list is a phase 5 item.

## Product name

The name must not contain "spektrafilm". **Spectra (working title)** stays until you decide.
It is defined once, as `SPECTRA_PRODUCT_NAME` in `CMakeLists.txt`.
Proposals (none checked against trademarks yet):

1. **Emulsa**
2. **Latentia** (from "latent image")
3. **Dyecloud**
4. **Argent Room**
5. **Halide Bench**

## v2 backlog

- Tileable diffusion filter (and a GPU version of it).
- ICC profile embedding and EXIF copy on export; lens correction; crop / rotate tool.
- Zero-copy preview presentation; half-float planes; per-stage caching of the CPU tables.
- Code signing for the installer.

- Experimental-branch negative-scan workflow (`workflow.route`, convert-film). The stage
  sequence in `Pipeline::process` is route-driven so it can be added.
- New upsampling methods of the experimental branch (same 2-D LUT plumbing).
- Printer lights / APD: excluded (needs licensed SMPTE data).
