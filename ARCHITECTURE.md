# Architecture proposal — spectra‑app

A native Windows photo app (C++20, Vulkan compute, Qt 6) that implements the spektrafilm pipeline.
The Python repo (`dev`) is the **algorithmic reference**. The OFX port is the **GPU and performance
reference**. Phase 1 document: design only, no code yet.

---

## 0. Target hardware and the constraints it imposes

**Intel Core Ultra 9 285H (Arrow Lake‑H)**
- CPU: 16 cores (6 P + 8 E + 2 LP‑E), AVX2/FMA, **no AVX‑512**.
- iGPU: **Arc 140T**. Xe‑LPG+, 8 Xe‑cores, about 4–5 TFLOPS FP32 (estimated), 2× rate at FP16.
  Variable subgroup size 8/16/32. Vulkan 1.3 on the Windows driver.
- NPU: unused.
- Memory: **unified** DDR5 / LPDDR5x, about 90–135 GB/s, shared between CPU and GPU. Vulkan
  exposes one heap (`DEVICE_LOCAL`), and its memory types include
  `DEVICE_LOCAL | HOST_VISIBLE | HOST_COHERENT`, with a `HOST_CACHED` variant. WDDM lets the iGPU
  use about half of system RAM.

What follows from this:
1. **Memory bandwidth, not ALU, is the bottleneck.** A full‑resolution RGBA32F read + write at
   24 MP moves about 0.77 GB, roughly 7–8 ms on its own. The design therefore minimises full‑res
   plane passes: kernel fusion, FP16 storage where it is provably safe, and low‑resolution
   pyramids for wide blurs.
2. **No PCIe**, so staging copies are pure waste. Decode straight into GPU‑visible memory and read
   results back from cached host‑visible memory.
3. **Long dispatches risk Windows TDR** (2 s default). Big jobs are split into multiple submits.
4. **CPU work competes for the same bandwidth.** Avoid heavy CPU work (RAW decode, thumbnails)
   while an interactive GPU frame is running.

---

## 1. Repository and module structure

```
spectra-app/
├─ CMakeLists.txt, CMakePresets.json, vcpkg.json   # MSVC 2022 / clang-cl, Ninja, vcpkg manifest
├─ apps/
│  ├─ spectra/            # Qt 6 GUI executable (thin: wires ui + engine)
│  └─ spectra-cli/        # headless: batch render, pack inspection, parity runner
├─ libs/
│  ├─ core/               # no deps beyond std: math (erf, normal CDF, Brent, interp, Mitchell),
│  │                      # spectral grid, colour math (matrices, CCTFs, OkLab, JzAzBz, CAM16-UCS),
│  │                      # Params schema (C++ mirror of RuntimePhotoParams), hashing
│  ├─ data/               # .sfpk pack loader (mmap), ProfileDb, presets, filters, illuminants,
│  │                      # optional direct loader for upstream profile JSON
│  ├─ model/              # physics, CPU reference implementations (templated on Real = float|double):
│  │                      # curves+morph, couplers, grain, halation, diffusion PSF, glare, upsampling,
│  │                      # enlarger, scanner, gamut compression, autoexposure, B/W correction
│  ├─ bake/               # parameter-dependent precompute ("digest"): builds every table the
│  │                      # kernels need (tc_lut, curves, DIR inverse, response tables, C_max, PSF)
│  ├─ pipeline/           # stage graph, taps, hash-based invalidation, tile planner, job scheduler
│  ├─ backend_gpu/        # Vulkan device, VMA, resource pools, pipelines, per-stage dispatchers
│  ├─ backend_cpu/        # same stages executed with oneTBB + Highway SIMD (fallback & oracle)
│  ├─ io/                 # OIIO image I/O, LibRaw, lensfun, exiv2, lcms2 (ICC, display profile)
│  └─ ui/                 # Qt widgets, parameter manifest, viewer (Vulkan surface), histograms
├─ shaders/               # GLSL 4.60 compute → SPIR-V (glslc at build time)
├─ tools/
│  ├─ exporter/           # Python: spektrafilm (pinned SHA) → data/spektrafilm.sfpk
│  └─ parity/             # Python: golden generation + comparison reports
├─ tests/                 # unit (GoogleTest), golden, parity, perf
├─ data/                  # generated pack + SPEKTRAFILM_LICENSE.txt + CHANGELOG.txt
└─ LICENSES/, THIRD_PARTY_NOTICES.md
```

