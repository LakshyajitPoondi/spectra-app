# Deviations and interpretation notes

Every place where this port knowingly differs from the Python reference
(`spektrafilm@dev`, commit `6cd00c8`), or where the reference's code and its documentation
disagree. Rule: **follow the code**, and write the choice down here.

## A. Differences from the reference

| # | What | Reference | This port | Effect on parity |
|---|---|---|---|---|
| D-01 | Random numbers | NumPy / Numba generators: sequential, fixed seeds per channel and sub-layer in exact mode, unseeded in fast mode | Philox4x32-10 counter-based generator keyed by (seed, effect, channel, sub-layer, x, y). Output does not depend on threads or evaluation order. | Stochastic effects are compared statistically (suite `t4_stochastic`), never pixel by pixel. |
| D-02 | Exact samplers | `scipy.stats.poisson.rvs`, `scipy.stats.binom.rvs` | Poisson: inversion (λ < 10), PTRS otherwise. Binomial: inversion (n·p < 10), BTRS otherwise. Both are exact samplers of the same distributions. | Tested against the analytic pmf. |
| D-03 | Grain working precision | float32 | float64 | None measurable. |
| D-04 | Diffusion-filter convolution | `scipy.signal.fftconvolve` in float32 | pocketfft in float64, same padding and kernel | Differences of about 1e-6 (the float32 rounding of the reference). These cases use a 3e-4 tolerance. |
| D-05 | Pixel-unit parameters (`grain.blur`, `grain.mult_usm_sigma`, `print_render.glare.blur`, `scanner.lens_blur`, `scanner.unsharp_mask` sigma) | Always in pixels of the image being rendered, so the look changes with resolution | Default: interpreted as pixels at a 6 µm reference pitch and scaled to the real pitch, so the look is the same at any resolution. `RenderOptions.match_python = true` restores the reference behaviour. | Parity tests run with `match_python = true`. |
| D-06 | Whole-image statistics | Recomputed from whatever image is passed in | Same by default. `RenderOptions` can supply the auto-exposure EV, the highlight-boost maximum and the full-image long edge, so a preview or crop reproduces the full render. | None when not used. |
| D-07 | Injecting past `rgb_in` | `pixel_size_um` is undefined (spatial effects crash) | Pixel size is derived from the injected image, or from `full_long_edge_px` | None for valid reference runs. |
| D-08 | Positive print stock with black/white correction | `black_white_printing_exposure_correction` returns `None` and the pipeline raises | Returns 1.0 | No positive print profile exists. |
| D-09 | B&W grain preset padding | Missing channels are `None` | Missing channels keep their previous value (never read for one channel) | None. |
| D-10 | Crop larger than the image | A negative slice start returns a sliver of the far edge | The crop is clamped to the image | Only for invalid crops. |
| D-11 | X-Trans RAW decode | rawpy bundles LibRaw 0.22.0 | LibRaw 0.22.0, fetched and built at the pinned commit `0b56545a` (`cmake/libraw.cmake`) | All Bayer test files (Canon R6 raw and C-RAW, Sony, Nikon x2, Panasonic) are now bit-identical to rawpy. Fujifilm X-T3 (X-Trans): mean relative difference 8.9e-6, 99.9th percentile 3.4e-4, a handful of pixels up to 0.029 (it was about 2 % with LibRaw 0.21.2). The remaining difference is in LibRaw's X-Trans interpolation between two builds of the same version; logged, not pursued. |
| D-12 | Autoexposure metering image in the app | 256 px metering preview reduced from the full frame with a Gaussian anti-aliasing filter (about 5 s at 20 MP in this port) | The app reduces by area to 1024 px first, then applies the reference's reduction. `spectra-cli render`, the parity tests and "Match Python exactly" use the reference's path unchanged. | Measured on three RAW files: at most 0.003 EV for `center_weighted` (default) and `matrix`, at most 0.02 EV for `highlight_weighted`. Needed for the 1.5 s open-to-preview target. |

## B. GPU backend: approximations and CPU fallbacks

Decision 4 requires every GPU approximation to be documented and shown to stay inside the
parity thresholds. The GPU works in 32-bit floats throughout.

