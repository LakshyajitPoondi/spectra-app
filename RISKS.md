# Risks, ambiguities and porting hazards

Each item has an ID, a severity (H/M/L), what we found in the source, and a proposed mitigation.
References are to `spektrafilm@dev` (`6cd00c8`) unless noted.

## A. Scope and upstream

**R‑01 (H) — The upstream is a moving target with three diverging branches.**
- `main` and `dev` differ substantially. Dev adds B&W, grain presets, Langmuir couplers, colour
  filters, film chemistry and base tuning.
- `experimental` restructures the schema again: `workflow.route`, convert‑film negative inversion,
  `FilmBaseParams` / `PrintBaseParams`, new spectral upsampling methods, profile‑derived enums.
- The README states the project "is in rapid development".

*Mitigation:*
- Pin one SHA (proposal: `dev@6cd00c8`) per release.
- Keep everything upstream‑specific behind the exporter and the `Params` schema version.
- Design the stage graph around routes so experimental can be absorbed later.
- **Needs your decision:** baseline `dev` (recommended) or `main`, and whether experimental features
  are in scope.

**R‑02 (M) — `spektrafilm_profile_creator` is not available.**
It was extracted to an unpublished repo (commit `dfb0219`) and its sources were dropped in the
2026‑05‑28 history rewrite.

*Consequence:* we cannot fit new stocks, re‑derive profiles, or regenerate the Hanatos
window/surface fits or `neutral_print_filters.json`.

*Mitigation:* treat profiles as input data and support loading new upstream JSON directly. If you
need profile creation, ask the author for access.

**R‑03 (M) — "All features of spektrafilm" is ambiguous, because the OFX port has many features
the Python reference lacks.**
OFX‑only features:
- push/pull model, bleach bypass, leuco‑cyan coupling
- printer lights / APD (needs non‑redistributable SMPTE data)
- HDR PQ/HLG output, Resolve RCM
- "grain synthesis" model, film‑format presets, scanner MTF50
- enlarger scale/offset, camera UV/IR cut, "Hanatos2026" window, Process‑Negative mode
- `.cube` export from the UI, `.spkpreset` presets

These have no Python reference, so there is nothing to test parity against. The Python LUT creator
(`spektrafilm_lut_creator`, about 9k lines: CLI, 4 LUT formats, OCIO emission, QA suite) is also a
large separate product.

*Mitigation:* phase the work.
1. Python runtime + GUI parity.
2. LUT export.
3. OFX extras, flagged "non‑reference".

**Needs your decision.**

## B. Algorithmic hazards for GPU, tiling and preview

**R‑04 (H) — The diffusion filter is huge and resolution‑dependent in Python.**
- The PSF bloom reaches λ = 1000 µm × spread 2.5 (cinebloom), with kernel radius 8·λmax ≈ 20 mm
  on a 36 mm frame. Python clamps the radius to `min(H, W)/2 − 1` (`apply_diffusion_filter_um`).
  So **the truncation depends on image size**, and preview vs full‑res output differ by design.
- FFT convolution of a 61 MP frame per channel is memory heavy.
- This is the most expensive effect in OFX (2.1 s at 4K naive).

*Mitigation:*
- Exponential → Gaussian mixture → pyramid + clustering (OFX method, 10×).
- An optional VkFFT exact path.
- Decide whether to reproduce the half‑image clamp (parity) or use physically consistent truncation
  (better). This is a documented deviation either way.

**R‑05 (H) — Several operations depend on global image statistics, which breaks naive tiling and
makes preview ≠ full render.**
- `boost_highlights` normalises by the **global `np.max(raw)`**. A single hot pixel changes the
  boost curve for the whole image, and a crop or preview changes the max.
- The multiplicative USM in grain renormalises **per‑channel sums over the whole image**.
- Auto‑exposure meters a 256 px **nearest‑neighbour** downsample of the full frame. That is stable,
  but aliasing‑sensitive.
- The diffusion clamp (R‑04).

*Mitigation:*
- Global reductions run as explicit pre‑passes (ARCHITECTURE §3.7).
- For boost, consider a robust max (99.99th percentile) as an *opt‑in* deviation.
- Document that preview at reduced resolution can differ slightly from full res, as in Python.

**R‑06 (H) — Stochastic parity is only statistical.**
- Grain uses NumPy/Numba RNG streams: sequential, and per‑thread under `numba parallel`.
- It has two sampler sets: scipy exact (the `simulate()` default) and `fast_stats` approximations
  (the GUI default; direct simulation for n < 25, normal approximation when var > 10, inversion
  otherwise). The two give different distributions at low particle counts, i.e. large pixels or
  small previews.
- Glare uses a log‑normal random field.
- A GPU counter‑based RNG can never be bit‑identical.

*Mitigation:*
- Statistical tests: per‑density mean/std, RMS granularity at 48 µm, radial power spectrum.
- Implement both sampler modes.
- Make GPU output tile‑invariant and seed‑stable.