Dependency direction: `core ← data ← model ← bake ← pipeline ← {backend_gpu, backend_cpu} ← ui ← apps`.
`model` has no Vulkan or Qt dependency. That keeps it testable on Linux CI and lets it serve as the
parity oracle.

**Language and tooling:** C++20, MSVC 2022 (with clang‑cl as a secondary compiler), CMake ≥ 3.28,
vcpkg manifest mode. Shaders are GLSL compiled with `glslc`. GLSL keeps OFX shader code reusable;
Slang can be revisited later if a single source for CPU and GPU becomes attractive.

---

## 2. Engine data model

- **`Params`** mirrors the Python `RuntimePhotoParams` field for field: same names, same units, no
  GUI‑only transforms. This is also the upstream GUI's design principle. Serialisation is JSON
  with a `schema_version`, and a migration table keeps old preset and state files loadable.
- **`Digest`** is the C++ port of `digest_params`. It applies the neutral‑filter database, the
  halation, grain and coupler presets, and the debug / lut_mode overrides. This is a pure function
  `Params → Params`, unit‑tested against Python.
- **`Bake`** (§4.3) is a pure function from `(digested Params, ProfileDb)` to immutable `Tables`.
  It is content‑hashed, so identical parameters never re‑bake.
- **Stage graph.** Nodes and taps match the Python topology (`rgb_in, rgb_pre, log_e_film,
  cmy_film, log_e_print, cmy_print, rgb_out`), plus finer internal cache points. Routes ("film →
  print → scan", "film → scan") are data, not code, which leaves room for the experimental
  `workflow.route` set (convert‑film) later.

---

## 3. GPU pipeline design

### 3.1 Stages, kernels and fusion

Below, "plane" means one full‑resolution image of RGBA (or R for B&W) values. Every stage below is
one or a few fused compute kernels.

| Stage | Kernel(s) | Reads | Writes | Notes |
|---|---|---|---|---|
| A. **Expose** | `k_expose`: CCTF decode → input→XYZ (CAT16 to film reference illuminant, precomputed 3×3) → xy → tri→quad → **Mitchell bicubic sample of `tc_lut` (192², RGBA32F texture)** × (X+Y+Z) → × 2^(EV + AE). Also produces a subgroup max for highlight boost. | source | `raw` (RGBA32F) | Mallett path = 3×3 matrix. Per‑workgroup max feeds a 2nd tiny pass for the global max. |
| A′. Boost | fused into the first read of the next spatial pass (or standalone if no spatial effect is on) | raw | raw | Needs the **global max**, so it always runs after A completes. |
| B. **Film spatial** | camera diffusion → lens blur → scatter (core + 3‑Gaussian tail) → halation bounces | raw | raw | §3.5. All linear and energy‑preserving. Large σ uses the pyramid. |
| C. **Develop** | `k_develop_dir_src`: log10 → interpolate normalised curves → density → DIR donor (Langmuir) · M → `corr`; `corr` blurred (Gaussian + tail); `k_redevelop`: log_raw − corr (+ receiver Langmuir) → interpolate **pre‑corrected** curves | raw | `dens_film` | Curve lookup: uniform 256‑sample grid → direct index, no binary search. The Python grid is uniform; OFX's binary search is unnecessary here. |
| D. **Grain** | `k_grain_layers` (per channel × sub‑layer: interpolate layer density, Poisson‑Binomial sample, write 9 planes or 3 for B&W) → per‑layer dye‑cloud blur → `k_grain_resolve` (sum, micro‑structure clumping field, blur) → multiplicative USM (with global per‑channel mass sums) → − d_min | dens_film | dens_film | §3.6. Memory heavy: 9 R16F planes ≈ 9 × 2 B × N. |
| E. **Print expose** | `k_print`: **direct 81‑λ spectral loop**: Σλ 10^(−c·ε_λ) · R_λ, where R_λ = T_base·illuminant·filters·paper sensitivity is precomputed (81×RGB). × mid‑grey factor, + pre‑flash, × print_exposure, × B&W correction | dens_film | `raw_print` | Same trick as OFX's `filteredEnlargerResponse`. `10^x` becomes `exp2(x·log2 10)`. An optional 17³/33³ 3D‑LUT path exists for the CPU fallback and low‑power mode. |
| F. Print spatial | enlarger diffusion, enlarger lens blur | raw_print | raw_print | |
| G. **Print develop + scan** | `k_scan`: log10 → morphed print curves → print density → Σλ 10^(−d·ε_λ) · S_λ (S_λ = T_base_tuned·viewing illuminant·CMF/norm, 81×XYZ) → B/W Y correction → **glare** (precomputed low‑res random field, bilinear) → XYZ→RGB (precomputed CAT matrix) → **output gamut compression** (C_max(L,h) 2D table) | raw_print | `lin_out` | One fused kernel: two curve lookups and one 81‑λ loop. |
| H. **Post** | scanner blur → unsharp mask → CCTF encode → (saving conversion) | lin_out | `out` | |
| I. Present | colour‑managed to the monitor ICC (3D LUT built with lcms2), zoom/pan, before/after split | out | swapchain | Sampled directly from GPU memory. No readback for display. |

The "scan film" route skips E–F and runs G with the film tables.

**Spectral loop cost:** 2 × 81 × (dot3 + exp2 + 3 FMA) ≈ 1.3 kFLOP per pixel, so about 30 GFLOP at
24 MP. That is ALU‑cheap on the 140T; exp2 throughput dominates. Measured OFX numbers on its
baseline (a Metal device) were about 15 ms (1080p) and about 67 ms (4K) for the whole "default
final" pass. Keep the per‑pixel loop as the exact path.

**Shader conventions:**
- `local_size` is 16×16 (or 32×8), with subgroup size requested through
  `VK_EXT_subgroup_size_control` (core in 1.3) for reductions.
- Channel count (1 or 3), sub‑layer count, wavelength count (81) and `use_lut` are
  **specialization constants**, so B&W gets its own pipeline variant without branching.
- A per‑frame UBO holds the scalar params, push constants hold per‑dispatch ops, and tables live in
  storage buffers or textures. The pipeline cache is persisted to `%LOCALAPPDATA%`.

### 3.2 Stage caching and invalidation

Each cache point stores its output plane (at preview resolution always, at full resolution when
memory allows) plus a **hash** of `(upstream hash, the params subset this stage reads, table
hashes)`. When a parameter changes, only stages whose hash changed re‑run.

| Cache point | Invalidated by |
|---|---|
| `src` (decoded, oriented, cropped/upscaled proxy) | file, RAW WB/tint/lens, rotation, crop, upscale, preview size |
| `raw_exposed` (after A/A′) | input colour space/CCTF, upsampling method/window/surface/spectral blur, input gamut, camera colour filter, **film stock**, EV, auto‑exposure, boost |
| `raw_film` (after B) | camera diffusion, lens blur, scatter, halation, film format |
| `dens_film_nograin` (after C) | film chemistry/dev‑time, DIR couplers |
| `dens_film` (after D) | grain params, seed |
| `raw_print` (after E/F) | print stock, enlarger illuminant/filters/exposure/compensation/pre‑flash, enlarger diffusion/blur |
| `lin_out` (after G) | print chemistry, base tuning, scanner B/W correction, glare, output colour space, output gamut compression, scan_film |
| `out` (after H) | scanner blur/USM, output CCTF |
| present | display ICC, zoom, view mode |

Some examples. Dragging "print exposure" re‑runs E → H only (about 1/3 of the work). Dragging
"unsharp amount" re‑runs H only. Switching the film stock re‑runs everything except `src`. Grain
uses a fixed per‑image seed, so cache hits stay visually stable.

### 3.3 Parameter‑dependent precompute ("bake")

These tables are built on the CPU in `libs/bake` (microseconds to milliseconds each) and uploaded
as small buffers or textures. Each is content‑hashed and cached in an LRU.

| Table | Size | Built from |
|---|---|---|
| `tc_lut` | 192×192×3 (fp32) | spectra LUT (fp16, 192×192×81) ⊗ (sensitivity × colour filter × window), × 2^surface, **input gamut remap‑resample** (port of `remap_tc_lut_for_compression`) |
| Film curves (morphed) | 256×C plus layers 256×3×C | model params, dev‑time, s023 morph (erf, Gumbel, Brent offset) |
| DIR | 3×3 matrix, pre‑corrected curves 256×C, Langmuir K / D_ref / Kr / c_ref | port of `couplers.py` |
| Grain | particle counts and d_max per (sub‑layer, channel); od per particle; dye‑cloud σ per layer | port of `_layer_grain_params` (exact curve‑based peak) |
| Print response | R_λ (81×3), pre‑flash raw (3), mid‑grey exposure factor (3) | illuminant, dichroics (CC), paper sensitivity, film base, mid‑grey reference density |
| Print curves (morphed) | 256×C | print model + chemistry |
| Scan response | S_λ (81×3) with tuned base, XYZ→RGB 3×3, illuminant XYZ, B/W reference Y | viewing illuminant, CMFs, base tuning |
| Gamut compression | C_max(L or Lr or Jz or J′, h) table per output space and algorithm, knee constants | port of `gamut_compression.py` tables |
| Diffusion / halation / scatter kernels | lists of (σ_px, weight, channel weights) after the exp→Gaussian mixture and clustering | family tables and params, pixel size |
| Display | 33³ LUT, output space → monitor | lcms2 |

A **3D‑LUT mode** samples stages E and G on a 33³ grid **on the GPU** (a few ms) and applies them
tetrahedrally. Use it for the CPU fallback, for battery saver, and to mirror the Python GUI's
`use_enlarger_lut/use_scanner_lut = True` (17³) when GUI‑exact parity is wanted.

### 3.4 Preview vs full render

- **Interactive preview** renders the *viewport*. When zoomed out, it renders a downsampled image
  (long edge = viewport pixels × devicePixelRatio, about 1.5–3 MP) with `pixel_size_um` scaled
  accordingly. That is physically consistent and matches what Python does in `preview_mode`.
  When zoomed to ≥ 1:1, it renders the visible crop at full resolution plus an apron, using
  Python's `io.crop` semantics. Metering always uses the full‑frame proxy.
- Unlike the Python GUI, preview does **not** turn off grain, DIR diffusion or blurs by default.
  The GPU can afford them. A "fast preview" toggle restores the Python `preview_mode` behaviour.
- **Full render** (export) runs as a background job with progress and cancellation, and never
  blocks the UI. Interactive frames take priority. The export job is split into submit chunks of
  ≤ 200 ms of GPU work (TDR safety).
- Targets (to be measured in phase 2+): interactive frame < 33 ms for the default look at about
  2 MP; 24 MP export with all effects in a few seconds.

### 3.5 Spatial filters

| Filter | Method |
|---|---|
| Gaussian σ < ~3 px | separable FIR in shared memory (one fused 2D tile pass where possible) |
| Gaussian σ ≥ ~3 px | **pyramid**: box‑downsample to a level where σ_level ≈ 2–4 px, separable FIR there, bilinear upsample and accumulate. An error budget per level is validated by the parity harness. |
| Exponential (scatter tail, DIR tail) | 3‑Gaussian mixture with Python's exact fit table (`_EXPONENTIAL_GAUSSIAN_FITS[3]`) |
| Halation bounces | N Gaussians (σ√k) accumulated in one pyramid walk |
| **Diffusion filter** (core 2 + halo 3 + bloom 4 exponentials, per‑channel halo) | each exponential → 3 Gaussians, then **cluster components with similar σ** (OFX, ratio 0.10), evaluated on the pyramid. OFX reported about 10× faster (353 → 35 ms at 1080p) with parity PASSED. **VkFFT** (MIT) is a selectable "reference quality" path that matches Python's FFT convolution, which uses a radius of 8λmax capped at half the image. |
| Multiplicative USM | blur + per‑pixel power, then **global per‑channel sums** (reduction pass) for mass renormalisation |

Boundaries: Python uses `reflect` (IIR uses sample replication). Use mirrored clamp addressing on
the GPU and document the edge tolerance.

### 3.6 Grain on the GPU

- **RNG:** counter‑based (Philox4x32‑10, or PCG/xxhash as in OFX), keyed by
  `(image_seed, absolute full‑res x, y, channel, sub‑layer, draw index)`. The result does not
  depend on tiling or scheduling. Bit parity with NumPy's sequential stream is impossible, so
  parity is statistical (§8).
- **Sampler:** port the model exactly, not OFX's "preview" Gaussian shortcut.
  N_s ~ Poisson(N/sat), then k ~ Binomial(N_s, p).
  - Poisson: inversion for λ < 10, PTRS (Hörmann) otherwise.
  - Binomial: inversion for n·min(p, 1−p) < 10, BTRD otherwise.
  - The "fast stats" mode reproduces Python's `fast_binomial` / `fast_poisson` thresholds (direct
    simulation for n < 25, normal approximation for var > 10) so that parity can be checked
    against the GUI configuration.