| # | What | Reference / CPU | GPU | Evidence |
|---|---|---|---|---|
| G-01 | Arithmetic precision | float64 | float32 | All deterministic parity suites pass at 2e-3 (typical error 1e-6 to 1e-5). |
| G-02 | Gaussian, sigma < 3 px | FIR truncated at 3 sigma, reflect boundary | Same filter | Exact port. |
| G-03 | Gaussian, 3 ≤ sigma ≤ 24 px | Young - van Vliet recursive filter, replicated boundary | Same recursion in fp32, one line per invocation | Exact port; spatial suite passes at 2e-3. |
| G-04 | Gaussian, sigma > 24 px (halation and DIR tails at high resolution) | Recursive filter at full resolution | The plane is reduced 2^k times with box filters, filtered with the recursive filter, and enlarged bilinearly. The low-resolution sigma accounts for the variance the box and bilinear steps add. Reason: the fp32 recursion loses accuracy for very wide kernels. | Reported separately by the parity runner ("approximate-path thresholds": RMSE ≤ 0.003, p99 ≤ 0.01). One case in the spatial suite takes this path: RMSE 0.0017, on a 96-px image where the kernel is over half the image. |
| G-05 | Grain counts with variance > 400 | Exact Poisson / binomial sampling | Rounded normal with the same mean and variance. Reason: fp32 cannot evaluate the rejection tests for large counts. | Skewness of the exact distribution is below 0.05 there. All 361 statistical cases pass on the GPU, including the sampler distribution tests. |
| G-06 | Grain random numbers | Philox4x32-10, 24-bit uniforms | Identical generator and uniforms | CPU and GPU grain agree statistically; single counts can differ where fp32 rounding flips a rejection test (RMS 0.012 between backends, mean bias 5e-5). |
| G-07 | Fast preview | 17³ LUTs for the spectral stages | Direct 81-wavelength evaluation (more accurate and cheap on a GPU) | Compared at the fast-preview threshold (0.015): worst 0.0042. |
| G-08 | Tiled rendering: grain unsharp-mask renormalisation | Per-channel sums over the whole image | Sums over each padded tile | Tiled vs untiled: 6e-6 without grain, RMS 1e-4 with grain. |
| G-09 | Tiled rendering: border | — | Every tile is padded by 4 sigma of each active spatial effect | Same test. |

Stages that still run on the CPU when used (the render reports them):

| # | Stage | Note |
|---|---|---|
| F-01 | Camera and enlarger **diffusion filters** | CPU FFT, as the reference. Off by default. **Not available on tiled renders (60 MP and up, or a small GPU budget) in v1**: the app shows a notice above the preview and blocks the export with an explanation; the CLI stops with a clear error. A tileable version is on the v2 list. |
| F-02 | Output gamut algorithms `jzazbz` and `cam16ucs` | `off`, `aces_rgc`, `oklch` (default) and `oklrab` run on the GPU. |
| F-03 | `mallett2019` spectral upsampling | `hanatos2025` (default) runs on the GPU. |
| F-04 | Auto-exposure metering, crop, upscale | Small or one-off work on the input image. |

## B1. GPU execution (no effect on results)

| # | What | How | Verified by |
|---|---|---|---|
| X-01 | Batched submissions | Dispatches are recorded into one command buffer with a memory barrier between them and submitted in batches bounded by dispatch count and by pixels touched. | `engine` test: batched and unbatched renders are bit-identical; 78 dispatches go out in 16 submissions. |
| X-02 | Persistent buffers | Freed planes are pooled and reused by size, bounded to 1.5 planes on top of the working set; parameter buffers are host-visible. | All GPU parity suites; memory figures in `PROGRESS.md`. |
| X-03 | Stage cache | The outputs of film exposure, development, grain and print are kept on the GPU per preview size; a parameter change re-runs only the stages after it. Which stage a parameter belongs to is a table in `GpuPipeline::stage_keys`; anything not listed re-renders everything. | `engine` test: for 153 single-parameter changes (every numeric, boolean and enumerated parameter, with black / white correction and the Extended features switched on), the cached render equals a fresh render exactly (max difference 0). |
| X-04 | Cancellation | A render is abandoned at the next batch boundary when a newer request exists. | `engine` test: no GPU memory is left behind and the next render is correct. |

## B4. Preview (app only; full renders and exports are not affected)

