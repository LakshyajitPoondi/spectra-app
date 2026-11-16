# Windows setup and local checks

The GPU backend cannot be verified on real hardware in CI (CI has no GPU; it runs the Vulkan
code on a software renderer). The script below builds everything on your machine, runs every
test and benchmark on your GPU, and writes one report file.

## Install once

| What | Why | Where |
|---|---|---|
| **Visual Studio 2022** (Community is fine) with the workload **"Desktop development with C++"** | MSVC compiler, CMake, and a bundled copy of vcpkg | https://visualstudio.microsoft.com/ |
| **Git for Windows** | cloning; the build also fetches LibRaw 0.22.0 with it | https://git-scm.com/ |
| **Current Intel Arc graphics driver** | provides the Vulkan runtime for the Arc 140T | Intel Driver & Support Assistant |
| **Python 3.8+** | downloads the public RAW samples, and Qt when you pass `-InstallQt` | https://www.python.org/ (tick "Add python.exe to PATH") |
| **Qt 6** (6.4 or newer, MSVC 2022 64-bit build) | only the desktop app needs it | easiest: let the script fetch it with `-InstallQt` (about 1 GB into `.deps\Qt`, no Qt account needed). Or use the Qt online installer and pass `-QtDir C:\Qt\6.8.3\msvc2022_64`. |

Not needed: the Vulkan SDK (the compiled shaders are in the repository; headers and loader come
from vcpkg) and NSIS (only for building the installer yourself with `-Package`; CI builds it).

The libraries (libtiff, libpng, libjpeg-turbo, OpenEXR, zlib, Vulkan headers and loader) are
built by vcpkg on the first configure. That takes roughly 10 to 20 minutes once and is cached.
If you use a standalone vcpkg instead of the one bundled with Visual Studio, set `VCPKG_ROOT`
to its folder.

Without Qt the script still runs every GPU check through the command-line tool and says in the
report that the desktop app was skipped.

## Put your RAW files in place (optional but wanted)

Copy your Canon EOS R6 `.CR3` files into `test_data\raw\canon_r6\`. The folder is git-ignored.
The list of wanted scenes is in `test_data\SOURCES.md`. When the folder is empty the R6 test
reports "skipped" and everything else runs on the public samples.

## Run

Open **"Developer PowerShell for VS 2022"** from the Start menu, go to the repository, and run:

```powershell
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass
```

```powershell
.\scripts\run_local_checks.ps1 -InstallQt
```

Options:

- `-InstallQt`: download Qt 6.8.3 into `.deps\Qt` if no Qt is found.
- `-QtDir <folder>`: use this Qt (the folder that contains `bin` and `lib`).
- `-Gpu "Arc"`: pick the GPU by name when the machine has more than one.
- `-SkipBuild`: reuse the existing build.
- `-Quick`: skip the 60 MP tiling and memory benchmark.
- `-NoDownload`: do not download the public RAW samples.
- `-Package`: also build the installer (needs NSIS on `PATH`).

Expect 30 to 60 minutes the first time (vcpkg and the build take most of it), about 10 minutes
with `-SkipBuild`.

## What it does

1. Records the CPU, RAM, OS, GPU and driver version.
2. Finds (or downloads) Qt, configures and builds (Release).
3. Downloads the public RAW samples (CC0, from raw.pixls.us).
4. Lists the Vulkan devices.
5. Runs the CPU tests: parity against the Python reference, file I/O, Extended features, the
   render engine, the RAW test sets.
6. Runs the GPU tests on your GPU: parity against the Python reference for every suite, tiled
   against untiled rendering, the stage cache against fresh renders.
7. Runs the performance calibration the app runs on first launch and saves what it chose.
8. Runs the desktop app unattended: start, calibrate, preview, change a parameter, export an
   image and a LUT, on the GPU and again on the CPU fallback.
9. Decodes every RAW file found and renders it on the GPU into `test_data\out\`.
10. Interactive benchmark: RAW open to first preview, and the time of a slider change for
    each pipeline stage (targets: under 1.5 s and under 50 ms).
11. Batch export of three RAW files at full resolution, and a LUT export.
12. Full-resolution benchmarks at 20 MP and 30 MP with peak GPU and process memory, then 60 MP
    (tiled), which fails if the process exceeds 10 GB.

The report is `reports\local_check_<date>_<time>.txt`. Paste it back, or attach the file,
together with `reports\calibration_<date>_<time>.json`. The JPEGs in `test_data\out\` are
useful too if anything looks wrong.

## Trying the app itself

After the script has run, start `build\Release\spectra-app.exe`, or install the CI-built
installer (see the README). Things worth a look on real hardware, since no test can judge them:

- does the preview keep up while you drag a slider (draft pass, then refinement)?
- 100 % view (double-click): panning and the grain at 1:1;
- an export of a 20 MP file, and the time the status bar reports.