- **Layout:** one dispatch with z = channel × sub‑layer, writing R16F planes. Per‑layer dye‑cloud
  blur (σ differs per layer), then a resolve kernel that sums layers, applies the log‑normal
  micro‑structure field (generated by the same RNG), blurs, applies the USM and subtracts d_min.
- Grain sampling and its accumulators use fp32 arithmetic. Python itself works in float32 here.

### 3.7 Tiling

- **Default: full‑frame** on the GPU when the working set fits the budget
  (`VK_EXT_memory_budget`, about 60 % of the reported budget). On this machine that covers
  ~60–100 MP in most configurations.
- **Tiled mode** for larger images or low memory:
  - Tile size about 2048² plus an **apron** equal to the sum of the active effects' support radii
    (3σ per Gaussian, the largest mixture σ for exponentials), like OFX's
    `estimateVulkanTileOverlap` but computed per effect.
  - **Global dependencies become pass boundaries**:
    - (1) auto‑exposure (the full‑frame proxy, computed up front);
    - (2) the boost global max (pre‑pass over all tiles);
    - (3) wide‑kernel components (diffusion halo/bloom, DIR tail, large halation σ). These are
      computed once on a **full‑frame low‑resolution pyramid** built during a pre‑pass, then
      sampled by each tile;
    - (4) grain USM mass sums (pre‑pass reduction);
    - (5) B/W correction references (parameter‑only, no image dependency).
  - Intermediate full‑frame planes between passes live in host‑visible memory, which is the same
    physical RAM, so there is no copy penalty.

