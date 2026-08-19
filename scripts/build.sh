#!/usr/bin/env bash
#
# build.sh — build the camera library.
#
# Usage:
#   ./build.sh                  # build camera .aar (debug)
#   ./build.sh --release        # build camera .aar (release)
#   ./build.sh --demo           # build + install demo APK (reference UI)
#   ./build.sh --skip-onnx      # skip ONNX Runtime build
#
# The demo/ folder is reference-only (old UI example). --demo temporarily
# enables it in settings.gradle, builds the APK, and reverts.
#
# Prerequisites:
#   - Android SDK + NDK installed
#   - gradle wrapper (generated automatically if missing)
#   - Submodules initialized: git submodule update --init --recursive
#

set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

# ── Parse args ───────────────────────────────────────────────────────────────
RELEASE=false
BUILD_DEMO=false
SKIP_ONNX=false
PYTHON=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --release)     RELEASE=true; shift ;;
        --demo)        BUILD_DEMO=true; shift ;;
        --skip-onnx)   SKIP_ONNX=true; shift ;;
        --python)      PYTHON="$2"; shift 2 ;;
        -*)            echo "Unknown option: $1"; exit 1 ;;
        *)             echo "Unknown argument: $1"; exit 1 ;;
    esac
done

# ── Ensure gradlew exists ────────────────────────────────────────────────────
if [[ ! -x "$ROOT/gradlew" ]]; then
    if command -v gradle &>/dev/null; then
        echo "==> generating Gradle wrapper"
        (cd "$ROOT" && gradle wrapper)
        chmod +x "$ROOT/gradlew"
    else
        echo "ERROR: No gradlew and no 'gradle' on PATH." >&2
        echo "Run 'gradle wrapper' once, then re-run this script." >&2
        exit 1
    fi
fi

# ── 1. ONNX Runtime ─────────────────────────────────────────────────────────
SO="$ROOT/libs/thirdparty/onnxruntime/lib/arm64-v8a/libonnxruntime.so"
if $SKIP_ONNX; then
    echo "==> [1/2] ONNX Runtime skipped (--skip-onnx)"
elif [[ -f "$SO" ]]; then
    echo "==> [1/2] ONNX Runtime .so present, skipping"
else
    echo "==> [1/2] building ONNX Runtime from source"
    ONNX_ARGS=()
    if [[ -n "$PYTHON" ]]; then
        ONNX_ARGS+=(--python "$PYTHON")
    fi
    bash "$ROOT/scripts/build_onnxruntime.sh" "${ONNX_ARGS[@]}"
fi

# ── 2. Gradle ───────────────────────────────────────────────────────────────
# Enable demo module temporarily if --demo was passed
if $BUILD_DEMO; then
    if grep -q "// DEMO placeholder" "$ROOT/settings.gradle"; then
        echo "==> enabling demo module in settings.gradle"
        sed -i "s|// DEMO placeholder.*|include ':demo'          // thin reference app (old UI, not compiled by default)|" "$ROOT/settings.gradle"
        REVERT_DEMO=true
    fi
fi

if $BUILD_DEMO; then
    if $RELEASE; then
        TASK=":demo:assembleRelease"
    else
        TASK=":demo:assembleDebug"
    fi
elif $RELEASE; then
    TASK=":camera:assembleRelease"
else
    TASK=":camera:assembleDebug"
fi
echo "==> [2/2] gradle ${TASK}"
"$ROOT/gradlew" "$TASK"

# Revert demo inclusion
if ${REVERT_DEMO:-false}; then
    echo "==> disabling demo module in settings.gradle"
    sed -i "s|include ':demo'.*|// DEMO placeholder — build.sh swaps this for \"include ':demo'\" then reverts|" "$ROOT/settings.gradle"
fi

# ── Done ─────────────────────────────────────────────────────────────────────
echo ""
echo "BUILD COMPLETE."
if [[ "$TASK" == *":camera:assemble"* ]]; then
    AAR=$(find "$ROOT/camera/build/outputs/aar" -name "*.aar" -type f 2>/dev/null | head -1)
    if [[ -n "$AAR" ]]; then
        echo "AAR: $AAR"
    fi
else
    APK=$(find "$ROOT/demo/build/outputs/apk" -name "*.apk" -type f 2>/dev/null | head -1)
    if [[ -n "$APK" ]]; then
        echo "APK: $APK"
    fi
fi
