# Licensing analysis

> This is an engineering summary, not legal advice. If you plan to sell the app or ship it closed‑source,
> have someone qualified review §4.

## 1. Licences of the two reference repositories

### 1.1 `andreavolpato/spektrafilm`

There are two separate licences, and the split matters:

| What | Licence | Evidence |
|---|---|---|
| **Source code** (`src/spektrafilm`, `src/spektrafilm_gui`, `src/spektrafilm_lut_creator`, scripts, tests) | **GNU GPL v3**. `CITATION.cff` declares `GPL-3.0-or-later`. `pyproject.toml` says "GPLv3". `LICENSE` is the plain GPLv3 text. | `LICENSE`, `CITATION.cff`, README |
| **Profiles** (`src/spektrafilm/data/profiles/*.json`) **and all direct derivatives** (copies, LUTs, "any other format that encodes the same content") | **CC BY‑SA 4.0** plus a preamble (v1.1, 2026‑05‑28) | `SPEKTRAFILM_LICENSE.txt`; the `metadata.license` field inside every profile JSON |

The CC BY‑SA 4.0 obligations (the legal floor) are:
- Credit **Andrea Volpato**, link **https://github.com/andreavolpato/spektrafilm**, and include the
  licence notice. This applies on every copy, "in an app's About screen" included.
- **Share‑alike.** Modified profiles or LUTs must stay CC BY‑SA 4.0.
- **Mark modifications.** The author asks you to keep `SPEKTRAFILM_LICENSE.txt` unchanged and to
  describe changes in a `CHANGELOG.txt` shipped alongside it.
- Removing the attribution ends the licence grant (CC BY‑SA §6(a)).

The preamble also lists **non‑binding requests** that you should honour anyway:
- Don't use "spektrafilm" or the author's name in **product branding** without asking. Factual use
  ("uses spektrafilm profiles") is welcome.
- Paid apps that add real value are fine. Don't sell the profiles or LUTs themselves as a paid
  LUT pack.
- Don't train **commercial** AI models on the LUTs or their outputs.

Other data bundled in the upstream repo:

| Data | Licence / provenance | Action for us |
|---|---|---|
| Hanatos spectral upsampling LUTs (`irradiance_xy_tc.npy`, `hanatos_irradiance_xy_coeffs_250304.lut`) | Provided by Johannes Hanika (hanatos/vkdt). vkdt itself is BSD‑2‑Clause (**verify**). No separate licence file sits in the folder. | Credit hanatos / vkdt in THIRD_PARTY_NOTICES. Treat as GPL‑distributed data from the upstream repo. |
| ICC profiles: `ellelstone/*` | CC BY‑SA 3.0 Unported | Ship with attribution and licence text, only if we embed ICC profiles. |
| ICC profiles: `saucecontrol/*` | MIT | Ship the MIT notice. |
| Filter spectra (Thorlabs / Edmund / Durst dichroics, Schott KG1/3/5, Hoya X0/X1/Y2/YA3/R1, Canon lens transmission) | Digitised from vendor datasheets and blog measurements; no explicit licence | Low risk, but provenance is unclear (RISKS R‑19). Keep the upstream `info.txt` attributions. |
| `neutral_print_filters.json`, `presets/*.toml` | Part of the upstream package (GPLv3). These are derived from the profiles, so treat them as CC BY‑SA too. | Ship with both notices. |
| Datasheet‑derived curves (Kodak, Fujifilm) | The profile metadata says "Original data are property of the respective holders". | Same as upstream. Nothing extra we can do. |

**A note on the README wording.** The upstream README says LUTs are on a strict *"commercial use,
free share, no resale" custom license*. It also claims that GPL "derivative work includes any …
tool … directly inspired by its methods". Neither statement matches the actual licence files: the
LUT licence is CC BY‑SA 4.0 with non‑binding requests, and the GPL covers copyrightable expression,
not methods. **Follow the licence files, and honour the stated wishes as a courtesy.** RISKS R‑18
covers this.

### 1.2 `chaert-s/spektrafilm-ofx`

| What | Licence |
|---|---|
| **Source repository** (C++, GLSL, Metal, Python tools) | **GNU GPL v3** (`LICENSE.txt`) |
| *Official binary distributions* by Aedan Diez | A custom proprietary notice (`Legal/SPEKTRAFILM_OFX_LICENSE.txt`). It applies **only** to those official binaries and their non‑GPL bundled resources. |
| LUTs exported by the *official* OFX plugin | A custom "Exported LUT License" (no resale, no LUT packs). It applies only to LUTs exported from that plugin. |
| OpenFX SDK headers | BSD‑3‑Clause (vendored, not in the repo) |
| **SMPTE ST 2065‑2** CSVs (Academy Printing Density) | **Licensed standards data. Not in the repo and not redistributable.** The build turns the "Printer lights / APD" mode off when they are missing. |

For us this means:
- We may study, port and **reuse OFX source code** (shaders, renderer techniques, generator scripts)
  as long as our app is GPLv3‑compatible. Keep Aedan Diez's copyright notices on any file we copy
  or adapt.
- The proprietary binary notice and the exported‑LUT licence **do not bind code we build from the
  GPL source**. We must not copy those restrictive licences into our project either: GPLv3 §10
  forbids adding further restrictions to GPL code.