### 3.8 FP16 policy

Arithmetic is **always fp32**. FP16 is used **only for storage**, and only where the error is
bounded:

| Data | Storage | Reason |
|---|---|---|
| Linear film / print raw | **fp32** | HDR: mid‑grey = 1, boosted highlights reach 2^10–2^20, which exceeds fp16's 65504 |
| log raw, densities | fp32 (final) / fp16 (preview option) | fp16 absolute error up to about 0.002 at density 2 means about 0.5 % transmittance. Fine for preview, not for export. |
| Grain layer planes, micro‑structure field, glare field | fp16 | stochastic texture; summed in fp32 |
| Pyramid levels for wide blurs | fp16 | low‑frequency content; validated by parity |
| `tc_lut`, curves, response tables | fp32 | small; precision‑critical |
| Spectra source LUT | fp16 on disk (as upstream), expanded to fp32 at bake | |
| Display / swapchain | RGBA8 sRGB, or RGBA16F (HDR display later) | |

Each fp16 choice is a switch in the parity harness, so it can be verified per stage (the same
methodology as OFX `PnP/Performance`).

### 3.9 Unified‑memory strategy (Arc 140T)

- **VMA** with `VMA_MEMORY_USAGE_AUTO` plus host‑access flags. On the iGPU this resolves to
  `DEVICE_LOCAL | HOST_VISIBLE` memory, so **no staging buffers**. Keep the
  `ALLOW_TRANSFER_INSTEAD` path so a future discrete GPU still works.
