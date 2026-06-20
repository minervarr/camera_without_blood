#!/usr/bin/env pwsh
#
# Build libonnxruntime.so (Android arm64-v8a) from source, to match the pinned
# headers in libs/onnxruntime/include/ (ORT_API_VERSION 22 -> ONNX Runtime v1.22.0).
#
# Why this exists: libs/onnxruntime/lib/<abi>/libonnxruntime.so is a prebuilt that
# is git-ignored (.gitignore: *.so), so a fresh clone has no .so and the native
# link step fails with:
#   ninja: error: '.../libs/onnxruntime/lib/arm64-v8a/libonnxruntime.so' ... missing
# This script regenerates that .so from upstream source instead of copying a binary.
#
# Output: libs/onnxruntime/lib/<abi>/libonnxruntime.so
#
# Usage (from either clone of the repo):
#   ./build_onnxruntime.ps1                       # clone v1.22.0 + build + install
#   ./build_onnxruntime.ps1 -SkipClone            # reuse existing ./.ort_src checkout
#   ./build_onnxruntime.ps1 -Python C:\py312\python.exe   # pin a known-good interpreter
#
# Heads up: this is a heavy build (downloads ONNX Runtime + submodules, compiles
# protobuf/abseil/onnx/eigen/..., ~30-90 min, several GB of disk/RAM).

[CmdletBinding()]
param(
  [string]$OrtVersion  = "v1.22.0",
  [string]$AndroidAbi  = "arm64-v8a",
  [int]   $AndroidApi  = 26,                       # matches the app's minSdk 26
  [string]$Sdk         = $env:ANDROID_HOME,
  [string]$Ndk         = "",
  [string]$Python      = "python",
  [string]$SrcDir      = (Join-Path $PSScriptRoot ".ort_src"),
  [switch]$SkipClone
)

$ErrorActionPreference = "Stop"

# --- resolve toolchain -------------------------------------------------------
if (-not $Sdk) { $Sdk = "C:/PPProgam/android_sdk" }
if (-not $Ndk) { $Ndk = Join-Path $Sdk "ndk/29.0.14206865" }

if (-not (Test-Path $Sdk)) { throw "Android SDK not found: $Sdk (set -Sdk or `$env:ANDROID_HOME)" }
if (-not (Test-Path $Ndk)) { throw "Android NDK not found: $Ndk (set -Ndk)" }

$buildPy = Join-Path $SrcDir "tools/ci_build/build.py"
$buildDir = Join-Path $SrcDir "build/Android"
$dest = Join-Path $PSScriptRoot "libs/onnxruntime/lib/$AndroidAbi"

Write-Host "ORT version : $OrtVersion"
Write-Host "ABI / API   : $AndroidAbi / $AndroidApi"
Write-Host "SDK         : $Sdk"
Write-Host "NDK         : $Ndk"
Write-Host "Python      : $Python"
Write-Host "Source dir  : $SrcDir"
Write-Host "Install to  : $dest/libonnxruntime.so"
Write-Host ""

# --- 1. fetch source at the matching tag ------------------------------------
if (-not $SkipClone) {
  if (Test-Path $SrcDir) { Remove-Item -Recurse -Force $SrcDir }
  git clone --recursive --depth 1 --branch $OrtVersion `
    https://github.com/microsoft/onnxruntime $SrcDir
  if ($LASTEXITCODE -ne 0) { throw "git clone failed" }
}
if (-not (Test-Path $buildPy)) { throw "build.py not found at $buildPy (bad checkout?)" }

# --- 2. cross-compile the shared lib ----------------------------------------
# CMAKE_POLICY_VERSION_MINIMUM=3.5 lets newer CMake (4.x) configure ORT's older
# vendored deps that still declare cmake_minimum_required(VERSION <3.5).
& $Python $buildPy `
  --build_dir $buildDir `
  --config Release `
  --parallel `
  --skip_tests `
  --android `
  --android_sdk_path $Sdk `
  --android_ndk_path $Ndk `
  --android_abi $AndroidAbi `
  --android_api $AndroidApi `
  --build_shared_lib `
  --cmake_generator Ninja `
  --compile_no_warning_as_error `
  --cmake_extra_defines CMAKE_POLICY_VERSION_MINIMUM=3.5
if ($LASTEXITCODE -ne 0) { throw "ONNX Runtime build failed (see output above)" }

# --- 3. install into libs/onnxruntime ---------------------------------------
$built = Get-ChildItem -Path $buildDir -Recurse -Filter "libonnxruntime.so" -ErrorAction SilentlyContinue |
         Select-Object -First 1
if (-not $built) { throw "Build finished but no libonnxruntime.so found under $buildDir" }

New-Item -ItemType Directory -Force $dest | Out-Null
Copy-Item -Force $built.FullName (Join-Path $dest "libonnxruntime.so")

$sizeMb = "{0:N1}" -f ((Get-Item (Join-Path $dest "libonnxruntime.so")).Length / 1MB)
Write-Host ""
Write-Host "Installed libonnxruntime.so ($sizeMb MB) -> $dest"
Write-Host "Now run: ./gradlew assembleDebug"
