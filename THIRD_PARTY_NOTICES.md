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
  performance reference. The Extended features (push / pull, bleach bypass, HDR output) in
  `libs/model/src/extended.cpp` and the matching shader code are adapted from its sources;
  the files carry the attribution.

## Bundled source

- **nlohmann/json 3.11.3** (`third_party/nlohmann/json.hpp`), MIT License,
  Copyright (c) 2013-2023 Niels Lohmann.
- **pocketfft** (`third_party/pocketfft/pocketfft_hdronly.h`), BSD-3-Clause,
  Copyright (C) 2010-2024 Max-Planck-Society, Peter Bell and contributors.
  The licence text is at the top of the header.

## Fetched at build time

- **LibRaw 0.22.0** (https://github.com/LibRaw/LibRaw, commit `0b56545a`), LGPL-2.1 / CDDL-1.0, used
  under LGPL-2.1. Copyright (C) 2008-2025 LibRaw LLC. It is the release the Python reference
  decodes with (through rawpy). It is linked statically; this program is GPL-3.0-or-later and its
  complete source is available, which satisfies the LGPL's relinking requirement.

## Libraries linked at build time

- **Qt 6** (Core, Gui, Widgets), LGPL-3.0, Copyright (C) The Qt Company Ltd. and other
  contributors. Linked dynamically; the installer ships the unmodified Qt DLLs, which can be
  replaced. Source: https://download.qt.io/official_releases/qt/
- **libtiff** (libtiff licence), **libpng** (PNG Reference Library License), **libjpeg-turbo**
  (IJG / BSD-3-Clause), **OpenEXR** and **Imath** (BSD-3-Clause), **zlib** (zlib licence),
  **Vulkan headers and loader** (Apache-2.0 / MIT).
- The Windows installer is built with **NSIS** (zlib/libpng licence) and includes the Microsoft
  Visual C++ runtime.
- RAW test samples from https://raw.pixls.us are CC0; they are downloaded, not redistributed.
  The screenshot in the README shows one of them (Nikon D750 sample).

## Reference GUI metadata

- `apps/spectra-app/resources/params_manifest.json` is exported from spektrafilm's
  `spektrafilm_gui/params_manifest.py` (GPL-3.0): the labels, ranges, steps and tooltips of the
  parameters, so the app presents them as the reference GUI does.

## Sampling algorithms

- Philox4x32-10: Salmon, Moraes, Dror, Shaw, "Parallel Random Numbers: As Easy as 1, 2, 3", 2011.
- PTRS and BTRS: W. Hoermann, "The transformed rejection method for generating Poisson
  random variables" (1993) and "The generation of binomial random variates" (1993).
