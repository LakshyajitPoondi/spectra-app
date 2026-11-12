# Windows setup and local checks

The GPU backend cannot be verified in CI on real hardware (CI has no GPU; it runs the Vulkan
code on a software renderer). The script below builds everything on your machine, runs the GPU
parity suites and the benchmarks on your GPU, and writes one report file.

## Install once

| What | Why | Where |
|---|---|---|
| **Visual Studio 2022** (Community is fine) with the workload **"Desktop development with C++"** | MSVC compiler, CMake, Ninja, and a bundled copy of vcpkg | https://visualstudio.microsoft.com/ |
| **Git for Windows** | cloning, and the report records the commit | https://git-scm.com/ |
| **Current Intel Arc graphics driver** | provides the Vulkan runtime for the Arc 140T | Intel Driver & Support Assistant |
| **Python 3.8+** (optional) | only for `scripts\fetch_test_raws.py`, which downloads the public RAW samples | https://www.python.org/ |

Not needed: the Vulkan SDK (the compiled shaders are in the repository; headers and loader come
from vcpkg), Qt (first needed in phase 4), any Python packages.

The libraries (LibRaw, libtiff, libpng, libjpeg-turbo, OpenEXR, Vulkan headers and loader) are
built by vcpkg on the first configure. That takes roughly 10 to 20 minutes once and is cached.

If you use a standalone vcpkg instead of the one bundled with Visual Studio, set `VCPKG_ROOT`
to its folder.

## Put your RAW files in place (optional but wanted)

Copy your Canon EOS R6 `.CR3` files into `test_data\raw\canon_r6\`. The folder is git-ignored.
The list of wanted scenes is in `test_data\SOURCES.md`.

## Run

Open **"Developer PowerShell for VS 2022"** from the Start menu, go to the repository, and run:

```powershell
.\scripts\run_local_checks.ps1
```

If PowerShell refuses to run scripts, allow it for this session first:

```powershell
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass
```

Options:

- `-Gpu "Arc"`: pick the GPU by name when the machine has more than one.
- `-SkipBuild`: reuse the existing build.
- `-Quick`: skip the 60 MP tiling and memory benchmark.

## What it does

1. Records the CPU, RAM, OS, GPU and driver version.
2. Configures and builds (Release).
3. Downloads the public RAW samples (CC0, from raw.pixls.us) if Python is present.
4. Lists the Vulkan devices.
5. Runs the CPU tests: parity against the Python reference, file I/O, Extended features.
6. Runs the GPU tests on your GPU: parity against the Python reference for every suite, tiled
   against untiled rendering, automatic tiling from the memory budget.
7. Decodes every RAW file found and renders a preview on the GPU into `test_data\out\`.
8. Benchmarks at 20 MP and 30 MP: full-resolution render time, interactive preview time with a
   per-stage breakdown, RAW open to first preview for your files, peak GPU and process memory.
9. Benchmarks 60 MP (tiled) and fails if the process exceeds 10 GB.

The report is `reports\local_check_<date>_<time>.txt`. Paste it back, or attach the file. The
preview JPEGs in `test_data\out\` are useful too if anything looks wrong.