- **Zero‑copy input:** OIIO and LibRaw decode directly into a persistently mapped buffer. Where the
  decoder owns the allocation, use `VK_EXT_external_memory_host` to import it. A single GPU pass
  converts the input (uint8/16/half/float, any channel count) into the working layout, as OFX's
  `SpektraFormatConvert` does.
- **Readback** for export from `HOST_CACHED` memory, with `vkInvalidateMappedMemoryRanges`.
- **Planes as `VkImage` (optimal tiling)** for 2D‑local access (separable blurs, pyramids,
  bilinear sampling via samplers). Small tables go in storage buffers. OFX uses buffers throughout;
  phase 2 includes a microbenchmark of images vs buffers on the 140T before committing.
- **Transient memory:** each render graph is lifetime‑analysed, and its transient planes are
  aliased in a VMA pool. Cache planes are evicted LRU against a budget refreshed from
  `VK_EXT_memory_budget`.
- **One queue** (graphics+compute family) with timeline semaphores. The iGPU gains nothing from
  async compute, and one queue keeps display and compute ordered without cross‑queue ownership
  transfers.
- **Device‑lost handling:** recreate the device, re‑bake from the hashed tables, fall back to CPU
  after two failures, and show a banner.

### 3.10 Qt integration and display

- The viewer is a `QWindow` with `surfaceType = VulkanSurface`, embedded in the widget UI through
  `QWidget::createWindowContainer`. The engine owns the `VkInstance` (wrapped in
  `QVulkanInstance::setVkInstance`), the `VkDevice` and the swapchain. Compute output images are
  sampled directly by the present pass: **zero readback**. This avoids Qt RHI choosing D3D11/12 on
  Windows and the cross‑API copies that would follow.
