# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A pro Android camera recorder that bypasses high-level Android frameworks (no CameraX/Jetpack, no Java UI) to capture the highest-quality video/audio the hardware allows and mux it into an `.mkv`. The UI is rendered entirely with Vulkan; almost all logic is C++17. The app is evolving toward a **headless background recorder** — a foreground service + wake lock keep it capturing while backgrounded / screen-off (see Architecture). `GUIDE.md` has the original vision and design rationale, but it is partly **stale** — it predates the `jni/`, `isp/`, and `camera/dng_*` code and still claims "no Java," which is no longer true (see Architecture). Trust the code over `GUIDE.md` when they disagree.

## Module layout (the camera is a reusable library)

The repo is a 3-module Gradle build so the whole camera can be **embedded in
another app** as a full-screen scene (see "Embedding" below):

- **`:camera`** — `com.android.library`, namespace `io.nava.camera`. All the
  native code (`camera/src/main/cpp`, builds `libcamera_recorder.so`), the camera
  Java (`HdrCameraSession`, `RecordingService`, `CameraActivity`, `CameraLauncher`),
  and assets (shaders/font/models). **This produces the `.aar`.**
- **`:engine-audio`** — `com.android.library`, namespace `com.nerio.audioengine`.
  Wraps `audio_engine`'s **Java** as a standalone Maven coordinate so a host app
  that also uses audio_engine dedups it (the audio *native* still compiles into
  `libcamera_recorder.so`). `:camera` `api`-depends on it.
- **`:demo`** — `com.android.application` (`io.nava.camera.demo`). Thin launcher
  that opens the camera scene and logs the returned files; the installable build
  for on-device verification.

Source paths below written as `cpp/...` live under `camera/src/main/cpp/...`.

## Build & run

```bash
git submodule update --init --recursive   # required: CMake builds most libs from source
./gradlew :demo:assembleDebug              # debug APK (builds the :camera native lib + .aar)
./gradlew :demo:installDebug               # build + adb install
./gradlew :camera:assembleRelease          # the camera .aar (camera/build/outputs/aar/)
```

- **Single ABI only:** `arm64-v8a` (`abiFilters` in `camera/build.gradle`); a host
  app embedding the `.aar` must also ship `arm64-v8a`.
- **`slangc.exe` path is hardcoded** in `camera/src/main/cpp/CMakeLists.txt` (`SLANGC`, ~line 100) to `C:/VulkanSDK/1.4.341.1/Bin/slangc.exe`. If the Vulkan SDK is elsewhere, edit that line or the build fails at shader compilation. Slang shaders in `libs/.../vulkan_font_engine/.../shaders_src/*.slang` compile to SPIR-V in `camera/src/main/assets/shaders/`.
- Toolchain: NDK `29.0.14206865`, CMake `3.22.1+`, `compileSdk 37`, `minSdk 26`. The `:camera` library has **no `targetSdk`** (the host governs it at runtime); the `:demo` app pins `targetSdk 28`. Captures no longer depend on legacy external storage — `CameraActivity.cameraOutputDir()` writes to the host's `getExternalFilesDir()` (scoped-storage-safe), so a modern host (targetSdk 33+) works. The old public-`Documents` path (`archive::get_documents_path`) survives only as a fallback for a non-`CameraActivity` host.
- `build.bat` (→ `build.ps1`) is a one-click **clean-clone** build: submodules + the ONNX Runtime `.so` **from source** (`build_onnxruntime.ps1`, which self-heals GitLab dep-hash drift in `cmake/deps.txt`) + the APK. For normal iteration use `gradlew`; `build.bat` is for a fresh checkout or to (re)build ORT into `libs/onnxruntime/lib/<abi>/`.
- No automated tests exist. Verification is on-device only (see below).

### Screenshots / device debugging