| # | What | Preview | Final render |
|---|---|---|---|
| P-01 | Border around a 100 % view | 128 px of context around the visible part. Effects with a longer reach (DIR-coupler diffusion tail, halation at high resolution) are slightly truncated near the edge of the view. | Tiles carry the full support of every active effect. Measured in the centre of a view: identical to the full render. |
| P-02 | Reduced previews | The fitted preview is an area-averaged reduction rendered with the resolution-independent parameter scaling (pixel-unit parameters are scaled to the film format). Grain is rendered at the preview's pixel size, so it looks finer than at 100 %; judge grain in the 100 % view. | Full resolution. |
| P-03 | Fast first preview of a RAW file | LibRaw half-size decode (no demosaic interpolation) until the full decode arrives. | Full decode. |
| P-04 | Highlight-boost maximum | Measured once on the loaded image and reused (per the phase 1 decision). On the CPU fallback it is taken from the preview itself. | Measured on the full image. |
| P-05 | Preview colour | Always sRGB with its transfer function; HDR export is not previewed. | The export colour space and transfer function. |

## B2. Not ported

| # | What | Note |
|---|---|---|
| N-01 | Lens correction (lensfun) | Optional in the reference, off by default. |
| N-02 | `logiflex8` adaptation window | No shipped profile uses it (all use `erf4`). |
| N-03 | EXIF copy and ICC embedding on save | Not done in v1: exported files carry no metadata and no ICC profile (sRGB is assumed by viewers). On the v2 list. |
| N-04 | Crop / upscale controls of the reference GUI | The parameters work (`io.crop`, `io.upscale_factor`, parity-tested) but the app does not expose them; the 100 % view replaces the reference's crop preview. |
| N-05 | Reference GUI extras | Channel swap, display transform through the monitor profile, white padding, napari layers. Not part of the model. |

## B3. Extended features (no reference)

Push / pull, bleach bypass and HDR output are adapted from spektrafilm-ofx and have no Python
counterpart. Differences from the OFX models, made so they fit this pipeline:

- **Standard push / pull** scales the exposure axis of the whole film development (curves and
  DIR couplers) about mid-grey. The reference's own `gamma_factor` could not be used: combined
  with the DIR-coupler inversion it becomes non-monotonic at push-level gammas (measurable from
  `gamma_factor` 1.22 on Provia; see RISKS R-07).
- **Print re-timing** (`extended.retime_print_to_midgray`, on by default): when a film-side
  Extended feature is active, each print channel is re-timed so mid-grey prints as with the
  normal process. Without it bleach bypass shifts the grey balance by up to 0.14.

## C. Where the reference's code and documentation disagree (we follow the code)

| # | Topic | Documentation says | Code does |
|---|---|---|---|
| C-01 | Default output gamut algorithm | `aces_rgc` | `oklch` |
| C-02 | `enlarger.lens_blur` | A blur of the enlarger lens | Never applied by the printing stage. It only appears in the digest switches. |
| C-03 | Auto-exposure metering preview | "256 px nearest-neighbour" | `skimage.rescale(order=0)` applies a Gaussian anti-aliasing filter first, then nearest sampling. Ported exactly. |
| C-04 | `film_render.glare` | In the schema | Unused; only `print_render.glare` has an effect. |
| C-05 | Base-density tuning | "Film base" controls | The same `film_render.base` controls are applied to whichever medium is scanned, print paper included. |
| C-06 | Mid-grey print reference | — | Computed from 0.184 grey in **sRGB primaries without CCTF**, independent of the input colour space. |
| C-07 | `settings.spectral_gaussian_blur` | "sigma in nm" | Used as a sigma in samples of the 5 nm grid. |
| C-08 | `RGB_to_RGB(x, cs, cs)` | Identity | Not exactly the identity for sRGB (colour-science's two published matrices are not exact inverses; about 2e-5). Reproduced, because the output CCTF step goes through it. |
| C-09 | `matrix` metering on a 1×1 image | — | Yields no zones, an infinite EV and therefore 0 EV. Affects the mid-grey reference when `auto_exposure_method = matrix`. Reproduced. |

## D. Corrections to the phase 1 notes

- **RISKS R-08, dye-cloud blur "unit bug":** not a bug. The blur is
  `blur_dye_clouds_um · sqrt(od_particle)` pixels and `od_particle` is inversely proportional to
  the pixel area, so the blur is a fixed physical length. It needs no resolution correction.
- **RISKS R-04, diffusion kernel clamp:** the clamp is half the short image side, which is a fixed
  fraction of the frame. It does not change with resolution, only with cropping.
- **FEATURE_MATRIX, "Enlarger lens blur ✅":** see C-02.