- The UI thread never waits on the GPU. A render thread owns the command submission, and the UI
  posts immutable `Params` snapshots (latest wins, coalesced).

---

## 4. CPU fallback

The `backend_cpu` executes the **same stage graph** with the `libs/model` kernels.
- Templated `Real` type: **float** for the fallback; **double** in "reference mode" for parity
  against Python's float64.
- Parallelism: oneTBB over row strips. SIMD: **Google Highway**, targeting AVX2 / FMA (no AVX‑512
  on the 285H; Highway dispatches at runtime).
- Spatial filters reproduce Python exactly: **Young–van Vliet IIR** for σ ≥ 3 and FIR (truncate 3)
  for σ < 3, the same split as `fast_gaussian_filter`, plus FFT (pocketfft, BSD) for the diffusion
  filter. That makes the CPU path the tightest oracle for Python.
- Spectral stages use the 3D‑LUT mode by default (Python GUI behaviour) and direct spectral in
  reference mode.
- **Used when:** no Vulkan 1.3 device, a device‑lost loop, an out‑of‑memory failure, the user
  setting, headless CI without a GPU, and parity reference runs.

---

## 5. UI layout (Qt 6 Widgets)

```
┌─────────────────────────────────────────────────────────────────────────────────────┐
│ Toolbar: Open · Open RAW · Rotate ⟲⟳ · Preview/Full · Before|After|Split · Tap view │
│          (Input · Negative · Film density · Print · Final) · Zoom · Export · Presets│
├───────────────┬───────────────────────────────────────────────┬─────────────────────┤
│ LEFT DOCK     │                VIEWER (Vulkan)                │ RIGHT DOCK          │
│ ▸ Input       │                                               │ [Basic | Advanced]  │
│   file, RAW   │   image on 18 % grey canvas + white border    │ ▸ Camera            │
│   WB/tint,    │   (paper-back watermark optional)             │ ▸ Diffusion (camera)│
│   lens corr., │                                               │ ▸ Halation/scatter  │
│   colour space│   loupe 1:1 on hover; split before/after      │ ▸ DIR couplers      │
│ ▸ Film stock  │                                               │ ▸ Grain             │
│   searchable  │                                               │ ▸ Film chemistry    │
│   list, info  │                                               │ ▸ Enlarger/pre-flash│
│   card (type, │                                               │ ▸ Diffusion (enl.)  │
│   use, ISO,   │                                               │ ▸ Print chemistry   │
│   dev-time)   │                                               │ ▸ Base tuning       │
│ ▸ Print stock │                                               │ ▸ Scanner & glare   │
│ ▸ Crop/upscale│                                               │ ▸ Gamut compression │
│ ▸ Presets     │                                               │ ▸ Spectral settings │
│               │                                               │ ▸ Display / Special │
├───────────────┴───────────────────────────────────────────────┴─────────────────────┤
│ Status: GPU name · stage timings bar · memory budget · render progress / cancel     │
│ Optional bottom dock: histogram (RGB / log-exposure), density-curve plot with       │
│ mid-grey marker, filmstrip of the current folder                                    │
└─────────────────────────────────────────────────────────────────────────────────────┘
```

- **Panels are generated** from a C++ port of `params_manifest.py`: label, tooltip, min/max/step,
  and `tier` (basic / advanced). Each widget binds to a dotted path on `Params`. That is the
  upstream design, and it keeps the UI in sync with the schema.
- Choosing a film or print stock applies the upstream defaults: neutral filters, grain and coupler
  presets, halation preset, and `scan_film` for positive film. The B&W dev‑time selector appears
  only when the profile has a family.
- Undo/redo is a `Params` snapshot stack (cheap: profiles are shared, as in
  `clone_runtime_params`). There is auto‑preview with debounce, "save as default", "restore
  factory", and state import/export.
- Save dialog: format, bit depth, saving colour space and CCTF, ICC embed, metadata copy.
- Cosmetic Python extras (polaroid developing animation, virtual paper back) are deferred.

---

## 6. Profile data export (Python → C++)

**Principle:** the C++ app never reimplements colour‑science's data tables or reads Python
pickles. A pinned exporter produces one versioned binary pack. Unlike OFX, which compiles tables
into generated `.cpp` files, the pack is loaded at runtime, so data updates don't need a rebuild.

