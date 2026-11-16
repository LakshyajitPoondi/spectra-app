# SPDX-License-Identifier: GPL-3.0-or-later
<#
.SYNOPSIS
  Builds the project, runs every test and the benchmarks on this machine's
  GPU, and writes one report file to paste back.

.DESCRIPTION
  Run from a "Developer PowerShell for VS 2022" (or any PowerShell where
  cmake is on PATH) in the repository root:

      .\scripts\run_local_checks.ps1

  Prerequisites are listed in docs\WINDOWS_SETUP.md. The report is written to
  reports\local_check_<date>_<time>.txt.

.PARAMETER Gpu
  Part of the GPU name to use (for example "Arc"). Default: the best device found.

.PARAMETER SkipBuild
  Reuse an existing build directory.

.PARAMETER Quick
  Skip the 60 MP tiling / memory benchmark.

.PARAMETER QtDir
  Qt 6 folder that contains bin\ and lib\ (for example C:\Qt\6.8.3\msvc2022_64).
  Default: found automatically (QT_ROOT_DIR, C:\Qt, .deps\Qt). Without Qt the
  desktop app is not built; every GPU check still runs through the CLI.

.PARAMETER InstallQt
  Download Qt 6.8.3 into .deps\Qt with aqtinstall (needs Python 3) when no Qt
  is found.

.PARAMETER NoDownload
  Do not download the public RAW samples.

.PARAMETER Package
  Also build the installer with cpack (needs NSIS on PATH).
#>
param(
    [string]$BuildDir = "build",
    [string]$Gpu = "",
    [string]$VcpkgRoot = $env:VCPKG_ROOT,
    [string]$QtDir = "",
    [switch]$InstallQt,
    [switch]$Package,
    [switch]$NoDownload,
    [switch]$SkipBuild,
    [switch]$Quick
)

$ErrorActionPreference = "Continue"
$repo = Split-Path -Parent $PSScriptRoot
Set-Location $repo
New-Item -ItemType Directory -Force -Path "reports" | Out-Null
$stamp = Get-Date -Format "yyyyMMdd_HHmmss"
$report = Join-Path $repo "reports\local_check_$stamp.txt"
$failed = @()

function Log([string]$text) {
    Write-Host $text
    Add-Content -Path $report -Value $text -Encoding utf8
}

function Section([string]$title) {
    Log ""
    Log "==================== $title ===================="
}

# Runs a command, copies its output to the report, returns the exit code.
function Step([string]$title, [string]$exe, [string[]]$arguments) {
    Section $title
    Log ("> " + $exe + " " + ($arguments -join " "))
    $output = & $exe @arguments 2>&1
    $code = $LASTEXITCODE
    foreach ($line in $output) { Log ([string]$line) }
    Log "exit code: $code"
    if ($code -ne 0) { $script:failed += $title }
    return $code
}

Log "spectra-app local check, $stamp"
Log ("commit: " + (git rev-parse HEAD 2>$null))
Log ("branch: " + (git rev-parse --abbrev-ref HEAD 2>$null))

Section "System"
try {
    $cpu = Get-CimInstance Win32_Processor | Select-Object -First 1
    $mem = Get-CimInstance Win32_ComputerSystem
    $os = Get-CimInstance Win32_OperatingSystem
    Log ("CPU: " + $cpu.Name + " (" + $cpu.NumberOfCores + " cores, " + $cpu.NumberOfLogicalProcessors + " threads)")
    Log ("RAM: " + [math]::Round($mem.TotalPhysicalMemory / 1GB, 1) + " GB")
    Log ("OS:  " + $os.Caption + " " + $os.Version)
    foreach ($g in (Get-CimInstance Win32_VideoController)) {
        Log ("GPU: " + $g.Name + ", driver " + $g.DriverVersion + " (" + $g.DriverDate + ")")
    }
} catch {
    Log "could not read system information: $_"
}

if ($Gpu -ne "") { $env:SPECTRA_GPU = $Gpu }

