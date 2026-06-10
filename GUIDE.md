# Camera Without Blood — Project Guide

## Vision

A professional Android camera recorder that bypasses as much software abstraction as possible.
The goal is to capture video and audio at the highest quality the hardware allows, with no unnecessary processing layers, and mux them into a single MKV file.

The name reflects the philosophy: get close to the hardware without the mess that most camera apps add on top.

---

## Core Development Principle — Improve Libraries On The Fly

**This is the most important rule of this project.**

All shared libraries (`vulkan_canvas_engine`, `audio_engine`, `archive_engine`, etc.) are live, reusable codebases. Whenever this project exposes a gap, missing feature, or improvement opportunity in one of those libraries, **the fix goes into the library itself** — not into workaround code here.

This means:
- If the camera recorder needs a new capability from `audio_engine` (e.g. a better capture buffer, FLAC integration, new format support), implement it in `audio_engine` and commit it there.
- If the Vulkan canvas needs a new widget or layout primitive, add it to `vulkan_canvas_engine`.
- Every improvement made here automatically benefits every other project that uses those libraries.

The programmer working on this project is expected to contribute back to the library repos as part of the work, not just consume them.

---

## What this is NOT

- Not a social/consumer camera app
- Not built on CameraX, Jetpack, or any high-level Android framework
- No Java UI — the interface is rendered entirely via Vulkan

---

## Architecture Overview

```
android_main()
    └── App          (lifecycle: init/destroy Vulkan, main loop)
         ├── Renderer + Canvas    (Vulkan UI, from vulkan_canvas_engine)
         ├── UI                   (draws Record/Stop button, timer)
         └── Recorder             (orchestrates everything)
              ├── Camera          (Camera2 NDK — raw frames)
              ├── AudioCapture    (USB DAC via audio_engine — PCM → FLAC)
              └── Muxer           (libmatroska — writes .mkv file)
```

Everything is pure C++17. No Kotlin, no Java UI. The only Java involved is the JNI bridge in `audio_engine` and `archive_engine` (needed for Android USB host APIs).

---

## Hardware Access Philosophy

### Camera
Accessed via **Camera2 NDK** (`libcamera2ndk`). This is the lowest level available to an unprivileged Android app. The full real stack is:

```
Our code → CameraService (binder IPC) → Camera HAL → Kernel driver → ISP (silicon)
```

- We cannot bypass `CameraService` without root
- The ISP hardware always runs — even `RAW_SENSOR` format has passed through minimal ISP processing (black level, lens shading)
- `RAW_SENSOR` is preferred if the device supports it; fallback is `YUV_420_888`
- Frame format negotiation happens at `Camera::open()` by querying device characteristics

### Audio
Accessed via **`audio_engine`** (custom library) which talks directly to a USB audio device via `libusb`. This bypasses Android's AudioRecord entirely:

```
Our code → libusb → USB kernel driver → DAC hardware
```

- Captures PCM at the device's native sample rate/bit depth (up to 32-bit/384kHz)
- Output encoded as **FLAC** (lossless) before being written to the mux

### Video Encoding
Runtime codec negotiation (no hardcoded choice):
1. Try `video/hevc` hardware encoder → use if available (`V_MPEGH/ISO/HEVC` in MKV)
2. Fall back to `video/avc` hardware encoder → always available (`V_MPEG4/ISO/AVC` in MKV)
3. **Never** fall back to software encoding — it overheats the device and drops frames

---

## Output Format

**Container:** MKV (Matroska) via **libmatroska + libebml**

Reasons for MKV over MP4:
- MP4 does not natively support FLAC audio
- MKV handles FLAC natively and cleanly
- MKV can also carry RAW video streams if encoding is skipped in the future
- Open standard, well-documented EBML structure

**Tracks:**
| Track | Codec | Notes |
|-------|-------|-------|
| Video | HEVC or AVC | Hardware encoded, negotiated at runtime |
| Audio | FLAC | Lossless, from USB DAC capture |

---

## Submodule Libraries

All external code lives in `libs/`. These are **shared libraries** that exist independently and should not be modified unless the change benefits all projects that use them.

| Submodule | Purpose | Repo |
|-----------|---------|------|
| `vulkan_canvas_engine` | Vulkan-based 2D UI canvas + font rendering | github.com/minervarr/vulkan_canvas_engine |
| `vulkan_font_engine` | Nested inside canvas engine — GPU font via FreeType + MSDF | github.com/minervarr/vulkan_font_engine |
| `audio_engine` | Bitperfect USB DAC access — playback + capture, FLAC | github.com/minervarr/audio_engine |
| `archive_engine` | Android file I/O helper (compress/extract via Android APIs) | github.com/minervarr/archive_engine |
| `regen_atlas` | Font atlas rebuild tool — only needed if atlas changes | github.com/minervarr/regen_atlas |
| `libebml` | EBML binary format library (required by libmatroska) | github.com/Matroska-Org/libebml |
| `libmatroska` | MKV muxing library | github.com/Matroska-Org/libmatroska |
| `flac` | Lossless audio encoding — used directly in audio capture pipeline | github.com/xiph/flac |

---

## Build Requirements

- **Android NDK**: 29.0.14206865
- **CMake**: 3.22.1+
- **Vulkan SDK**: 1.4.341.1 — specifically `slangc.exe` at `C:/VulkanSDK/1.4.341.1/Bin/slangc.exe`
  - The Vulkan canvas shaders are written in Slang and compiled to SPIR-V automatically by CMake
  - If your SDK is installed elsewhere, update `SLANGC` in `app/src/main/cpp/CMakeLists.txt`