### 6.1 Exporter (`tools/exporter/export_pack.py`)

- It runs in a venv with `spektrafilm` installed **at a pinned commit SHA** plus a pinned
  `colour-science` version. It imports spektrafilm's own loader and helper functions, so the
  values are exactly what the reference uses.
- It writes `data/spektrafilm.sfpk` and a `pack_manifest.json` (human‑readable summary).

### 6.2 Pack contents

| Group | Content |
|---|---|
| Provenance | upstream repo URL, commit SHA, branch, colour‑science version, exporter version, date, licence block (CC BY‑SA 4.0 text reference + attribution string) |
| Profiles (all 31) | `info` (stock, name, type, support, stage, use, antihalation, target_print, channel_model, densitometer, reference / viewing illuminant), `metadata`, every `data` array **as float64** (wavelengths, log_sensitivity, channel_density, base_density [81] or [81×N dev‑times], midscale_neutral_density, log_exposure, density_curves, density_curves_layers, development_time, hanatos window / surface params), and `density_curves_model` (type, centers / amplitudes / sigmas / alphas) |
| Presets | `grain.toml`, `couplers.toml` → normalised JSON; halation preset table |
| Enlarger | `neutral_print_filters.json`; dichroic sets (custom erf params plus resampled Thorlabs / Edmund / Durst); KG1/3/5; lens transmission; Hoya colour filters. **Every curve resampled to the 81‑λ grid with upstream's own Akima interpolation.** |
| Illuminants | every name the pipeline can request (D50, D55, D65, A, T = "Incandescent", K75P = "Kinoton 75P", TH‑KG3, TH‑KG3‑L, BB<T> sampled on a grid, plus experimental scan illuminants), on 81 λ, normalised as `standard_illuminant()` does |
| Colorimetry | CIE 1931 2° CMFs and Stockman & Sharpe LMS on 81 λ; Mallett 2019 basis |
| Upsampling | `irradiance_xy_tc` spectra LUT (192×192×81, float16, as upstream); spectral locus xy |
| Colour spaces | per space: RGB→XYZ matrix, whitepoint, CCTF descriptor (analytic type + params, or 4096‑entry LUT for log curves); CAT16 / CAT02 matrices |
| Gamut compression | precomputed C_max tables per (output space × algorithm), or the inputs needed to build them |
| Diffusion | family table (λ, spread, n, α, weights, warmth), strength breakpoints, family gains |
| ICC | the bundled `.icc` files with their licences |

**Format (`.sfpk`):** a GLB‑style container.
- Header: magic `SFPK`, `u32 version`, then a JSON chunk (the manifest: per‑array
  name / dtype / shape / offset / xxh3 hash) and a BIN chunk (little‑endian, 64‑byte aligned).
- The loader `mmap`s the file and exposes `std::span` views. The pack hash is part of every cache
  key.
- A secondary **direct‑JSON loader** reads upstream profile `.json` files with the same schema
  (simdjson), so users can drop in new upstream stocks without re‑exporting. The shared numeric
  tables still come from the pack.

**Updating upstream:** bump the pinned SHA, re‑export, run the parity suite, and commit
`pack_manifest.json` so the diff shows exactly what changed. Add a `CHANGELOG.txt` entry, which
the CC BY‑SA licence also requires.

---

## 7. Determinism, threading, errors

- Bake runs on the CPU (TBB task), rendering on the GPU render thread, I/O on a worker pool. The
  UI thread only posts `Params`.
- All stochastic effects are seeded per image, plus an optional "animate grain" seed per frame.
  CPU and GPU produce statistically identical output; the GPU is bit‑stable across runs.
- Errors (missing profile, device lost, OOM) are surfaced in the status bar. A render never
  crashes the app.

---

## 8. Parity‑testing plan (vs the Python reference)

**Reference generator** (`tools/parity/make_golden.py`, pinned SHA): runs Python with explicit
settings and writes `.npz` / `.exr` plus a `cases.json` manifest. **Runner:** `spectra-cli parity
cases.json --backend {cpu-ref,cpu,gpu}` renders the same cases. **Comparator** (Python) writes an
HTML report with metrics, heatmaps and PASS/FAIL. The thresholds start from OFX's proven ones.

