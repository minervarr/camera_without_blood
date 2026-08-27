# AGENTS.md

## Reading order

- **`CLAUDE.md` is the canonical deep reference** (architecture, pipeline internals, per-gotcha history). Read it before any non-trivial change.
- `GUIDE.md` is **stale** (predates `jni/`, `isp/`, DNG-only stills). When docs and code disagree, trust the code.

## What this is

Pro Android camera recorder: C++17 + Vulkan (UI render + compute ISP), no CameraX/Jetpack. Three Gradle modules:

- **`:camera`** (`io.nava.camera`) — everything meaningful: native code in `camera/src/main/cpp/`, Java (`HdrCameraSession`, `RecordingService`, `CameraActivity`, `CameraLauncher`), assets. Builds `libcamera_recorder.so` and **the `.aar`** other apps embed. See `camera/README.md`.
- **`:engine-audio`** — audio_engine's *Java* as a standalone coordinate (for host-app dedup); `:camera` `api`-depends on it. Its *native* audio compiles into `libcamera_recorder.so`.
- **`:demo`** (`io.nava.camera.demo`, targetSdk 28) — thin launcher; the installable build for on-device verification.

## Build (Linux: `./gradlew ...`)

```bash
git submodule update --init --recursive   # REQUIRED first: CMake builds most libs from source
./gradlew :demo:assembleDebug             # builds native + .aar + APK
./gradlew :demo:installDebug              # + adb install
./gradlew :camera:assembleRelease         # the embeddable .aar
```

- **Single ABI: `arm64-v8a`.** Any host embedding the `.aar` must ship arm64-v8a too.
- Toolchain pins: NDK `29.0.14206865`, CMake ≥ 3.22.1, compileSdk 37, minSdk 26.
- Shaders compile from Slang via `slangc` at CMake time (`$VULKAN_SDK`, common paths, or `-DVCE_SLANGC=...`). Output `.spv` files land in `camera/src/main/assets/shaders/` and are **gitignored**. A clean clone without slangc ships an APK with no shaders — canvas fails loudly, but the ISP ones fail *silently* (`RawVideoPipeline::init()` returns false, RAW path vanishes).
- No lint/typecheck beyond compilation; the build IS the check. There are **no automated tests** — verification is on-device only.

## Verifying changes on-device

- Logcat tags: `RawVideo`, `Recorder`, `HdrCamera`, `DfNet`, `AudioCapture`; native logs also mirror to `<externalDataPath>/app.log`.
- RAW_PQ clip sanity via `ffprobe`: expect HEVC Main 10, `yuv420p10le`, `bt2020nc/smpte2084/pc`.
- **Stale-APK trap:** a native build failure leaves the previous install runnable — you will debug dead code. Before on-device debugging, confirm your newest log string appears (`adb logcat -d -s Tag:*`).
- Verification devices: Galaxy S23 Ultra (exercises RAW_PQ) and moto g06 (no Camera2 RAW → native YUV path / legacy fallback).

## Architecture in five lines

- Capture session is **hybrid**: native NDK Camera2 (`cpp/camera/ndk_session.*`, preferred) with a Java `HdrCameraSession` fallback — the Java layer exists mainly because dynamic-range profiles have no NDK equivalent.
- Two/three video paths picked at runtime: `RAW_PQ` (Vulkan compute ISP → HEVC Main10), `YUV_NATIVE` (zero-copy input-surface encoder), `LEGACY_HLG` (Java fallback, often plain SDR on weak devices).
- Stills: RAW devices write **DNG only** (3-shot bracket + merge); non-RAW write lossless PNG. Nothing viewable is written next to a DNG — developing belongs to the post app.
- After REC stops, the recorder enters **FINALIZING** (background drain + audio denoise); UI shows "Processing N%"; no new capture until done.
- The ISP runs on its **own Vulkan device** (`isp/vk_compute.hh`) separate from rendering.

## Hard-won rules

- **Do not reintroduce removed things** (all deliberately deleted, some after device testing): libjxl / JXL stills, ncnn (AiDenoiser), rnnoise, temporal denoise pass, 50 MP "max resolution" mode, auto black-point subtraction, DfNet output muxed into the `.mkv` (denoised audio stays a `_ai.flac` side-car).
- Camera-output paths must stay **host-relative** (`getExternalFilesDir` via `CameraActivity.cameraOutputDir()`) — never hardcode public storage.
- Fixes to `libs/firstparty/*` submodules belong **in that submodule's repo**, not as local workarounds. Exception: `archive_engine`'s native API is incompatible — native usage is the local copy `cpp/archive.cc`; don't rewire the submodule.
- Android's `libvulkan.so` exports only Vulkan 1.0 symbols: resolve anything newer (`vkCmdDispatchBase`, feature structs) through `vkGetInstanceProcAddr`/`vkGetDeviceProcAddr`.
- Always fully release/shutdown the camera session on preview stop, not just stopRepeating — otherwise the HAL wedges device-wide ("connectHelper: Could not initialize client") until reboot, defeating the Java fallback too.
- `:demo` release signing uses a keystore + checked-in password (legacy artifact). Never copy credentials into new files, logs, or commits.

## Hygiene

Don't commit without being asked; keep `camera/.cxx/` churn out of commits. `tools/*.py` are standalone dev helpers outside the Gradle build (`df_validate.py` needs the `.venv_313` venv).