**R‑07 (M) — DIR "pre‑correction" uses `np.interp` on a possibly non‑monotonic axis.**
`compute_density_curves_before_dir_couplers` computes
`log_exposure_0 = log_exposure − couplers_amount_curves` and calls
`np.interp(log_exposure, log_exposure_0, …)`. With large `amount`, `inhibition_*` or gamma values,
`log_exposure_0` can become non‑monotonic, and NumPy then silently returns implementation‑defined
values.

*Mitigation:* replicate NumPy's exact algorithm (binary search on an assumed‑sorted axis) for
parity. Detect non‑monotonicity, warn in the UI, and clamp parameter ranges.

**R‑08 (M) — Several parameters are in pixels, not µm, so the look changes with resolution or
upscale.**
- `grain.blur` (0.8 px)
- `print_render.glare.blur` (0.5 px)
- `scanner.lens_blur`, `scanner.unsharp_mask` σ
- `grain.mult_usm_sigma`

There is also a **unit ambiguity**: in `layer_particle_model` the dye‑cloud blur σ is
`blur_dye_clouds_um * sqrt(od_particle)` **without dividing by `pixel_size_um`**. The parameter is
named "µm", but it is used as pixels.

*Mitigation:*
- Reproduce exactly for parity, and expose the units honestly in the UI.
- Raise the unit question upstream.
- An optional "resolution‑independent" mode would be a documented deviation.

**R‑09 (M) — Python preview mode disables effects.**
`digest_params(preview_mode = True)` turns off grain, DIR diffusion (`diffusion_size_um = 0`),
enlarger, camera and scanner blurs, glare blur and USM. So the Python GUI preview is *not* the same
look as its "Scan" output.

*Mitigation:* our GPU preview keeps effects on by default, with a "fast preview" toggle that
matches Python. Parity tests always compare like with like.

**R‑10 (L) — Docstring vs code conflicts.**
- `OutputGamutCompressSpec`: the docstring and tooltip say **`aces_rgc` (default)**, but the
  dataclass default is **`oklch`**. Commit `85717fb` set `aces_rgc`, and a later commit, `67b47b4`,
  set `oklch` back.
- `model/stocks.py` enums are hand‑maintained, while experimental derives them from the profiles.
- `film_render.glare` exists in the schema but is never used.
- The `compute_band_pass_filter` (camera UV/IR) exists but isn't wired in.

*Mitigation:* follow the **code**, and list each such choice in a `DEVIATIONS.md`.

**R‑11 (M) — Hidden colour‑science behaviour.**
- The input projection uses `colour.RGB_to_XYZ(…, chromatic_adaptation_transform = 'CAT16')`
  toward the film reference illuminant.
- The output uses `colour.XYZ_to_RGB(…, illuminant = viewing_xy)` with colour's **default CAT
  (CAT02)**.
- CCTFs follow colour's definitions (sRGB piecewise, ProPhoto, etc.).
- Illuminant data: `'Incandescent'` (T) and `'Kinoton 75P'` (K75P) from `SDS_LIGHT_SOURCES`, CIE
  D‑series, blackbody via Planck.
- The normalisation `sum/len` and the 81‑λ alignment use colour's interpolators.

Small differences in any of these show up as colour shifts.

*Mitigation:* export **numeric** tables and matrices from the pinned colour version (ARCHITECTURE
§6) rather than reimplementing them, and unit‑test the analytic CCTFs against exported samples.

**R‑12 (M) — Neutral print filter coverage is partial.**
`neutral_print_filters.json` covers only the **TH‑KG3** illuminant, 9 papers and 22 films. Other
combinations fall back to C0 M50 Y50, which may be visibly off-balance. That includes any other
enlarger illuminant, future stocks, and colour film on the B&W print 2302. The database was also
computed by the unavailable profile creator.

*Mitigation:*
- Show the user when the fallback is in use.
- Offer an "auto‑neutralise" that solves the CMY filters numerically for a grey print (an
  extension, not reference behaviour).

**R‑13 (M) — B&W and mixed‑channel paths are new and lightly exercised upstream.**
- 1‑channel profiles have development‑time families: (81 × N) base density, model rows per dev
  time.
- The morph uses only `gamma_factor_red` for B&W (channel index 0).
- `match_channels` takes the first RGB value for B&W.
- Colour film → B&W print needs a 3→1 LUT.
- B&W film → colour paper is untested territory.
- Coupler and grain presets for B&W reuse colour values ("roughly modeled after …").

*Mitigation:* specialization‑constant kernel variants, and explicit parity cases for every channel
combination. Mark unusual combinations "experimental" in the UI.

**R‑14 (L) — Density‑curve interpolation details.**
- `fast_interp` uses `searchsorted(side = 'right')` and endpoint clamping.
- The film curves are **normalised by subtracting the per‑channel minimum** before develop. The
  print curves are not.
- For positive film, grain uses negated axes (`fast_interp(−stacked, −density_curves …)`).
- The density grid is uniform (−3 … 4, 256 samples). That allows O(1) index math on the GPU, but
  only if every profile keeps a uniform grid.