- `screenshot.bat` — grabs a screencap from the first attached adb device and pulls it locally.
- Native logs go to logcat **and** to `<externalDataPath>/app.log` (see `main.cc` / `logger.cc`). Key logcat tags: `RawVideo`, `Recorder`, `HdrCamera`, `DfNet`, `AudioCapture`.
- The RAW pipeline emits a per-window profile line (`RawVideo: .. fps | gpu A/MAXms enc-wait B/MAXms cam-gap MAXms ..`) for locating frame-rate stalls.
- Verify recorded output with `ffprobe` (expect HEVC Main 10, `yuv420p10le`, `bt2020nc/smpte2084/pc` for the RAW PQ path) and play it back on-device.
- **Gotcha that wasted a lot of time once:** when on-device behavior contradicts the source, first confirm the build actually **compiled and installed**. The native build can fail (a C++ error) while a *prior* APK stays installed, so you end up testing stale code. Verify by grepping logcat for a log string you know is new (`adb logcat -d -s DfNet:* AudioCapture:*`) before debugging the algorithm. From Git Bash, `adb pull`/`shell` of `/storage/...` paths needs `MSYS_NO_PATHCONV=1`.
- **Verification devices in use:** a **Galaxy S23 Ultra** (exercises the RAW_PQ path) and a **Motorola moto g06** (no Camera2 RAW → legacy HLG video + the YUV→PNG still path; HEVC encoder caps its video ~1440×1088). Wireless-debug ports change on every reconnect — re-`adb connect <ip:port>` and expect to be handed a new port.
- **Host-side `.mkv`/audio diagnostics:** `ffmpeg -i clip -af volumedetect -f null -` for audio level (a silent USB-DAC clip is `~-73 dB`); `adb exec-out "dd if=<path> bs=1M count=N"` grabs just a clip's *header* without pulling the whole multi-GB file; `Format-Hex` of the first 64 bytes confirms the EBML `DocType` (see the muxer gotcha under Muxing).

## Architecture

### Embedding (host integration) — the camera as a full-screen scene

The whole camera is launchable from another app via `io.nava.camera.CameraActivity`
(a `NativeActivity` subclass) — see `camera/README.md`. A host uses
`CameraLauncher.createIntent()` + the Activity Result API; the scene owns the
window, the user shoots PHOTO/VIDEO, presses **Back**, and the host gets the
captured file paths. Wiring: the **Back key is handled in native** (`app.cc`
`handle_input` → `exit_requested_`); `App::maybe_finish_session()` waits out the
async **FINALIZING** phase, then JNI-up-calls `CameraActivity.finishWithResults(String[])`
(`jni::host_finish_with_results`). Exact output paths are recorded as each file is
written (`jni::session_record_file` in `nativeOn{Raw,Still}Frame` + the `.mkv` in
`app.cc`). Output dir + initial mode come *down* from the Activity
(`cameraOutputDir()`/`cameraStartVideo()`). Don't reintroduce a hardcoded output
path — it must stay host-relative (`getExternalFilesDir`).

### Hybrid native/Java split — the key thing to understand

Although the app is a `NativeActivity` driven by C++, the **Camera2 capture session lives in Java** (`app/src/main/java/io/nava/camera/HdrCameraSession.java`). This is unavoidable: 10-bit HDR (`OutputConfiguration.setDynamicRangeProfile`) and the HLG10 encoder path have **no NDK equivalent**. The native side controls the session and receives frames through the JNI bridge in `cpp/jni/jni_camera.{cc,hh}`:

- Preview frames come back to native as `AHardwareBuffer`s (zero-copy into the Vulkan renderer), each with a `release` callback that must be called exactly once.
- While recording in legacy mode, Java drives the HEVC encoder and forwards encoded packets back to the native muxer.
- **Background recording:** the second Java class, `RecordingService.java`, is a `startForeground()` / `START_STICKY` service (`foregroundServiceType="camera|microphone"`) that also holds a `PARTIAL_WAKE_LOCK` ("camera:record"). `HdrCameraSession.startCameraService()/stopCameraService()` bring it up/down as the preview starts/stops, so Android keeps the camera grant and the CPU keeps developing/encoding with the screen off. The `FOREGROUND_SERVICE*` + `WAKE_LOCK` permissions in `AndroidManifest.xml` exist for this.

The native `cpp/camera/` code (`camera.cc`, `dng_writer.cc`, `dng_meta_source.cc`) supplies the static DNG metadata reused by the RAW video ISP and contains a native RAW/DNG still path — but the **live shutter is Java** (`HdrCameraSession.takePhoto`), not `cam::Camera::take_photo`. See **Still photos** below.

### Lifecycle / ownership

