# Third-party notices

This project is licensed under GPL-3.0-or-later (see `LICENSE`).

## Algorithms and data

- **spektrafilm** by Andrea Volpato, https://github.com/andreavolpato/spektrafilm.
  The simulation code in `libs/` is a C++ port of the `dev` branch (commit `6cd00c8`),
  which is licensed GPL-3.0-or-later.
- **spektrafilm profiles and derived data** (`data/spektrafilm.sfpk`): by Andrea Volpato,
  licensed CC BY-SA 4.0. See `data/SPEKTRAFILM_LICENSE.txt` and `data/CHANGELOG.txt`.
  Original film and paper data are the property of their respective holders
  (Kodak and Fujifilm datasheets, scientific publications).
- **Spectral upsampling LUT** (`irradiance_xy_tc`): provided to spektrafilm by
  Johannes Hanika (hanatos / vkdt).
- **colour-science** (BSD-3-Clause), https://www.colour-science.org: numeric colorimetry
  tables in the data pack (CIE 1931 observer, illuminants, colour-space and adaptation
  matrices) were exported from colour-science 0.4.7. The library itself is not shipped.
- **spektrafilm-ofx** by Aedan Diez, https://github.com/chaert-s/spektrafilm-ofx (GPL-3.0):
  studied as a performance reference. No code from it is included in this phase.

## Bundled source

- **nlohmann/json 3.11.3** (`third_party/nlohmann/json.hpp`), MIT License,
  Copyright (c) 2013-2023 Niels Lohmann.
- **pocketfft** (`third_party/pocketfft/pocketfft_hdronly.h`), BSD-3-Clause,
  Copyright (C) 2010-2024 Max-Planck-Society, Peter Bell and contributors.
  The licence text is at the top of the header.

## Sampling algorithms

- Philox4x32-10: Salmon, Moraes, Dror, Shaw, "Parallel Random Numbers: As Easy as 1, 2, 3", 2011.
- PTRS and BTRS: W. Hoermann, "The transformed rejection method for generating Poisson
  random variables" (1993) and "The generation of binomial random variates" (1993).