*Mitigation:*
- The exporter asserts a uniform grid.
- A binary‑search fallback handles any non‑uniform profile.
- Bit‑level tests on breakpoints.

**R‑15 (L) — Resampling differences.**
- Upscale uses skimage `rescale(order = 3)`; GPU bicubic kernels differ.
- The preview resize uses skimage `resize(anti_aliasing = True, order = 1)`.
- Crop uses rounding conventions in `crop_image` (center flipped to row/col order).

*Mitigation:* replicate the crop math exactly. For resampling, compare after the resample tap with
looser tolerances, or inject at `rgb_pre`.

## C. Platform, GPU and performance

**R‑16 (M) — Intel Arc Vulkan driver maturity on Windows.**
- There is a history of compiler bugs with large shaders and loops.
- Subgroup size varies (8/16/32).
- WDDM shared‑memory limits on the iGPU.
- TDR resets on long dispatches.

*Mitigation:*
- Keep shaders moderately sized. The 81‑λ loop is unrolled by 3, like OFX.
- Request the subgroup size explicitly.
- Chunk submissions under ~200 ms.
- Device‑lost recovery plus CPU fallback.
- Test on the current Intel driver and record its version in perf logs.

**R‑17 (M) — Bandwidth‑bound unified memory.**
About 100 GB/s is shared with the CPU, and each full‑res RGBA32F pass at 24 MP costs about 8 ms.
A naive port, OFX baseline style, is 83 passes for "all effects": 3.4 s at 4K on its Metal
baseline.

*Mitigation:*
- Kernel fusion.
- FP16 storage only where validated.
- Pyramid blurs.
- Stage caching.
- Budget‑aware residency.
- Profile early (phase 4) with Vulkan timestamps and Intel GPA.

**R‑18 (M) — Licence statements upstream are inconsistent.**
The README says LUTs are under a "strict *commercial use, free share, no resale* custom license"
and that GPL covers tools "directly inspired by its methods". But `SPEKTRAFILM_LICENSE.txt` is
CC BY‑SA 4.0 with *non‑binding* requests. The OFX official‑binary and exported‑LUT licences are
proprietary, but they don't apply to builds from its GPL source.

*Mitigation:* follow the licence files, honour the stated wishes (no "spektrafilm" branding, no
paid LUT packs, full attribution), and ask the author if commercial distribution is planned
(LICENSES.md).

**R‑19 (L) — Data provenance.**
Filter spectra (Thorlabs, Edmund, Schott via Newport, Hoya, Canon via a Lensrentals blog) and
datasheet curves (Kodak, Fujifilm) have no explicit licence. The SMPTE ST 2065‑2 data used by OFX
APD mode is explicitly not redistributable.

*Mitigation:* ship only what upstream ships, with its attributions. Exclude APD.

**R‑20 (M) — RAW pipeline parity.**
Python uses rawpy (LibRaw) postprocess parameters (ACES output, WB modes), colour‑science CCT→xy
for custom WB, and a lensfun lookup heuristic (`_find_lens_candidates`, `_lens_model_score`).
Different LibRaw versions or demosaic settings change the input before film simulation even starts.

*Mitigation:*
- Pin the LibRaw version.
- Port the parameter mapping and lens‑matching heuristic verbatim.
- Test RAW → linear ACES separately from the film pipeline, with sample RAWs (we need a small
  licensed RAW test set).

**R‑21 (L) — Qt + Vulkan display integration.**
Qt 6 on Windows defaults to D3D for RHI. Embedding a Vulkan `QWindow` through
`createWindowContainer` has known focus/IME and high‑DPI quirks. HDR swapchains need extra work.

*Mitigation:* prototype the viewer early in phase 6, and keep a fallback that blits to a `QImage`
for robustness.

**R‑22 (L) — Python GUI cosmetics and display path.**
The Python GUI relies on napari features: interpolation modes `spline36`/`lanczos`/`blackman`, the
polaroid animation, the paper‑back watermark. Its display transform comes from Pillow `ImageCms`
(Windows‑only monitor profile).

*Mitigation:* reimplement the essentials (zoom filters, ICC display) and defer the cosmetics.

## D. Open questions for you (to settle before phase 2)

1. **Baseline:** `dev@6cd00c8` (recommended), `main`, or track `experimental`?
2. **Scope of "all features":** Python runtime + GUI only? Plus LUT creator? Plus the OFX‑only
   extras (push/pull, bleach bypass, HDR output, film formats)? And the experimental convert‑film
   negative inversion?
3. **Licence:** OK with **GPL‑3.0‑or‑later** for the app and CC BY‑SA 4.0 for the data pack? Any
   plan to sell it, which would change who we should contact?
4. **Parity target:** Python *reference* defaults (direct spectral, exact stats), the *GUI*
   configuration (17³ LUTs, fast stats), or both?
5. **Deviation policy:** where Python behaviour is resolution‑dependent (R‑04/05/08), match it
   exactly, or prefer physically consistent behaviour behind a flag?
6. **Machine RAM** on your 285H laptop (16 / 32 / 64 GB)? This sets the full‑frame vs tiled
   threshold.
