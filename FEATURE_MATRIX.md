# Feature Matrix — spektrafilm → native Windows app (C++20 / Vulkan / Qt 6)

Phase 1 research output. **No app code exists yet.**

## Sources studied

| Repo | Ref | Commit / date | Role |
|---|---|---|---|
| `andreavolpato/spektrafilm` | `main` | `3bb2c2d` (2026‑06‑07) | Reference, stable |
| `andreavolpato/spektrafilm` | `dev` | `6cd00c8` (2026‑06‑10) | **Reference baseline for this plan.** It is a superset of `main` except for one LUT‑creator fix (`3bb2c2d` "vlog midgray exposure"). |
| `andreavolpato/spektrafilm` | `experimental` | `28bf883` (2026‑07‑08) | 25 commits ahead of `dev`. Forward‑looking only (see the last section). |
| `chaert-s/spektrafilm-ofx` | `main` | `86476af` | Performance reference: Vulkan/Metal kernels, data export, perf/parity harness. |

> **`src/spektrafilm_profile_creator` does not exist on any current branch.** It was removed from
> the packaging in commit `dfb0219` (2026‑05‑11, *"extract profile creator into separate package …
> creating a separate repo"*). The 2026‑05‑28 history rewrite dropped its sources, and there is no
> public replacement repo. The C++ app therefore **consumes finished profiles**. It cannot fit new
> stocks from datasheets. See RISKS.md R‑02.

Legend: ✅ present · ◐ present but different, older, or partial · ❌ absent · **dev** = only on the
`dev` branch, not `main` · **exp** = only on `experimental`.

---

## 1. Film stocks (filming profiles)

All profiles are JSON, `metadata/info/data`, CC BY‑SA 4.0. Spectral grid: 81 samples, 380–780 nm
in 5 nm steps. Log‑exposure grid: 256 samples from −3 to 4. The density‑curve model is a sum of 3
Gaussian‑CDF sub‑layers per channel (`norm_cdfs`).

| Stock | Type / use / AH | Python | OFX | Notes |
|---|---|---|---|---|
| Kodak Portra 160 / 400 / 800 | neg, still, strong | ✅ | ✅ | |
| Kodak Portra 800 Push1 / Push2 | neg, still, strong | ✅ | ✅ | Separate profiles. OFX also has a generic push/pull model. |
| Kodak Ektar 100 | neg, still, strong | ✅ | ✅ | |
| Kodak Gold 200, Ultramax 400 | neg, still, weak | ✅ | ✅ | |
| Fujifilm Pro 400H | neg, still, strong | ✅ | ✅ | |
| Fujifilm C200, X‑Tra 400 | neg, still, weak | ✅ | ✅ | |
| Kodak Vision3 50D / 250D / 200T / 500T | neg, cine, strong | ✅ | ✅ | 200T and 500T use reference illuminant `T`. |
| Kodak Verita 200D | neg, cine, strong | ✅ | ✅ | |
| Kodak Ektachrome 100 | pos, still, strong | ✅ | ✅ | |
| Kodak Kodachrome 64 | pos, still, weak | ✅ | ✅ | |
| Fujifilm Velvia 100, Provia 100F | pos, still, strong | ✅ | ✅ | |
| **Kodak Double‑X 5222** (B&W) | neg, cine, 1‑ch | ✅ dev | ❌ | Development‑time family: 4 / 5 / 6.5 / 9 / 12 min (5 curves, 5 base+fog). |
| **Kodak Tri‑X 7266** (B&W reversal) | pos, cine, 1‑ch | ✅ dev | ❌ | Single development time. |

**Best implementation:** load the JSON data through an exported pack (ARCHITECTURE §6). Port the
Python model evaluation (`refresh_density_curves_from_model`, `select_development_time`) to C++ so
the chemistry morph can stay interactive.

## 2. Print media (printing profiles)

| Stock | Python | OFX | Notes |
|---|---|---|---|
| Kodak Portra Endura, Supra Endura, Ultra Endura, Endura Premier, Ektacolor Edge | ✅ | ✅ | RA‑4 papers, viewed under D50. Upstream comments mark Ultra Endura as "problematic". |
| Fujifilm Crystal Archive Type II | ✅ | ✅ | |
| Kodak Vision 2383, Vision Premier 2393 | ✅ | ✅ | Cine print films, viewed under `K75P` (Kinoton 75P projector). |
| **Kodak 2302** (B&W print film) | ✅ dev | ❌ | 1‑channel, with a 5‑entry development‑time family. |

## 3. Parameters and features

### 3.1 Input, I/O and colour management

| Feature / parameter | Python | OFX | Best implementation | Notes / gaps |
|---|---|---|---|---|
| Image load: TIFF / EXR / PNG / JPEG | ✅ OIIO | host‑supplied | OpenImageIO (or libtiff + OpenEXR + libpng + libjpeg‑turbo) | Python reads uint8/16/half/float and keeps 3 channels. |
| RAW load (LibRaw → linear ACES2065‑1) | ✅ rawpy | ❌ | LibRaw (same settings as `_postprocess_params`) | Separate worker thread in dev. |
| RAW white balance: as‑shot / daylight / tungsten / custom (temp + tint) | ✅ | ❌ | Port `_apply_white_balance_adaptation` and `_apply_tint_adjustment` | Uses colour‑science CCT→xy. Export the numbers rather than reimplementing. |
| Lens correction (lensfun: vignetting, TCA, distortion) | ✅ | ❌ | lensfun C library | Database is CC BY‑SA 3.0. |
| EXIF read / copy on save | ✅ exiv2 | ❌ | exiv2 | |
| `io.input_color_space` | ✅ any colour‑science name. GUI list: sRGB, DCI‑P3, Display P3, Adobe RGB, BT.2020, ProPhoto (default), ACES2065‑1 | ✅ 26 spaces incl. camera logs | Matrix + transfer function per space. Use the OFX list (superset). | Experimental adds DaVinci Wide Gamut and V‑Gamut. |
| `io.input_cctf_decoding` | ✅ | ✅ (per‑space decode LUT, 4096 entries) | Analytic CCTFs, with a 1D‑LUT fallback for log curves | |
| `io.crop`, `crop_center`, `crop_size` | ✅ | ◐ host ROI | Python semantics: size is a fraction of the long side | Metering uses the full frame before the crop. |
| `io.upscale_factor` | ✅ (skimage, bicubic, order 3) | ❌ | GPU bicubic or Lanczos | Divides pixel size; changes the physical scale of every spatial effect. |
| `camera.film_format_mm` (long edge) | ✅ float, default 35 | ✅ enum: 8 mm … IMAX 70 | Float plus presets | Sets `pixel_size_um`, which every spatial effect depends on. |
| Output colour space | ✅ GUI list above | ✅ 26 spaces + HDR PQ/HLG + Resolve RCM | Python list + OFX display spaces | |
| `io.output_cctf_encoding` | ✅ | ✅ | | |
| Saving colour space + CCTF (separate from simulation output) | ✅ GUI | ❌ | | |
| ICC embedding on save | ✅ Elle Stone (CC BY‑SA 3.0) + saucecontrol (MIT) | ❌ | lcms2 or embed the bundled profiles | |
| Save formats JPEG / PNG 8‑bit, TIFF 8/16/32, EXR half/float | ✅ | host | OIIO | |
| Display transform via the monitor ICC profile | ✅ Pillow ImageCms (Windows only) | host | Windows WCS / `GetICMProfile` + lcms2, or a Vulkan HDR swapchain | |
| HDR output (PQ / HLG, peak / reference white, tone map) | ❌ | ✅ | Optional extra | Not in the reference. Mark as an OFX‑only extension. |

### 3.2 Spectral upsampling (RGB → film raw exposure)

| Feature / parameter | Python | OFX | Best implementation | Notes / gaps |
|---|---|---|---|---|
| `settings.rgb_to_raw_method = hanatos2025` (default) | ✅ | ✅ | Precompute `tc_lut[192×192×3]` on the CPU at parameter change. Per pixel on the GPU: RGB → XYZ (CAT16 to the film reference illuminant) → xy → tri→quad → Mitchell‑Netravali bicubic 2D lookup × (X+Y+Z). | Spectra LUT `irradiance_xy_tc.npy` is float16, 192×192×81 (from hanatos/vkdt). OFX does the same thing (`HanatosRawResponse`). |
| `mallett2019` | ✅ | ✅ | Per‑pixel 3×3 matrix (basis·illuminant·sensitivity) | Normalised to the green channel at mid‑grey. |
| Hanatos2025 adaptation **window** (erf4 / logiflex8 band‑pass, WB‑preserving) | ✅ default on | ◐ (`bandpass_hanatos2025` from archived profiles, plus its own "Hanatos2026" window) | Python | Profiles carry 4 window params (erf4). |
| Hanatos2025 adaptation **surface** (poly4 log‑exposure correction, ±2 stops, Hanika sigmoid) | ✅ default off | ❌ | Python | Profiles carry 3×15 params. |
| `settings.spectral_gaussian_blur` (nm) | ✅ | ❌ | Blur the spectra LUT along λ before the bake | |
| Input gamut compression (`io.input_gamut_compress`: `xy` radial to the spectral locus, or `oklch`; Reinhard knee (0, 1, 6)) | ✅ baked into `tc_lut` by remap‑resample | ◐ (`colorAdaptationInputCompression` flag, xy radial) | Bake on the CPU, as Python does. The hot path stays unchanged. | Experimental adds `boundary = inscribed_hull`. |
| arctic2026beta04, gauss‑lasers, otsu2018, jakob2019 upsampling | ❌ | ❌ | — | **exp only.** The data‑driven `*_reflectance_xy_tc.npy` LUTs share the same 2D‑LUT plumbing. |
| Profile creator (fit sensitivities and curves from datasheets) | ❌ (removed) | ❌ | Out of scope | RISKS R‑02 |

### 3.3 Camera / exposure

| Feature / parameter | Python | OFX | Best implementation | Notes / gaps |
|---|---|---|---|---|
| `camera.exposure_compensation_ev` | ✅ | ✅ `filmExposureEv` | | |
| `camera.auto_exposure` + method: center_weighted (σ=0.2), matrix 5×5 raised‑cosine, multi_zone (3 rings), partial (15 %), highlight_weighted (w=x²), median, average | ✅ (meters the 256 px nearest‑neighbour full‑frame preview of film raw, target raw = 1) | ◐ only center_weighted and median, metering Y of the input | Python (all 7). Small CPU or GPU reduction on a downsampled preview. | Target is raw = 1 (normalised mid‑grey), on the mean of the 3 raw channels. |
| Camera taking filter (`camera.color_filter`): Hoya X0, X1, Y2, YA3, R1 | ✅ dev | ❌ | Multiply into the sensitivity before the `tc_lut` bake | Not renormalised (the cast and attenuation are intended). The mid‑grey print reference accounts for it. |
| Camera UV / IR cut (band‑pass erf) | ◐ function exists (`compute_band_pass_filter`) but not wired in | ✅ | Optional, same plumbing as the colour filter | |
| `camera.lens_blur_um` | ✅ | ❌ | Gaussian in µm / pixel_size | |
| Camera diffusion filter (`camera.diffusion_filter`, see 3.6) | ✅ | ✅ | | |

### 3.4 Film rendering

| Feature / parameter | Python | OFX | Best implementation | Notes / gaps |
|---|---|---|---|---|
| **Halation highlight boost** `boost_ev`, `boost_range`, `protect_ev` | ✅ | ✅ | Two passes: GPU global‑max reduction, then a per‑pixel curve | Normalised by the **global max** of the raw image (RISKS R‑05). |
| **In‑emulsion scatter** `scatter_amount`, `scatter_spatial_scale`, `scatter_core_um` (2.2, 2.0, 1.6), `scatter_tail_um` (9.3, 9.7, 9.1), `scatter_tail_weight` (0.78, 0.65, 0.67) | ✅ | ✅ | Gaussian core + exponential tail. The tail is a mixture of 3 Gaussians (fixed fit). Energy‑preserving. | |
| **Halation** `halation_amount`, `halation_spatial_scale`, `halation_strength` (RGB), `halation_first_sigma_um`, `halation_n_bounces` (3), `halation_bounce_decay` (0.5), `halation_renormalize` | ✅ | ✅ (no bounce/renormalise controls exposed) | N Gaussians with σ·√k widths, weights ρ^(k−1) | Presets come from `(use, antihalation)`: still σ = 65 µm, cine σ = 50 µm; strong / weak / no. |
| **DIR couplers** `active`, `amount`, `inhibition_samelayer`, `inhibition_interlayer`, `gamma_samelayer_rgb`, `gamma_interlayer_{r→gb, g→rb, b→rg}` | ✅ | ✅ | 3×3 donor→receiver matrix | Presets: `couplers.toml` (defaults per colour/BW × neg/pos, a cine daylight/tungsten split, per‑stock overrides; optimised 2026‑06‑07). OFX uses its older, single set of default gammas. |
| DIR **Langmuir saturation** `langmuir_donor_k_rgb` (negative), `langmuir_receiver_k_rgb` (reversal, Shuto 1997) | ✅ dev | ❌ | Python | Replaces the old `high_exposure_couplers_shift`. |
| DIR pre‑correction (inverting the curves "before couplers") | ✅ | ✅ | CPU precompute per parameter change (np.interp inversion) | RISKS R‑07 (non‑monotonic inversion). |
| DIR spatial diffusion `diffusion_size_um` (20), `diffusion_tail_um` (200), `diffusion_tail_weight` (0.03) | ✅ | ✅ (tail weight default 0.06) | Gaussian + exp tail. The tail runs on a downsampled pyramid. | |
| **Film chemistry morph** (s023): `development_time`, `active`, `gamma_factor`, `gamma_factor_fast`, `gamma_factor_slow`, `gamma_factor_{red,green,blue}`, `developer_exhaustion` (Gumbel blend with a Brent‑solved offset that keeps D(0)) | ✅ dev (the film side is new) | ◐ `filmGamma`, `printShadowShape/HighlightShape`, push/pull | Python (CPU precompute of 256×3 curves + 256×3×3 layers) | Replaced the old `density_curve_gamma`. Identity is bit‑exact. |
| **B&W development‑time selection** | ✅ dev | ❌ | Python | |
| Film push/pull (standard / experimental), bleach bypass, leuco‑cyan coupling | ❌ | ✅ | Optional OFX‑only extension | Not in the reference. |
| **Grain** `active`, `rms_granularity` (per channel, ×1000 σ_D at a 48 µm aperture, D = 1 net), `density_min`, `uniformity`, `particle_scale_sublayers` (1, 0.5, 0.25), `blur` (0.8 px), `mult_usm_sigma` (0.7), `mult_usm_amount` (1.5), `blur_dye_clouds_um` (2), `micro_structure` (0.2 µm, 30 nm), `micro_sublayers` | ✅ dev (RMS parametrisation, mass‑conserving multiplicative USM, presets) | ◐ older model: `particleAreaUm2`, per‑channel scale, preview / production / "grain synthesis" models, amount, saturation, animate, seed | Python model; GPU sampling uses a counter‑based RNG | Compound Poisson‑Binomial per sub‑layer. Particle area is solved so the realised multilayer peak equals the datasheet RMS. Presets in `grain.toml`. RISKS R‑06. |
| Grain presets (`grain.toml`) | ✅ dev | ❌ | Exported pack | |
| Film **base tuning** (`film_render.base`: active, scale, cyan, magenta, yellow) | ✅ dev | ❌ | Python | Applied **at scan time** on the scanned medium's base (status‑A peaks 445/530/610 nm, 20 nm Gaussian smoothing). Experimental splits it into Film and Print base with a tilt. |
| Film glare (`film_render.glare`) | ◐ in the schema, but unused by the stages | ❌ | Skip | Only `print_render.glare` is used (scan stage). |

### 3.5 Enlarger / printing

| Feature / parameter | Python | OFX | Best implementation | Notes / gaps |
|---|---|---|---|---|
| `enlarger.illuminant` | ✅ GUI offers `TH-KG3` only (BB 3400 K × Schott KG3). Also supported: `T`, `K75P`, `BB<T>`, `TH-KG3-L` (+ lens transmission), any CIE name | ✅ TH‑KG3 | Exported spectra | |
| Dichroic CMY filters (Kodak CC units: 100 = 1.0 density) | ✅ "custom" erf dichroics (edges 516/500/610/607). Thorlabs, Edmund and Durst Digital Light datasets are bundled. | ✅ custom | Python | `color_enlarger` defaults to the custom set. |
| `c/m/y_filter_neutral` from the database | ✅ `neutral_print_filters.json`: 9 papers × TH‑KG3 × 22 films; fallback C0 M50 Y50 | ✅ | Exported pack | Only TH‑KG3 is present. RISKS R‑12. |
| `y_filter_shift`, `m_filter_shift` | ✅ | ✅ (+ `filterC`) | | |
| `print_exposure`, `print_exposure_compensation`, `normalize_print_exposure` | ✅ | ✅ (EV) | Mid‑grey geometric‑mean normalisation, as in Python | Four‑way truth table in `_compute_exposure_factor_midgray`. |
| **Pre‑flash** `preflash_exposure`, `preflash_y/m_filter_shift` | ✅ | ✅ | Base‑density light through the pre‑flash‑filtered illuminant | |
| Enlarger lens blur | ✅ | ❌ | Gaussian | |
| Enlarger diffusion filter | ✅ | ✅ | §3.6 | |
| Enlarger scale / offset (crop‑in) | ❌ | ✅ | Optional | |
| Printer lights / APD (SMPTE ST 2065‑2) print timing | ❌ | ✅ (needs licensed SMPTE CSVs, not redistributable) | **Exclude** | LICENSES.md |
| Print 3D LUT acceleration (`use_enlarger_lut`, 17³; 1D LUT with 1024 steps for B&W) | ✅ (the GUI forces it on) | ❌ (direct per‑pixel 81‑λ loop) | GPU: direct spectral per pixel (exact). LUT path as a CPU‑fallback accelerator. | 3→1 LUT for a colour film onto a B&W print. |
| **Print chemistry morph** (`print_render.chemistry`, same fields as the film morph) | ✅ | ◐ `printGamma`, shadow/highlight shape, print push/pull | Python | |
| B&W print development time | ✅ dev | ❌ | Python | |

### 3.6 Diffusion filters (camera and enlarger, shared model)

| Feature / parameter | Python | OFX | Best implementation | Notes / gaps |
|---|---|---|---|---|
| Families: glimmerglass, black_pro_mist (default), pro_mist, cinebloom | ✅ | ✅ | | Per‑family core / halo / bloom groups of 2D exponentials (2 / 3 / 4 components). Bloom weights follow a power law, w ∝ λ^(2−α). |
| `strength` (stops 1/8 … 2, log2 interpolation → p_s × family gain) | ✅ | ✅ | | Energy‑conserving: (1−p_s)·E + p_s·(K⊗E). |
| `spatial_scale`, `halo_warmth` (per‑channel halo redistribution) | ✅ | ✅ | | |
| `core/halo/bloom _intensity`, `_size` | ✅ | ✅ | | |
| Convolution method | FFT `fftconvolve` (float32), radius 8·λmax, **capped at half the image** | Separable Gaussian mixtures on a downsampled pyramid with component clustering (~10× faster, parity PASSED) | **OFX approach** (exponential → Gaussian mixture → pyramid). VkFFT as a high‑quality option. | RISKS R‑04 |

### 3.7 Scanner / output

| Feature / parameter | Python | OFX | Best implementation | Notes / gaps |
|---|---|---|---|---|
| Scan print (default) / **scan film** (`io.scan_film`) | ✅ (GUI forces `scan_film = True` for positive film) | ✅ ScanNegative mode (+ "invert") | | |
| Spectral scan: density → light under the viewing illuminant (D50 / K75P) → CIE 1931 2° XYZ | ✅ (scanner 3D LUT 17³ in the GUI) | ✅ direct 81‑λ | GPU direct | |
| Black / white correction (`white_correction`, `black_correction`, `white_level` 0.98, `black_level` 0.01) | ✅ | ✅ | Linear map on Y + exposure re‑centring | Inactive for scans of negative film. |
| Print **glare** (`percent` 0.03, `roughness` 0.7, `blur` 0.5 px) | ✅ (log‑normal random field) | ✅ | Counter‑hash RNG + small blur | Stochastic. |
| XYZ → output RGB (chromatic adaptation from the viewing illuminant) | ✅ colour `XYZ_to_RGB` (CAT02 default) | ✅ precomputed matrices | Export the matrices | RISKS R‑11 |
| **Output gamut compression** `algorithm`: off, aces_rgc, oklch (**code default**), oklrab, jzazbz, cam16ucs; `knee` (0, 1, 6); `lightness_compression` (0.7, 1.0, 2.2) | ✅ | ◐ oklch and lightness only | All 6. C_max(L, h) tables precomputed per output space. | Docstring says the default is aces_rgc; code says oklch (RISKS R‑10). |
| Scanner `lens_blur`, `unsharp_mask` (σ, amount) = (0.7, 0.7) | ✅ | ✅ (+ MTF50 → σ) | | |

### 3.8 Settings / debug / pipeline control

| Feature | Python | OFX | Best implementation | Notes |
|---|---|---|---|---|
| Taps (`taps.inject/collect`: rgb_in, rgb_pre, log_e_film, cmy_film, log_e_print, cmy_print, rgb_out) | ✅ | ◐ debug `RenderOutputMode` | Native: each tap is a cached GPU image | Basis of the parity harness and stage caching. |
| `debug.lut_mode`, `deactivate_spatial_effects`, `deactivate_stochastic_effects`, `print_timings` | ✅ | ◐ | | |
| `settings.preview_mode` (turns off grain, DIR diffusion, lens blurs, glare blur and USM) | ✅ | ❌ | **Do not copy blindly.** The GPU can afford most of these. Keep a "fast preview" toggle. | |
| `settings.use_fast_stats` (numba approximations for binomial/Poisson) | ✅ (GUI on, reference off) | n/a | GPU sampler | RISKS R‑06 |
| `settings.neutral_print_filters_from_database` | ✅ | ✅ | | |
| Soft update without rebuild (exposure, print exposure, filters, curves) | ✅ `soft_update` | n/a | Replaced by stage‑hash invalidation | |
| Per‑stage timings | ✅ | ✅ (GPU timestamps) | Vulkan timestamp queries | |

### 3.9 GUI features (`spektrafilm_gui`, napari + Qt)

| Feature | Python | OFX | Plan |
|---|---|---|---|
| Parameter panels generated from a manifest (label, tooltip, range, tier basic/dev) | ✅ | host UI | Port `params_manifest.py` to a C++ table (or generate it). |
| Film / print profile pickers; defaults synced on change (neutral filters, presets, `scan_film` for positive film) | ✅ | ✅ | ✅ |
| Preview (downsampled to `preview_max_size`, 640) vs Scan (full resolution) on separate worker channels | ✅ | host | Interactive GPU preview + background full render |
| Auto‑preview on parameter change | ✅ | host | ✅ (debounced) |
| Rotate input ±90° | ✅ | host | ✅ |
| Gray 18 % canvas, white border padding, output interpolation (nearest … lanczos) | ✅ | ❌ | ✅ |
| Display transform (monitor ICC) | ✅ (Windows) | host | ✅ |
| Save output (with saving colour space, CCTF, ICC, metadata) | ✅ | host | ✅ |
| Save / load state file; save as default; restore factory defaults | ✅ | ◐ `.spkpreset` JSON presets | ✅ |
| "Special": film / print channel swap (permute `channel_density` columns) | ✅ | ❌ | ✅ (creative / debug) |
| Polaroid‑style developing animation; virtual photo‑paper back with watermark | ✅ | ❌ | Low priority, cosmetic |
| Startup warm‑up (numba JIT) | ✅ | n/a | Replaced by a pipeline cache and SPIR‑V precompile |
| LUT export (`spektrafilm_lut_creator`: .cube, Lumix .cube, .3dl, Hald PNG; 1–4‑LUT topologies; OCIO 2 config; QA suite; 27 colour spaces including camera logs) | ✅ (separate package) | ◐ single `.cube` export | Later phase. Bake with `lut_mode` semantics. LUTs are CC BY‑SA derivatives (LICENSES.md). |

### 3.10 Experimental‑branch features (not in main/dev — scope decision needed)

| Feature | Notes |
|---|---|
| `workflow.route`: input passthrough · film→scan · film→print→scan · **convert‑film** (scanned negative → film density by inverting the scan model) → print → scan · convert‑film → scan(‑minus‑base) | A real negative‑inversion workflow. Adds `ConvertFilmParams` (scan illuminant, EV, base percentile, 3×3 calibration). The OFX has a different "Process Negative" mode. |
| New spectral upsampling methods (arctic2026beta04 default, gauss‑lasers, otsu2018, jakob2019, mallett2019 LUT) | Same 2D‑LUT runtime; only the data changes. |
| Film base with a spectral tilt; separate print base | |
| Profile enums auto‑generated from the profile library | Already planned (data‑driven pickers). |

---

## 4. Pipeline stages, in order (dev, default route "film → print → scan")

Everything is float64 in Python unless noted. Shapes are H×W×C (C = 3 colour, C = 1 B&W).

| # | Stage (Python location) | Input | Output | Operations, in order |
|---|---|---|---|---|
| 0 | **Load** (`utils/io.py`, `raw_file_processor.py`, GUI) | file | linear RGB float in `io.input_color_space` (RAW → ACES2065‑1) | decode · RAW WB / tint · lensfun · rotate |
| 1 | **Parameter digest** (`params_builder.digest_params`) | `RuntimePhotoParams` | digested params | neutral print filters from the DB · preview‑mode switches · halation preset by (use, AH) · grain preset · couplers preset · debug and lut_mode overrides |
| 2 | **Pipeline init** (`SimulationPipeline.__init__`) | profiles + params | prepared curves and services | sample density curves from the model · pick the B&W development time · bake the film chemistry morph (total + layers) · `tc_lut` bake (sensitivity × colour filter × window × surface → input gamut remap) · mid‑grey print‑balance reference (`density_spectral_midgray`, plus the `_comp` variant) |
| 3 | **preprocess** (`tap rgb_in → rgb_pre`) | RGB | RGB | first 3 channels · `pixel_size_um = format_mm·1000 / long_edge` · stash a 256 px full‑frame preview for metering · crop · upscale (bicubic, pixel_size /= factor) |
| 4 | **filming.expose** (`rgb_pre → log_e_film`) | linear RGB | log10 film raw (normalised so mid‑grey = 1) | RGB → raw (hanatos2025 `tc_lut` bicubic ×b, or mallett2019) · auto‑exposure · × 2^EV · highlight boost · camera diffusion filter · camera lens blur · scatter + halation · B&W reference exposure correction (positive scan only) · log10(max(raw,0)+1e‑10) |
| 5 | **filming.develop** (`log_e_film → cmy_film`) | log raw | CMY (or 1‑ch) dye density above the minimum | normalise curves (− min) · interpolate density · **DIR couplers** (matrix, Langmuir, pre‑corrected curves, spatial diffusion of the correction, redevelop) · **grain** (per channel × sub‑layer Poisson‑Binomial, dye‑cloud blur, micro‑structure clumping, blur, multiplicative USM, − density_min) |
| 6 | **printing.expose** (`cmy_film → log_e_print`) | film density | log10 print raw | [optional 17³ LUT] spectral density = cmy·channel_densityᵀ + base · light = 10^−D · (illuminant × CMY dichroic filters) · raw = light · paper sensitivity · × mid‑grey exposure factor · + pre‑flash · × print_exposure · × B&W print correction · enlarger diffusion filter · log10 |
| 7 | **printing.develop** (`log_e_print → cmy_print`) | log raw | print density (absolute) | print chemistry morph on the model (or the plain curves) → interpolate |
| 8 | **scanning.scan** (`cmy_print → rgb_out`) | print density | output RGB (encoded) | [optional 17³ LUT] density → spectral (+ base tuning) → light under the viewing illuminant → XYZ / normalisation · black/white Y correction · glare · XYZ → RGB (adapting from the viewing illuminant) · output gamut compression · scanner lens blur · unsharp mask · output CCTF |
| 9 | **Display / save** (GUI) | output RGB | screen / file | display ICC transform · saving colour space / CCTF conversion · ICC + EXIF |

**"Scan film" route** (positive film, or the negative‑scan view): 0–5, then 8 using the **film**
channel density, film base and the film viewing illuminant, with no glare and density bounds
`[−density_min, max curve]`.

**Data that depends only on parameters** (precompute once per parameter change, not per pixel):
the sampled and morphed curves, DIR matrix and pre‑corrected curves, Langmuir constants, grain
particle counts per (sub‑layer, channel), `tc_lut`, filtered enlarger and pre‑flash response
(81×3), the mid‑grey exposure factor, black/white reference Y values, the XYZ→RGB matrix, gamut
C_max tables, diffusion PSF components, halation weights.
