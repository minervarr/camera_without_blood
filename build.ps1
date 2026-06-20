#!/usr/bin/env pwsh
#
# One-shot build for a fresh/clean clone of this repo.
# Does the whole thing end to end:
#   1. git submodule update --init --recursive   (CMake builds most libs from source)
#   2. build libonnxruntime.so from source        (the git-ignored .so that a clean
#                                                   clone lacks -> the "missing and no
#                                                   known rule to make it" link error)
#   3. ./gradlew assembleDebug                     (or assembleRelease with -Release)
#
# Entry point: build.bat (double-click / run from cmd) -> this script.
# Direct use:  ./build.ps1 [-Release] [-SkipSubmodules] [-SkipOnnx] [-Python <exe>]

[CmdletBinding()]
param(
  [switch]$Release,
  [switch]$SkipSubmodules,
  [switch]$SkipOnnx,
  [string]$Python = ""        # empty = auto-detect a build-friendly CPython
)

$ErrorActionPreference = "Stop"
$root = $PSScriptRoot

# ONNX Runtime's build scripts are happiest on CPython 3.10-3.12; this host's
# default may be newer (e.g. 3.14). Prefer a known-good interpreter via the
# Windows 'py' launcher, else fall back to whatever 'python' is.
function Find-BuildPython {
  param([string]$Explicit)
  if ($Explicit) { return $Explicit }
  foreach ($v in '3.12','3.11','3.10','3.13') {
    try {
      $exe = & py -$v -c "import sys; print(sys.executable)" 2>$null
      if ($LASTEXITCODE -eq 0 -and $exe) { return $exe.Trim() }
    } catch {}
  }
  return "python"
}

# --- 1. submodules -----------------------------------------------------------
if (-not $SkipSubmodules) {
  Write-Host "==> [1/3] git submodule update --init --recursive" -ForegroundColor Cyan
  git -C $root submodule update --init --recursive
  if ($LASTEXITCODE -ne 0) { throw "submodule update failed" }
} else {
  Write-Host "==> [1/3] submodules skipped (-SkipSubmodules)" -ForegroundColor DarkGray
}

# --- 2. ONNX Runtime .so -----------------------------------------------------
$so = Join-Path $root "libs/onnxruntime/lib/arm64-v8a/libonnxruntime.so"
if ($SkipOnnx) {
  Write-Host "==> [2/3] ONNX Runtime build skipped (-SkipOnnx)" -ForegroundColor DarkGray
} elseif (Test-Path $so) {
  Write-Host "==> [2/3] ONNX Runtime .so already present, skipping" -ForegroundColor DarkGray
} else {
  $py = Find-BuildPython $Python
  Write-Host "==> [2/3] building ONNX Runtime from source (python: $py)" -ForegroundColor Cyan
  & (Join-Path $root "build_onnxruntime.ps1") -Python $py
  if ($LASTEXITCODE -ne 0) { throw "ONNX Runtime build failed" }
}

# --- 3. gradle ---------------------------------------------------------------
$task = if ($Release) { "assembleRelease" } else { "assembleDebug" }
Write-Host "==> [3/3] gradle $task" -ForegroundColor Cyan
$gradlew = Join-Path $root "gradlew.bat"
if (Test-Path $gradlew) {
  & $gradlew $task
} elseif (Get-Command gradle -ErrorAction SilentlyContinue) {
  & gradle -p $root $task
} else {
  throw "No gradlew.bat (it is git-ignored) and no 'gradle' on PATH. Run 'gradle wrapper' once, then re-run."
}
if ($LASTEXITCODE -ne 0) { throw "gradle $task failed" }

# --- done --------------------------------------------------------------------
$apkDir = if ($Release) { "app/build/outputs/apk/release" } else { "app/build/outputs/apk/debug" }
Write-Host ""
Write-Host "BUILD COMPLETE." -ForegroundColor Green
$apk = Get-ChildItem -Path (Join-Path $root $apkDir) -Filter *.apk -ErrorAction SilentlyContinue | Select-Object -First 1
if ($apk) { Write-Host "APK: $($apk.FullName)" }