# ---- Qt 6 (only the desktop app needs it) ----
function Find-Qt {
    $candidates = @()
    if ($QtDir -ne "") { $candidates += $QtDir }
    if ($env:QT_ROOT_DIR) { $candidates += $env:QT_ROOT_DIR }
    foreach ($root in @((Join-Path $repo ".deps\Qt"), "C:\Qt")) {
        if (Test-Path $root) {
            $candidates += Get-ChildItem -Path $root -Directory -Filter "6.*" -ErrorAction SilentlyContinue |
                Sort-Object Name -Descending |
                ForEach-Object { Get-ChildItem -Path $_.FullName -Directory -Filter "msvc*_64" -ErrorAction SilentlyContinue } |
                ForEach-Object { $_.FullName }
        }
    }
    foreach ($c in $candidates) {
        if ($c -and (Test-Path (Join-Path $c "lib\cmake\Qt6\Qt6Config.cmake"))) { return $c }
    }
    return ""
}
Section "Qt"
$qt = Find-Qt
if ($qt -eq "" -and $InstallQt) {
    if (Get-Command python -ErrorAction SilentlyContinue) {
        Log "Downloading Qt 6.8.3 (msvc2022_64) into .deps\Qt with aqtinstall ..."
        & python -m pip install --quiet --user aqtinstall 2>&1 | ForEach-Object { Log ([string]$_) }
        & python -m aqt install-qt windows desktop 6.8.3 win64_msvc2022_64 -O (Join-Path $repo ".deps\Qt") 2>&1 | ForEach-Object { Log ([string]$_) }
        $qt = Find-Qt
    } else {
        Log "python not found: cannot download Qt"
    }
}
if ($qt -ne "") {
    Log "Qt: $qt"
    $env:PATH = (Join-Path $qt "bin") + ";" + $env:PATH
} else {
    Log "Qt 6 not found: the desktop app is NOT built or tested in this run (the GPU checks do not need it)."
    Log "To include it, install Qt 6 (see docs\WINDOWS_SETUP.md) or re-run with -InstallQt."
}

if (-not $SkipBuild) {
    if (-not $VcpkgRoot -or -not (Test-Path (Join-Path $VcpkgRoot "scripts\buildsystems\vcpkg.cmake"))) {
        # Visual Studio 2022 ships a copy of vcpkg.
        $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
        if (Test-Path $vswhere) {
            $vs = & $vswhere -latest -property installationPath
            $candidate = Join-Path $vs "VC\vcpkg"
            if (Test-Path (Join-Path $candidate "scripts\buildsystems\vcpkg.cmake")) { $VcpkgRoot = $candidate }
        }
    }
    if (-not $VcpkgRoot) {
        Log "ERROR: vcpkg was not found. Set VCPKG_ROOT or pass -VcpkgRoot (see docs\WINDOWS_SETUP.md)."
        exit 2
    }
    Log "vcpkg: $VcpkgRoot"
    $toolchain = Join-Path $VcpkgRoot "scripts\buildsystems\vcpkg.cmake"
    $configure = @("-S", ".", "-B", $BuildDir, "-DCMAKE_TOOLCHAIN_FILE=$toolchain", "-DVCPKG_TARGET_TRIPLET=x64-windows")
    if ($qt -ne "") { $configure += "-DCMAKE_PREFIX_PATH=$qt" }
    $code = Step "Configure" "cmake" $configure
    if ($code -ne 0) { Log "Configure failed; stopping."; exit 1 }
    $code = Step "Build" "cmake" @("--build", $BuildDir, "--config", "Release", "--parallel")
    if ($code -ne 0) { Log "Build failed; stopping."; exit 1 }
}

$cli = Join-Path $repo "$BuildDir\Release\spectra-cli.exe"
if (-not (Test-Path $cli)) { $cli = Join-Path $repo "$BuildDir\spectra-cli.exe" }
if (-not (Test-Path $cli)) { Log "ERROR: spectra-cli.exe not found in $BuildDir"; exit 1 }

# Public RAW samples (CC0). Needs Python 3; skipped when it is missing.
Section "RAW samples"
$python = Get-Command python -ErrorAction SilentlyContinue
if ($NoDownload) {
    Log "download of the public RAW samples skipped (-NoDownload)"
} elseif ($python) {
    $output = & python scripts\fetch_test_raws.py 2>&1
    foreach ($line in $output) { Log ([string]$line) }
} else {
    Log "python not found: skipping the download of the public RAW samples"
}
$raws = @()
foreach ($dir in @("test_data\raw\canon_r6", "test_data\raw\pixls")) {
    if (Test-Path $dir) {
        $raws += Get-ChildItem -Path $dir -File | Where-Object { $_.Extension -notin @(".md", ".txt") } | ForEach-Object { $_.FullName }
    }
}
Log ("RAW files found: " + $raws.Count)

Step "GPU devices" $cli @("gpu-info") | Out-Null
Step "CPU tests (reference pipeline, parity against Python, I/O, Extended features, engine, RAW sets)" "ctest" @("--test-dir", $BuildDir, "-C", "Release", "-LE", "gpu|memory|app", "--output-on-failure") | Out-Null
Step "GPU tests (parity against Python on this GPU, tiling, memory budget)" "ctest" @("--test-dir", $BuildDir, "-C", "Release", "-L", "gpu", "--output-on-failure") | Out-Null

