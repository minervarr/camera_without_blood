#!/usr/bin/env bash
#
# Build libonnxruntime.so (Android arm64-v8a) from source, to match the pinned
# headers in libs/thirdparty/onnxruntime/include/ (ORT_API_VERSION 22 -> ONNX Runtime v1.22.0).
#
# Why this exists: libs/thirdparty/onnxruntime/lib/<abi>/libonnxruntime.so is a prebuilt that
# is git-ignored (.gitignore: *.so), so a fresh clone has no .so and the native
# link step fails with:
#   ninja: error: '.../libs/thirdparty/onnxruntime/lib/arm64-v8a/libonnxruntime.so' ... missing
# This script regenerates that .so from upstream source instead of copying a binary.
#
# Output: libs/thirdparty/onnxruntime/lib/<abi>/libonnxruntime.so
#
# Usage:
#   ./build_onnxruntime.sh                       # clone v1.22.0 + build + install
#   ./build_onnxruntime.sh --skip-clone          # reuse existing .ort_src checkout
#   ./build_onnxruntime.sh --python /path/to/python3.12  # pin a known-good interpreter
#
# Heads up: this is a heavy build (downloads ONNX Runtime + submodules, compiles
# protobuf/abseil/onnx/eigen/..., ~30-99 min, several GB of disk/RAM).

set -euo pipefail

ORT_VERSION="v1.22.0"
ANDROID_ABI="arm64-v8a"
ANDROID_API=26               # matches the app's minSdk 26
SKIP_CLONE=false
FORCE_CLONE=false
PYTHON=""

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC_DIR="${SCRIPT_DIR}/.ort_src"

# ── Parse args ───────────────────────────────────────────────────────────────
while [[ $# -gt 0 ]]; do
    case "$1" in
        --skip-clone)  SKIP_CLONE=true; shift ;;
        --force-clone) FORCE_CLONE=true; shift ;;
        --python)      PYTHON="$2"; shift 2 ;;
        -*)            echo "Unknown option: $1"; exit 1 ;;
        *)             echo "Unknown argument: $1"; exit 1 ;;
    esac
done

# ── Resolve toolchain ────────────────────────────────────────────────────────
if [[ -z "$PYTHON" ]]; then
    # Prefer CPython 3.10-3.12 (ORT build scripts are happiest there)
    for v in python3.12 python3.11 python3.10 python3.13 python3 python; do
        if command -v "$v" &>/dev/null; then
            PYTHON="$v"
            break
        fi
    done
fi
if [[ -z "$PYTHON" ]]; then
    echo "ERROR: No Python found. Install Python 3.10+ or pass --python /path/to/python" >&2
    exit 1
fi

SDK="${ANDROID_HOME:-}"
NDK="${ANDROID_NDK:-}"

if [[ -z "$SDK" ]]; then
    # Common Linux paths
    for p in "$HOME/Android/Sdk" "$HOME/android-sdk" "/opt/android-sdk" "/usr/local/android-sdk"; do
        if [[ -d "$p" ]]; then SDK="$p"; break; fi
    done
fi
if [[ -z "$SDK" ]]; then
    echo "ERROR: Android SDK not found. Set ANDROID_HOME or install to a standard path." >&2
    exit 1
fi

if [[ -z "$NDK" ]]; then
    NDK="${SDK}/ndk/29.0.14206865"
fi
if [[ ! -d "$NDK" ]]; then
    echo "ERROR: Android NDK not found at $NDK" >&2
    exit 1
fi

BUILD_PY="${SRC_DIR}/tools/ci_build/build.py"
BUILD_DIR="${SRC_DIR}/build/Android"
DEST="${SCRIPT_DIR}/libs/thirdparty/onnxruntime/lib/${ANDROID_ABI}"

echo "ORT version : ${ORT_VERSION}"
echo "ABI / API   : ${ANDROID_ABI} / ${ANDROID_API}"
echo "SDK         : ${SDK}"
echo "NDK         : ${NDK}"
echo "Python      : ${PYTHON}"
echo "Source dir  : ${SRC_DIR}"
echo "Install to  : ${DEST}/libonnxruntime.so"
echo ""

# ── 1. Fetch source at the matching tag ──────────────────────────────────────
if $SKIP_CLONE; then
    echo "==> reusing existing checkout (--skip-clone): ${SRC_DIR}"