`android_main()` (`main.cc`) → `App` (`app.{cc,hh}`) owns the Vulkan `Renderer` + `Canvas`, the `ui::UI`, and the `rec::Recorder` (`cpp/recorder/recorder.{cc,hh}`). `App` distinguishes surface-tied teardown (`destroy_vulkan`, on surface loss/rotate) from full recorder shutdown — the camera keeps running across renderer recreation.

`Recorder` is a 4-state machine — `IDLE, PREVIEW, SAVING, FINALIZING` (`recorder.hh`). The interesting one is **`FINALIZING` (offline finalize):** `stop_saving()` returns immediately and a background `finalize_thread_` keeps draining the slower-than-realtime NLM/ISP backlog through the encoder and muxer after the camera has stopped — the UI shows **"Processing N%"** (`Recorder::finalize_percent()` → `ui/ui.cc`). No new recording can start until it returns to `PREVIEW`.

**Preview freezes during RAW recording (intended).** At idle the live preview renders normally, but while RAW-recording the Java session **drops the preview target from the capture request** (`HdrCameraSession.java`, the `rawRecord` branch) and `App` **skips swapchain presents** (`app.cc`) — both hand the whole GPU to the ISP compute, which a periodic compositor present would otherwise stall ~90 ms and drop frames.

### Two video pipelines (chosen at runtime in `Recorder::choose_video_mode`)

- **`RAW_PQ` (native, preferred, device-verified):** Java streams RAW16 Bayer into a shared `DEVICE_LOCAL|HOST_VISIBLE` staging buffer that the **Vulkan compute ISP** reads directly as a `StructuredBuffer` — **no upload copy** (the camera-thread memcpy stays; AHardwareBuffer import is impossible for RAW16 on Adreno). `cpp/isp/raw_video_pipeline.cc` runs, per frame: an optional Bayer denoise prepass (`denoise_bayer.slang`), then the demosaic — default **HQ directional**, a two-pass RCD-style method (`green_isp.slang` builds a directional green plane → `debayer_isp.slang` reconstructs R/B from residual color differences), selectable to the single-pass **Malvar** path in the same `debayer_isp.slang` (demosaic mode = `push.cfa` **bit 8**) — then white balance, CCM to BT.2020, PQ ST 2084, P010 pack. An optional **chroma-only denoise** then runs *post-ISP*: when on (`push.cfa` **bit 9**), `debayer_isp.slang` diverts the noisy CbCr into a per-slot scratch buffer and `chroma_denoise.slang` runs a luma-guided bilateral over chroma, leaving the **luma plane byte-identical**. Output → `AMediaCodec` HEVC Main10 byte-buffer encoder → native muxer. These passes are switched by `RawVideoPipeline` atomics (`set_denoise/set_demosaic_hq/set_temporal/set_chroma`); current **defaults = HQ demosaic + chroma denoise on**, with the legacy Bayer denoise and the motion-adaptive `temporal_denoise.slang` present but **off** (the temporal pass is device-verified to smear handheld motion — a known dead end, kept dormant for reference). The ISP uses its **own Vulkan device**, separate from the rendering one (`cpp/isp/vk_compute.cc`), and a `kInFlight=3` command-buffer ring overlaps GPU compute with the CPU readback+encode.
- **`LEGACY_HLG` (Java fallback):** for devices without RAW or a P010 encoder — the Java `HdrCameraSession` HLG10 MediaCodec path. Record/preview aspect comes from the sensor's true active array (`sensorAspect()` ← `SENSOR_INFO_ACTIVE_ARRAY_SIZE`), **not** `getOutputSizes()` max-area: the framework moves sub-30 fps modes to the *high-resolution* list, so on some sensors the largest *full-rate* output is a square and the old max-area heuristic recorded a 1:1 crop.

The RAW_PQ pipeline is **device-verified** on a Galaxy S23 Ultra (SD8g2 / Adreno 740): sustained **30.0 fps / 0 drops** at 4080×3060, HEVC Main10 PQ. Frame rate (30 fps RAW — sensor-locked) and bitrate (~240 Mbps — HEVC L6.x tier ceiling) are **hardware/encoder walls**, not tunables. See `~/.claude/.../memory/raw-pq-pipeline-status.md` for the full optimization history and the items still worth watching (encoder stride bytes-vs-pixels heuristic, sensor-clock BOOTTIME→MONOTONIC rebase, the `wb_valid_` gate).