| Tier | What | How | Pass criteria |
|---|---|---|---|
| T0 Data | the pack round‑trips | C++ re‑emits every array; compared to Python | bit‑exact (float64) |
| T1 Units | pure functions: illuminants, filters, `color_enlarger`, model sampling, s023 morph incl. Brent offset, dev‑time select, DIR matrix / Langmuir / pre‑correction, grain params (particle counts), `tc_lut` incl. window, surface and gamut remap, Mitchell 2D sample, boost curve, halation weights, diffusion PSF radial profile, exp‑Gaussian fit, auto‑exposure EV (all 7 methods), B/W correction factors, gamut compression (all 6 algorithms), CCTFs, XYZ→RGB | golden vectors (random and edge inputs) | cpu‑ref: rel ≤ 1e‑9; cpu (fp32): rel ≤ 1e‑5; gpu: ≤ 1e‑4 relative or 1e‑5 absolute |
| T2 Non‑spatial taps | `lut_mode` / spatial and stochastic off; collect each tap (`log_e_film`, `cmy_film`, `log_e_print`, `cmy_print`, `rgb_out`) | images: 16‑step grey ramp, saturated patches, upstream `img/targets` (cc07, cc11, IT8), `portrait_leaves_32bit_linear_prophoto_rgb.tif`, a synthetic HDR highlight ramp; **every film × every compatible print** (incl. B&W) plus scan‑film | OFX "non_spatial": max_abs ≤ 0.015, p99 ≤ 0.006, RMSE ≤ 0.003 (output‑encoded RGB) **and** CIEDE2000 mean ≤ 0.5, p99 ≤ 1.5. cpu‑ref is held 10× tighter. |
| T3 Spatial deterministic | halation, scatter, diffusion (4 families × strengths), DIR diffusion, lens / scanner blur, USM | the same images, grain and glare off | SSIM ≥ 0.98, NRMSE ≤ 0.02, p95 ≤ 0.02. VkFFT diffusion vs Python FFT ≤ 1e‑3. |
| T4 Stochastic | grain, glare | 512² flat density patches at 10 levels per stock, plus the portrait | per density bin: mean bias ≤ 0.005 D, std within 5 %, **RMS granularity at a 48 µm aperture within 5 % of Python**, radial power spectrum similarity ≥ 0.9 (OFX metric, stricter) |
| T5 Param sweeps | each manifest parameter at min / default / max (one at a time) on 2 representative stock pairs | T2/T3 metrics | as the tier |
| T6 Cross‑backend | gpu vs cpu (both ours) on everything above | | tighter than vs Python |
| T7 Perf | fixed cases (default, grain, all effects) at 2 MP / 24 MP / 61 MP | GPU timestamps per stage | tracked over time; regression > 10 % fails locally |

**Mapping our params to the Python reference:**
- *Reference parity* uses `simulate()` defaults: direct spectral (`use_enlarger_lut = False`),
  exact stats (`use_fast_stats = False`).
- *GUI parity* uses the Python GUI's overrides: LUT 17³ and fast stats. Both are generated.

**CI:**
- GitHub Actions on Windows and Linux runs T0–T2 and T6 on the CPU backends.
- GPU tiers run on lavapipe (Mesa software Vulkan) in CI for correctness.
- On the 285H / 140T, run locally (`ctest -L gpu`) for real numbers and driver coverage.

**Python‑side notes for golden generation:**
- Set `np.random.seed`.
- Numba parallel RNG streams are per‑thread, so stochastic goldens are only statistically
  reproducible.
- Run Python with `NUMBA_NUM_THREADS = 1` for repeatable fast‑stats goldens.

---

## 9. Proposed roadmap after approval (not started)

| Phase | Deliverable |
|---|---|
| 2 | Build skeleton (CMake / vcpkg / CI), exporter + `.sfpk`, `libs/core` + `libs/data`, golden generator, T0 / T1 for data and curves |
| 3 | `libs/model` + `bake` CPU reference for all non‑spatial stages; T2 green (cpu‑ref) |
| 4 | Vulkan backend: device / memory, expose / develop / print / scan kernels; T2 green (gpu); headless CLI renders |
| 5 | Spatial effects + pyramid + grain on CPU and GPU; T3 / T4 green |
| 6 | Qt GUI: viewer, panels from the manifest, caching / invalidation, presets, state |
| 7 | I/O: LibRaw, lensfun, exiv2, ICC, save; tiling + budgets; Arc tuning (FP16 switches, images vs buffers, subgroup sizes) |
| 8 | Extras by decision: LUT export, experimental routes (convert‑film), OFX‑only features (push/pull, bleach bypass, HDR output) |
