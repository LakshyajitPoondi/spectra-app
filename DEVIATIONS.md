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

## B. Not ported yet (the call fails with a clear error)

| # | What | Note |
|---|---|---|
| N-01 | Output gamut algorithm `cam16ucs` | `off`, `aces_rgc`, `oklch` (default), `oklrab` and `jzazbz` are ported and tested. |
| N-02 | `io.upscale_factor != 1` | Needs scikit-image's order-3 spline resampling. |
| N-03 | `logiflex8` adaptation window | No shipped profile uses it (all use `erf4`). |

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