**Frame store + offline develop.** RAW16 frames flow from the camera through `cpp/isp/frame_store.{cc,hh}`, a FIFO between the ~30 fps producer and the slower-than-realtime NLM/ISP consumer. The header comment still describes a disk-spill design, but the **`.cc` is authoritative and never spills**: a bounded in-RAM queue (~16 frames ≈ 0.4 GB); under sustained overload `push()` drops the *newest* frame past the cap rather than writing tens of GB to storage. When develop falls behind during capture, the backlog is finished after Stop in the `FINALIZING` phase (see Lifecycle). A strong NLM pass (`nlm_bayer.slang`) runs in this offline develop; see `~/.claude/.../memory/offline-finalize-nlm.md`. An experimental **NCNN (Vulkan) DNCNN Bayer denoiser** also exists (`cpp/isp/ai_denoiser.{cc,hh}`): it is `init()`'d in `raw_video_pipeline.cc` but its `run()` call is **commented out**, so it is **not in the live pipeline** — scaffolding, like the dormant temporal pass.

### Still photos (the shutter)

The **shutter** (`ui::UI::Action::SHUTTER` → `app.cc` → `Recorder::take_photo` → `jni::hdr_take_photo`) routes to **Java `HdrCameraSession.takePhoto`**. Java picks the still stream by an `activeStill` flag set when the session is built:

- **RAW devices → DNG.** A 3-shot exposure-bracketed RAW16 burst; each frame is paired with its capture result by sensor timestamp and written via `nativeOnRawFrame` → `cpp/camera/dng_writer.cc`.
- **Non-RAW devices → full-res YUV → lossless PNG.** Devices without a Camera2 `RAW` capability capture the largest `YUV_420_888` size and save it losslessly: `onStillImage` copies the planes, then a **dedicated single-thread worker** (with a FIFO output-name queue so bursts don't mislabel) runs `nativeOnStillFrame` → `cpp/camera/still_writer.cc` (vendored `third_party/stb_image_write.h`; BT.601 limited-range YUV→RGB, sensor orientation baked into the pixels, NR/edge **off**). The encode is slow (~9 s for 12 MP — `stb`'s deflate), so it must stay off the camera reader thread; the worker is `shutdown()`-ed (not killed) on stop so queued shots still flush.

**PHOTO vs VIDEO is a session split** on the legacy/non-raw path: `preview + still` in PHOTO mode vs `preview + encoder` in VIDEO mode (`setPhotoMode` recreates the session on the UI toggle) — never preview+encoder+still together (not a guaranteed stream combo). RAW-video devices are unaffected (their session is preview+RAW16, and stills come from the same RAW stream).

**No "max resolution" / 50 MP.** A vendor-interpolated 50 MP mode was built and **removed**: budget Quad-Bayer-marketed sensors expose 8160×6144 only via a private ODM tag (`com.ontim.private.metadata.availableStreamInterpolaCfg`, i.e. *interpolated*, not real detail), and the standard `SCALER_STREAM_CONFIGURATION_MAP_MAXIMUM_RESOLUTION` is **null** without the `ULTRA_HIGH_RESOLUTION_SENSOR` capability — the NDK reads the same HAL metadata, so it's unreachable from "pure C++" too. The sensor's real binned max (~12.6 MP on that device) is the ceiling. Don't re-add it.

### Muxing, audio, UI