# Runtime calibration: what the app measures and chooses on first launch for this GPU.
Step "Performance calibration on this GPU" $cli @("calibrate", "--out", "reports\calibration_$stamp.json") | Out-Null

# The desktop app, unattended: start, calibrate, preview, adjust, export, LUT (GPU, then CPU fallback).
$app = Join-Path $repo "$BuildDir\Release\spectra-app.exe"
if (Test-Path $app) {
    Step "Desktop app self-test (GPU path and CPU fallback)" "ctest" @("--test-dir", $BuildDir, "-C", "Release", "-L", "app", "--output-on-failure") | Out-Null
} else {
    Section "Desktop app self-test"
    Log "skipped: spectra-app.exe was not built (Qt not found)"
}

# One line per parity suite with the worst error, for the record.
foreach ($suite in @("t2_nonspatial", "t3_spatial", "t5_sweeps", "t4_stochastic")) {
    Step "GPU parity detail: $suite" $cli @("parity", "tests\golden\$suite\cases.json", "--backend", "gpu") | Out-Null
}

# RAW decode check on every file (all brands): decode, render a preview on the GPU, save it.
New-Item -ItemType Directory -Force -Path "test_data\out" | Out-Null
foreach ($raw in $raws) {
    $name = [System.IO.Path]::GetFileNameWithoutExtension($raw)
    Step "RAW render: $name" $cli @("render", "--in", $raw, "--out", "test_data\out\$name.jpg", "--backend", "gpu", "--half", "1", "--bits", "8") | Out-Null
}

# The interactive path as the app drives it: first preview, then slider changes
# through the stage cache (targets: < 50 ms per slider change, < 1.5 s RAW open to first preview).
Step "Interactive benchmark: synthetic 20 MP" $cli @("bench-preview", "--mp", "20", "--repeats", "7") | Out-Null
foreach ($raw in ($raws | Select-Object -First 3)) {
    $name = [System.IO.Path]::GetFileName($raw)
    Step "Interactive benchmark: $name" $cli @("bench-preview", "--in", $raw, "--calibration", "reports\calibration_$stamp.json", "--repeats", "5") | Out-Null
}

# Batch export and LUT export.
$batchRaws = @($raws | Select-Object -First 3)
if ($batchRaws.Count -gt 0) {
    Step "Batch export (full resolution, JPEG)" $cli (@("batch", "--out-dir", "test_data\out\batch", "--format", "jpg") + $batchRaws) | Out-Null
}
Step "LUT export (33^3 .cube)" $cli @("lut", "--out", "test_data\out\portra_400.cube", "--size", "33") | Out-Null

# Benchmarks: 20 MP and 30 MP are the headline numbers.
$benchArgs = @("bench", "--backend", "gpu", "--mp", "20,30", "--repeats", "5")
$benchRaws = $raws | Select-Object -First 6
if ($benchRaws.Count -gt 0) { $benchArgs += @("--in", ($benchRaws -join ",")) }
Step "Benchmark: 20 MP and 30 MP on the GPU" $cli $benchArgs | Out-Null
if (-not $Quick) {
    Step "Benchmark: 60 MP (tiling) with the 10 GB memory limit" $cli @("bench", "--backend", "gpu", "--mp", "60", "--repeats", "2", "--limit-gb", "10") | Out-Null
    Step "Benchmark: 30 MP forced into tiles (1 GB budget)" $cli @("bench", "--backend", "gpu", "--mp", "30", "--repeats", "1", "--budget-mb", "1024") | Out-Null
}

if ($Package) {
    if (Get-Command makensis -ErrorAction SilentlyContinue) {
        Push-Location $BuildDir
        Step "Installer (cpack, NSIS + zip)" "cpack" @("-C", "Release", "-G", "NSIS;ZIP") | Out-Null
        Pop-Location
    } else {
        Section "Installer"
        Log "skipped: makensis (NSIS) is not on PATH. The CI build also produces the installer as an artifact."
    }
}

Section "Summary"
if ($failed.Count -eq 0) {
    Log "ALL STEPS PASSED"
} else {
    Log ("FAILED STEPS: " + ($failed -join "; "))
}
Log "Report: $report"
Write-Host ""
Write-Host "Paste the contents of $report back (or attach the file)."
if ($failed.Count -ne 0) { exit 1 }
exit 0
