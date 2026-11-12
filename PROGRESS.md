# Progress

| Phase | Scope | Status |
|---|---|---|
| 1 | Research and plan | Done, approved |
| 2 | CPU reference pipeline in C++ + parity harness against Python | Done, approved |
| 3 | RAW / image I/O, Extended features, reference gaps, Vulkan compute port, parity, benchmarks | **Done on software Vulkan; awaiting your GPU run and approval** |
| 4 | UI + preview / final render system | Not started |
| 5 | Presets, batch, LUT export, polish | Not started |
| 6 | Windows installer + README with build steps | Not started |

Reference baseline: `andreavolpato/spektrafilm`, branch `dev`, commit
`6cd00c8d4f30b5b550f50f4bbd3753c9f2a48507`. The hash is recorded in the data pack
(`spectra-cli info`) and in every `cases.json`.

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

### What I need from you

1. **Run `scripts\run_local_checks.ps1`** on the 285H / Arc 140T and paste the report
   (`docs/WINDOWS_SETUP.md` lists what to install: Visual Studio 2022 with the C++ workload,
   Git, a current Intel driver, optionally Python). No Vulkan SDK and no Qt are needed yet.
2. **Your R6 files** in `test_data\raw\canon_r6\` before that run, if you have them ready
   (8 scenes listed in `test_data/SOURCES.md`). Two public R6 samples are already in the set.

## Open issues

1. **Fujifilm X-Trans decode differs from the reference by about 2 %** (other brands match
   exactly). The reference's rawpy bundles LibRaw 0.22.0; this build links LibRaw 0.21.2. I
   could not build 0.22 here to confirm that is the cause. To check on Windows with whatever
   LibRaw vcpkg provides, and pin 0.22 if it resolves it.
2. **The GPU backend has only run on a software renderer.** Driver-specific problems on the
   Arc (shader compiler, long dispatches) are unknown until the local run. One known risk: the
   recursive Gaussian kernel runs a whole image line per invocation; if a single dispatch takes
   longer than Windows' 2-second GPU watchdog at 20 to 30 MP, it has to be split.
3. **Interactive preview is not built yet.** The benchmark reports the complete pipeline at
   preview size and derives per-slider costs from the stage timings. Stage caching (re-running
   only the stages after the changed parameter), batching dispatches into one submission, and
   the half-float storage options from the architecture are phase 4 work, and are what the
   50 ms target depends on.
4. **CPU fallbacks inside the GPU path**: diffusion filters, `jzazbz` / `cam16ucs` gamut
   compression, `mallett2019` upsampling. All are off by default. Diffusion filters cannot be
   tiled yet (the render stops with a clear error).
5. **Tiled grain unsharp mask** uses per-tile sums instead of whole-image sums (measured
   effect: RMS 1e-4).
6. Not ported: lens correction (lensfun), EXIF copy and ICC embedding on save.
7. Windows CI now needs vcpkg to build LibRaw, OpenEXR and the Vulkan loader; the first run
   after this push will show whether that works.
8. CPU fp32 / SIMD fallback backend: still deferred.

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

The name must not contain "spektrafilm". **Spectra (working title)** stays until you decide
(before phase 5).
Proposals (none checked against trademarks yet):

1. **Emulsa**
2. **Latentia** (from "latent image")
3. **Dyecloud**
4. **Argent Room**
5. **Halide Bench**

## v2 backlog

- Experimental-branch negative-scan workflow (`workflow.route`, convert-film). The stage
  sequence in `Pipeline::process` is route-driven so it can be added.
- New upsampling methods of the experimental branch (same 2-D LUT plumbing).
- Printer lights / APD: excluded (needs licensed SMPTE data).
