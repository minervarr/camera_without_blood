#!/usr/bin/env bash
#
# build.sh — build the camera library as part of this project.
#
# Usage:
#   ./build.sh                  # debug build
#   ./build.sh --release        # release build
#   ./build.sh --install        # debug build + install on device
#   ./build.sh --skip-onnx      # skip ONNX Runtime build
#
# Prerequisites:
#   - Android SDK + NDK installed (ANDROID_HOME / ANDROID_NDK_HOME)
#   - gradle wrapper (generated automatically if missing)
#   - Submodules initialized: git submodule update --init --recursive
#

set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"

# ── Parse args ───────────────────────────────────────────────────────────────
RELEASE=false
INSTALL=false
SKIP_ONNX=false
PYTHON=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --release)     RELEASE=true; shift ;;
        --install)     INSTALL=true; shift ;;
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
    bash "$ROOT/build_onnxruntime.sh" "${ONNX_ARGS[@]}"
fi

# ── 2. Gradle ───────────────────────────────────────────────────────────────
if $RELEASE; then
    TASK=":camera:assembleRelease"
elif $INSTALL; then
    TASK=":demo:installDebug"
else
    TASK=":demo:assembleDebug"
fi
echo "==> [2/2] gradle ${TASK}"
"$ROOT/gradlew" "$TASK"

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