- **Do not ship the APD / printer‑lights feature with SMPTE data.** The only option would be to let
  users supply their own legally obtained ST 2065‑2 files at runtime, which is a low priority.

## 2. What this means for the app

1. **The app is a derivative work of GPLv3 code.** It will be a translation of spektrafilm's
   algorithms, and probably reuse OFX shader code. So the **whole application must be distributed
   under GPLv3 or later.** In practice:
   - **Distribution:** every binary you ship (installer, ZIP, Microsoft Store, winget) must come
     with the full corresponding source, or a written offer for it. A public GitHub repo tagged at
     each release satisfies this. Include the licence text and keep copyright notices.
   - **Selling is allowed.** GPLv3 permits charging for binaries, but buyers can redistribute them
     for free.
   - **No closed‑source modules** that link into the app. Plugins loaded in‑process would also be
     covered.
   - **Patent clause** (GPLv3 §11) and **anti‑tivoization** (§6, "Installation Information") apply.
     The second one is irrelevant on Windows desktop, since users can install a modified build.
2. **Profile data stays CC BY‑SA 4.0.** That covers the JSON, our exported binary pack (a "direct
   derivative" in a new format), and any LUTs the app exports.
   - Ship `SPEKTRAFILM_LICENSE.txt` unmodified next to the data pack, plus a `CHANGELOG.txt`
     describing our conversion ("re‑encoded to binary pack format vX; values unchanged" or
     whatever applies).
   - Credit "spektrafilm by Andrea Volpato — https://github.com/andreavolpato/spektrafilm — CC BY‑SA
     4.0" in the About box and docs. Write the same attribution into the header of every exported
     LUT (`.cube` `TITLE` / comments, OCIO description).
   - CC BY‑SA 4.0 is **one‑way compatible with GPLv3** (Creative Commons declared this in 2015), so
     bundling the two is fine.
3. **Third‑party dependencies** in the planned stack are all GPLv3‑compatible:

   | Dependency | Licence | Notes |
   |---|---|---|
   | Qt 6 (Core, Gui, Widgets, plus the RHI/Vulkan bits) | LGPLv3 / GPLv3 (open‑source option) | Dynamic linking is easiest. Under GPLv3 everything is fine anyway. |
   | Vulkan loader / headers, SPIR‑V tools, glslang / shaderc (build only) | Apache‑2.0 / MIT | |
   | Vulkan Memory Allocator | MIT | |
   | VkFFT (optional) | MIT | |
   | OpenImageIO | Apache‑2.0 | Drags in OpenEXR (BSD‑3), libtiff, libpng, libjpeg‑turbo (BSD‑like) |
   | LibRaw | LGPL‑2.1 or CDDL‑1.0 | Pick LGPL‑2.1. It is GPL‑compatible. |
   | lensfun | LGPL‑3.0; **database CC BY‑SA 3.0** | Attribute the database. |
   | exiv2 | GPL‑2.0‑or‑later | Compatible through "or later". |
   | Little CMS 2 | MIT | |
   | oneTBB, Highway | Apache‑2.0 | |
   | nlohmann/json or simdjson, fmt, spdlog | MIT | |
   | colour‑science (build‑time exporter only, not shipped) | BSD‑3‑Clause | Exported numeric tables are CIE / ACES / standards data. Credit colour‑science. |
   | GoogleTest or Catch2 (tests only) | BSD‑3 / BSL‑1.0 | |

   Avoid any dependency under GPL‑2.0‑**only** (incompatible with GPLv3) or a proprietary SDK
   licence. For example, don't ship Intel oneAPI components with restrictive redistribution terms;
   the plain Vulkan driver path needs none.

## 3. Recommended licence for this project

| Part | Licence |
|---|---|
| All application source code (C++, GLSL, CMake, tools) | **GPL‑3.0‑or‑later** (SPDX header `// SPDX-License-Identifier: GPL-3.0-or-later` in every file). This matches upstream's `CITATION.cff`, and OFX code (GPLv3) can be merged in. |
| Exported data pack (profiles, presets, neutral filters, spectra) | **CC BY‑SA 4.0**, with `SPEKTRAFILM_LICENSE.txt` unchanged plus `CHANGELOG.txt` |
| Our own documentation | CC BY‑SA 4.0 (simplest alongside the data) |
| LUTs exported by the app | CC BY‑SA 4.0, with attribution embedded in the file header |
| Repo housekeeping | `LICENSE` (GPLv3 text), `LICENSES/` folder with every third‑party licence, `THIRD_PARTY_NOTICES.md`, and an About dialog that lists all of the above |

**Naming.** Don't put "spektrafilm" in the product name or icon. "spectra‑app" is distinct.
Before release, check that the final name doesn't collide with an existing photo‑app trademark.
Use factual phrasing such as "film simulation powered by spektrafilm profiles".

## 4. If you ever want a non‑GPL (closed or paid‑proprietary) app

This is only possible with **written permission (a dual licence) from Andrea Volpato**, who says in
the README that he is open to discussing this. You would also need permission from **Aedan Diez**
for any OFX‑derived code, or you would have to avoid that code. The profile data would remain
CC BY‑SA 4.0 regardless, shipped as a separate, attributed data file. A "clean‑room
reimplementation from the published methods" is legally plausible but contradicts the author's
stated wishes. It is not recommended.