- **Muxer** (`cpp/muxer/`, libmatroska + libebml): writes `.mkv`. A **single writer thread muxes both audio and video** (`Recorder::MuxPkt` queue) so disk I/O on cluster flushes never backpressures the encoder drain or the FLAC capture thread. **EBML-head gotcha:** the file's `kWriteDefault` is libebml's `WriteSkipDefault`, which omits any element whose value equals the libebml default — including the EBML **`DocType`** ("matroska" *is* its default). Rendered that way the file has doctype `(none)`: ffmpeg/VLC/mpv cope, but **strict players/editors then build no timeline**. The EBML head must therefore render with `kWriteAll` (`WriteAll`) so `DocType` is emitted; everything else stays `kWriteDefault`. The Segment is intentionally left **unknown-size with no final SeekHead** (streaming-valid); `KaxDuration` is patched in place at `close()`.
- **Audio** (`cpp/audio/audio_capture.cc`): USB DAC capture via `libusb` (from the `audio_engine` submodule), encoded inline to **FLAC** and muxed into the `.mkv` at the native rate; `aaudio` (built-in mic) is the fallback when no DAC is attached. **Source selection + FLAC init (`start()`):** `open_fd()` adopts the USB device's *highest* advertised rate/depth/channels ("max everything"), and `Recorder` falls back to the internal mic if a granted USB device exposes no capture stream (playback-only DAC). The muxer only adds the audio track when `is_capturing() && !codec_private().empty()`, and `codec_private()` is only filled once `FLAC__stream_encoder_init_stream` succeeds — so a **failed FLAC init silently drops the whole audio track** (and with it the `_ai.flac` side-car). libFLAC's *streamable subset* rejects sample rates **≥ 655360 Hz** (705.6/768 kHz high-end ADCs hit this), so `start()` retries init with `streamable_subset(false)` to keep the native raw rate (still lossless; FLAC max 1048575 Hz), after a sanity-guard on the negotiated format (rate>0, ch 1–8, bits 4–32 — also prevents a div-by-0 in the capture loop). It logs `FLAC encoder ready: …Hz …ch …bit (subset|non-subset)`. Sensor PTS vs audio clock domains are reconciled in the RAW pipeline (BOOTTIME→MONOTONIC rebase). **DeepFilterNet denoise** (`cpp/audio/df_net.{cc,hh}`, **device-verified**): a real DeepFilterNet3 speech denoiser on **ONNX Runtime, CPU EP, `ORT_ENABLE_ALL`** — the 3 bundled ONNX models (`assets/models/tmp/export/{enc,erb_dec,df_dec}.onnx`) driven by a hand-ported, libdf-exact front/back-end (vorbis-window STFT, width-based ERB, `erb_norm`/`unit_norm`, ERB mask + optional **post-filter** + 5-tap deep filter, ISTFT; `kiss_fft` for the transforms). Runs **offline over the whole clip in the `FINALIZING` phase** (the exported models have no GRU state I/O, so per-frame streaming is impossible), **chunked-with-warmup**, per-channel (**stereo preserved**). Output is a single **48 kHz/24-bit `<clip>_ai.flac` side-car** — DeepFilterNet **+ post-filter baked in** (`DfNet::process(buf, post_filter)`). **By design the `.mkv`'s own audio stays the untouched native-rate raw track** (the user wants the rawest video; the denoise is a convenience side-car, deliberately NOT muxed in), and **no loudness normalization** is applied (it would crush conversational dynamics). Measured on-device: noise floor −44→−63 dB (**≈+19 dB SNR**), voice level/dynamics preserved. If the model is missing/fails, `finalize_denoise()` falls back to the original audio per channel. **CPU EP only** — XNNPACK/NNAPI/QNN were tried and don't help (model is GRU-bound; the bundled `.so` does contain those providers). `df_validate.py` (repo root) is the desktop parity/quality **oracle** — it runs the real ONNX models via `libdf` and is the regression check for any front/back-end change. First-chunk `DfNet`-tagged stage diagnostics (input/feat/mask/coefs/output rms·min·max·nan) localize on-device issues. `rnnoise` remains an unused submodule. See `~/.claude/.../memory/deepfilternet-audio-impl.md`. **USB teardown gotcha:** `UsbAudioDriver` (in `audio_engine`) runs a *refcounted* libusb event thread; a **mid-stream device detach** can leak that refcount (`stopCapture()`'s "already torn down" early-return skips `releaseEventThread()`), leaving the thread spinning in `libusb_handle_events` — then `libusb_exit()` aborts destroying a still-held mutex (`pthread_mutex_destroy == 0` assertion) on the next **Stop**. `UsbAudioDriver::close()` force-zeroes the refcount and joins the thread before `libusb_exit`. Separately, a **silent USB-DAC recording is almost always the interface's input gain at zero**, not a bug — capture/encode/mux all work, the samples are just noise-floor.
- **UI** (`cpp/ui/ui.cc`): drawn with `Vk_Canvas_Lb_LAW` + `vulkan_font_engine` (MSDF fonts), composited over the camera preview (which freezes during RAW recording — see Lifecycle). The recording UI is just the **shutter + a PHOTO/VIDEO switch** (`ui::UI::Action` is `{NONE, TOGGLE_MODE, SHUTTER}`; touch handling in `app.cc`), plus a **"Processing N%"** readout during the `FINALIZING` offline drain. Earlier builds exposed DN/DM/TD/CD RAW-pipeline A/B toggle buttons; those were **retired** once the best config was locked in as the default — the denoise/demosaic knobs now live only as `RawVideoPipeline` atomic defaults, not UI. The `Recorder::set_*`/`RawVideoPipeline::set_*` hooks remain for re-wiring.

## Libraries under `libs/` & the "improve libraries on the fly" rule

Libraries are organized into `libs/firstparty/` (minervarr repos) and `libs/thirdparty/` (external dependencies):

- **First-party submodules** (`libs/firstparty/`): `Vk_Canvas_Lb_LAW` (canvas + font engine, bundles FreeType + msdfgen), `archive_engine`, `audio_engine`, `regen_atlas`.
- **Third-party submodules** (`libs/thirdparty/`): `libebml`, `libmatroska`, `flac`, `ncnn` (NCNN, built with `NCNN_VULKAN ON`), `rnnoise`. `libusb` comes via `audio_engine`'s `usb_audio.cpp`.
- **Vendored source (not submodules), also built by CMake:** `libs/thirdparty/kiss_fft` and `libs/thirdparty/soxr` (`CMakeLists.txt` ~lines 23–40).
- **Prebuilt import (not compiled):** `libs/thirdparty/onnxruntime` — an `IMPORTED` `.so` per ABI (`CMakeLists.txt` ~lines 43–48).

Per `GUIDE.md`'s core principle: when this app needs a capability from `Vk_Canvas_Lb_LAW`, `audio_engine`, etc., **the fix belongs in that library's repo**, not in a workaround here, so every consuming project benefits. (So real bug fixes land *inside* `libs/firstparty/<submodule>/…` — e.g. the USB teardown fix above is in `audio_engine`, and the camera-preview compositing — `Renderer::update_camera_frame`/`clear_camera_frames`, AHardwareBuffer→Vulkan YCbCr import — lives in `Vk_Canvas_Lb_LAW`'s `renderer.cc`.) **Renderer-teardown gotcha:** `Renderer::cleanup_hwb_resources()` destroys the descriptor pool *and then* calls `clear_camera_frames()`, which `vkResetDescriptorPool`s it again → a Mali `pthread_mutex_destroy` abort on every background/surface-loss. Handles must be nulled right after `vkDestroy*` so the second pass skips them.

**Exception to note:** the `archive_engine` submodule's native API changed and no longer builds against this project, so its native usage was replaced by a local restored copy at `cpp/archive.cc` (`archive::get_documents_path`); only its C++ *headers* are still on the include path (CMake). Its **Java was unused and is no longer on the source path** (dropped when the camera became a library). `audio_engine`'s Java moved to the `:engine-audio` module. Don't assume the `archive_engine` submodule native code is in use.

## Repo hygiene notes

- `camera/.cxx/` (CMake/Ninja build cache) is committed and shows up churned in `git status` — generally not something to hand-edit; don't include its noise in feature commits.
- `script.py` / `script_writer.py` / `patch_hwb_cache.py` are standalone Python dev/bundling helpers, not part of the Gradle build. `build_model.py`, `convert_dncnn.py`, `convert_dncnn_bayer.py`, and `dump_erb.py` are likewise standalone helpers for the AI work (model build/conversion and ERB-filterbank dump). `df_validate.py` is the DeepFilterNet audio **parity/quality oracle** (needs the `.venv_313` venv with `onnxruntime`/`libdf`/`soundfile`); `tmp_df_golden/` holds its scratch/golden fixtures (dev-only, ignorable).
- The old release `signingConfig` (checked-in keystore password) was **removed** when `app/` became the `:camera` library — the `.aar` is unsigned (the host app signs), and `:demo` uses debug signing. `bruno.jks` remains at the repo root but is no longer referenced by any `build.gradle`. Do not reintroduce the password into new files or logs.
