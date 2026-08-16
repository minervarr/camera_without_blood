#!/usr/bin/env bash
#
# One-shot build for a fresh/clean clone of this repo on Linux.
# Does the whole thing end to end:
#   1. build libonnxruntime.so from source        (the git-ignored .so that a clean
#                                                   clone lacks -> the "missing and no
#                                                   known rule to make it" link error)
#   2. ./gradlew assembleDebug                     (or assembleRelease with --release)
#
# Submodules are expected to already be initialized (--recursive clone).
# If not, run: git submodule update --init --recursive
#
# Direct use:  ./build.sh [--release] [--skip-onnx] [--python <exe>]

set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"

# ── Parse args ───────────────────────────────────────────────────────────────
RELEASE=false
SKIP_ONNX=false
PYTHON=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --release)    RELEASE=true; shift ;;
        --skip-onnx)  SKIP_ONNX=true; shift ;;
        --python)     PYTHON="$2"; shift 2 ;;
        -*)           echo "Unknown option: $1"; exit 1 ;;
        *)            echo "Unknown argument: $1"; exit 1 ;;
    esac
done

# ── Ensure Gradle wrapper exists ─────────────────────────────────────────────
# gradlew is git-ignored; generate it once with `gradle wrapper`.
if [[ ! -x "$ROOT/gradlew" ]]; then
    if command -v gradle &>/dev/null; then
        echo "==> generating Gradle wrapper (gradlew)"
        (cd "$ROOT" && gradle wrapper)
        chmod +x "$ROOT/gradlew"
    else
        echo "ERROR: No gradlew (it is git-ignored) and no 'gradle' on PATH." >&2
        echo "Run 'gradle wrapper' once, then re-run this script." >&2
        exit 1
    fi
fi

# ── 1. ONNX Runtime ─────────────────────────────────────────────────────────
SO="$ROOT/libs/thirdparty/onnxruntime/lib/arm64-v8a/libonnxruntime.so"
if $SKIP_ONNX; then
    echo "==> [1/2] ONNX Runtime build skipped (--skip-onnx)"
elif [[ -f "$SO" ]]; then
    echo "==> [1/2] ONNX Runtime .so already present, skipping"
else
    echo "==> [1/2] building ONNX Runtime from source"
    ONNX_ARGS=()
    if [[ -n "$PYTHON" ]]; then
        ONNX_ARGS+=(--python "$PYTHON")
    fi
    bash "$ROOT/build_onnxruntime.sh" "${ONNX_ARGS[@]}"
fi

# ── 2. gradle ────────────────────────────────────────────────────────────────
# The installable app is now the thin :demo module (it depends on the :camera
# library, which builds libcamera_recorder.so + packages the assets/.aar).
if $RELEASE; then
    TASK=":demo:assembleRelease"
else
    TASK=":demo:assembleDebug"
fi
echo "==> [2/2] gradle ${TASK}"
"$ROOT/gradlew" "$TASK"

# ── done ─────────────────────────────────────────────────────────────────────
echo ""
echo "BUILD COMPLETE."
APK_DIR="$ROOT/demo/build/outputs/apk"
APK=$(find "$APK_DIR" -name "*.apk" -type f 2>/dev/null | head -1)
if [[ -n "$APK" ]]; then
    echo "APK: $APK"
fi