- **Min SDK**: 26 (Android 8.0)
- **Target ABI**: `arm64-v8a`

### First-time setup

```bash
git submodule update --init --recursive
./gradlew assembleDebug
```

---

## Project File Map

```
app/src/main/
├── AndroidManifest.xml         # NativeActivity, camera/audio/USB permissions
├── assets/shaders/             # Compiled SPIR-V shaders (generated by CMake)
└── cpp/
    ├── CMakeLists.txt          # Full build definition
    ├── main.cc                 # android_main() entry point
    ├── app.hh / app.cc         # Android lifecycle, owns all subsystems
    ├── camera/
    │   ├── camera.hh           # Camera public API
    │   └── camera.cc           # Camera2 NDK implementation
    ├── audio/
    │   ├── audio_capture.hh    # AudioCapture public API
    │   └── audio_capture.cc    # Wraps audio_engine USB capture
    ├── muxer/
    │   ├── muxer.hh            # Muxer public API
    │   └── muxer.cc            # libmatroska MKV writer ← STUB, needs implementation
    ├── recorder/
    │   ├── recorder.hh         # Recorder public API
    │   └── recorder.cc         # Ties camera + audio + muxer together
    └── ui/
        ├── ui.hh               # UI public API
        └── ui.cc               # Canvas-based Record/Stop interface
```

---

## Current State & What Remains

### Done
- [x] Android project skeleton (Gradle, Manifest, NativeActivity)
- [x] All submodules added and cloned recursively
- [x] CMakeLists wired to all submodule sources with correct real paths
- [x] `camera/` — Camera2 NDK capture pipeline with real format/resolution negotiation (RAW_SENSOR → YUV fallback, largest resolution, noise reduction disabled)
- [x] `audio/` — USB audio capture with inline FLAC encoding via libFLAC (frame-accurate, configurable compression level)
- [x] `recorder/` — Runtime codec negotiation (HEVC → AVC), orchestrates all subsystems
- [x] `ui/` — Vulkan canvas UI with Record/Stop button and duration timer
- [x] `muxer/` — Public API defined, libmatroska headers imported

### TODO (in priority order)

#### 1. Muxer implementation (`muxer/muxer.cc`) — **critical path**
The muxer stub must be implemented using libmatroska + libebml.
Steps:
- Implement a `libebml` `IOCallback` subclass that writes to a file via `archive_engine` or standard POSIX `fopen`
- Write the EBML head and Matroska segment
- Create video and audio `KaxTrackEntry` objects with the right codec IDs and private data
- Accumulate frames into `KaxCluster` blocks (new cluster every ~1s or on keyframe)
- On `close()`: flush last cluster, write `KaxCues` (seek table), finalize segment size

#### 2. Camera format negotiation (`camera/camera.cc`)
Currently `width_`, `height_`, and `format_` are hardcoded to 1920×1080 YUV.
Must be replaced with actual querying of `ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS`:
- Prefer `RAW_SENSOR` if available at the target resolution
- Otherwise use `YUV_420_888`
- Pick the highest resolution the device supports at ≥30fps

#### 3. Video encoding pipeline (`recorder/recorder.cc`)
The camera frame callback currently receives `AImage*` but does nothing with it.
Must implement:
- Configure `AMediaCodec` encoder (HEVC or AVC per negotiation result)
- Feed `AImage` planes into the encoder input surface or byte buffer
- Drain encoded output and pass to `muxer_->write_video()`
- Extract SPS/PPS (AVC) or VPS/SPS/PPS (HEVC) from encoder output for `VideoTrackConfig.private_data`

#### 4. ~~FLAC encoding pipeline~~ — DONE
libFLAC is integrated directly in `audio/audio_capture.cc`. The capture loop accumulates raw PCM frames, de-interleaves into per-channel buffers, and feeds `FLAC__stream_encoder_process()`. Encoded frames are delivered via `FlacFrameCallback` with a nanosecond timestamp.

#### 5. Touch input → UI actions (`ui/ui.cc`)
The Record and Stop buttons are drawn but not wired to touch events.
Must implement:
- Add `input_handler` from `vulkan_canvas_engine` to the build
- Route `AInputEvent` from the main loop to `InputHandler`
- On tap of Record button → `recorder_.start(output_path)`
- On tap of Stop button → `recorder_.stop()`
- Output path: use `archive_engine` or `AStorageManager` to resolve a writable path

#### 6. Output file path
Where to save the `.mkv` file needs to be decided:
- Android 10+: `MediaStore` API (requires Java side) or app-specific external storage
- Simpler: write to `getExternalFilesDir()` — no extra permissions on Android 10+
- Use `archive_engine` utilities to resolve the path from the native side

#### 7. Permissions at runtime
Camera and audio permissions must be requested at runtime (not just declared in manifest).
Since this is a `NativeActivity` (no Java), this requires a JNI call to `ActivityCompat.requestPermissions`.
One approach: add a minimal Java `Activity` wrapper just for permission handling, then hand off to `NativeActivity`. Or use the NDK permission API if available on minSdk 26.

---

## Key Design Decisions to Preserve

- **No Java UI** — all rendering via Vulkan canvas
- **No AudioRecord** — USB DAC only via `audio_engine`
- **No CameraX / Jetpack** — Camera2 NDK only
- **HEVC preferred, AVC fallback** — always hardware, never software encode
- **FLAC audio, MKV container** — lossless audio, flexible container
- **Improve libraries on the fly** — see "Core Development Principle" above; fixes and features go into the lib repos so all projects benefit