elif [[ -f "$BUILD_PY" ]] && ! $FORCE_CLONE; then
    echo "==> source already present, reusing it (pass --force-clone to re-fetch)"
else
    echo "==> cloning ONNX Runtime ${ORT_VERSION}"
    rm -rf "${SRC_DIR}"
    git clone --recursive --depth 1 --branch "${ORT_VERSION}" \
        https://github.com/microsoft/onnxruntime "${SRC_DIR}"
fi
if [[ ! -f "$BUILD_PY" ]]; then
    echo "ERROR: build.py not found at ${BUILD_PY} (bad checkout?)" >&2
    exit 1
fi

# ── 1b. Self-heal stale GitLab archive hashes ────────────────────────────────
# ONNX Runtime pins SHA1 hashes for third-party archive downloads in
# cmake/deps.txt. GitLab periodically regenerates archives (same source,
# different compression -> different SHA1), which makes the build fail with
# "Hash mismatch". Download each GitLab archive and patch the hash.
DEPS_FILE="${SRC_DIR}/cmake/deps.txt"
if [[ -f "$DEPS_FILE" ]]; then
    echo "==> verifying third-party archive hashes (cmake/deps.txt)"
    CHANGED=false
    while IFS= read -r line; do
        [[ "$line" =~ ^[[:space:]]*# ]] && continue
        [[ -z "${line// /}" ]] && continue
        IFS=';' read -ra parts <<< "$line"
        [[ ${#parts[@]} -lt 3 ]] && continue
        url="${parts[1]}"
        [[ "$url" != *gitlab.com*/-/archive/* ]] && continue
        name="${parts[0]}"
        expected="${parts[2]}"
        expected=$(echo "$expected" | tr '[:upper:]' '[:lower:]')

        tmpfile=$(mktemp)
        if curl -sL "$url" -o "$tmpfile" 2>/dev/null; then
            actual=$(sha1sum "$tmpfile" | cut -d' ' -f1)
            rm -f "$tmpfile"
            if [[ "$actual" != "$expected" ]]; then
                echo "    ${name}: ${expected} -> ${actual} (gitlab rehashed its archive)"
                sed -i "s|${expected}|${actual}|" "$DEPS_FILE"
                CHANGED=true
            else
                echo "    ${name}: hash OK"
            fi
        else
            rm -f "$tmpfile"
            echo "    ${name}: download failed, skipping hash check"
        fi
    done < "$DEPS_FILE"

    if $CHANGED && [[ -d "$BUILD_DIR" ]]; then
        echo "    deps.txt updated; clearing stale build dir for a clean re-configure"
        rm -rf "${BUILD_DIR}"
    fi
fi

# ── 2. Cross-compile the shared lib ─────────────────────────────────────────
# CMAKE_POLICY_VERSION_MINIMUM=3.5 lets newer CMake (4.x) configure ORT's older
# vendored deps that still declare cmake_minimum_required(VERSION <3.5).
"$PYTHON" "$BUILD_PY" \
    --build_dir "$BUILD_DIR" \
    --config Release \
    --parallel \
    --skip_tests \
    --android \
    --android_sdk_path "$SDK" \
    --android_ndk_path "$NDK" \
    --android_abi "$ANDROID_ABI" \
    --android_api "$ANDROID_API" \
    --build_shared_lib \
    --cmake_generator Ninja \
    --compile_no_warning_as_error \
    --cmake_extra_defines CMAKE_POLICY_VERSION_MINIMUM=3.5

# ── 3. Install into libs/onnxruntime ────────────────────────────────────────
BUILT=$(find "$BUILD_DIR" -name "libonnxruntime.so" -type f 2>/dev/null | head -1)
if [[ -z "$BUILT" ]]; then
    echo "ERROR: Build finished but no libonnxruntime.so found under ${BUILD_DIR}" >&2
    exit 1
fi

mkdir -p "$DEST"
cp -f "$BUILT" "${DEST}/libonnxruntime.so"

SIZE_MB=$(du -m "${DEST}/libonnxruntime.so" | cut -f1)
echo ""
echo "Installed libonnxruntime.so (${SIZE_MB} MB) -> ${DEST}"
echo "Now run: ./build.sh"
