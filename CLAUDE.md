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
- **Shaders**: `CMakeLists.txt` resolves `slangc` from `$VULKAN_SDK`, else searches `/opt/shader-slang/bin`, `/usr/local/bin`, `/usr/bin`; override with `-DVCE_SLANGC=/path/to/slangc`. Three `vce_compile_slang` rules produce SPIR-V into `camera/src/main/assets/shaders/` (gitignored): the font and canvas shaders from `libs/firstparty/Vk_Canvas_Lb_LAW/...`, and **this project's own ISP compute shaders from `camera/src/main/cpp/isp/shaders_src/`** (`nlm_bayer`, `green_isp`, `debayer_isp`, `chroma_denoise` — the `.slang` sources ARE committed). `camera/build.gradle` forces `merge*Assets` to depend on the `buildCMake*` tasks: without that the asset merge runs first on a clean clone and ships an APK with no shaders at all, which makes `RawVideoPipeline::init()` silently return false and the RAW path vanish.
- Toolchain: NDK `29.0.14206865`, CMake `3.22.1+`, `compileSdk 37`, `minSdk 26`. The `:camera` library has **no `targetSdk`** (the host governs it at runtime); the `:demo` app pins `targetSdk 28`. Captures no longer depend on legacy external storage — `CameraActivity.cameraOutputDir()` writes to the host's `getExternalFilesDir()` (scoped-storage-safe), so a modern host (targetSdk 33+) works. The old public-`Documents` path (`archive::get_documents_path`) survives only as a fallback for a non-`CameraActivity` host.
- `scripts/build.bat` (→ `scripts/build.ps1`) is a one-click **clean-clone** build: submodules + the ONNX Runtime `.so` **from source** (`scripts/build_onnxruntime.ps1`, which self-heals GitLab dep-hash drift in `cmake/deps.txt`) + the APK. For normal iteration use `gradlew`; `scripts/build.bat` is for a fresh checkout or to (re)build ORT into `libs/onnxruntime/lib/<abi>/`.
- No automated tests exist. Verification is on-device only (see below).

### Screenshots / device debugging

- `scripts/screenshot.bat` — grabs a screencap from the first attached adb device and pulls it locally.
- Native logs go to logcat **and** to `<externalDataPath>/app.log` (see `main.cc` / `logger.cc`). Key logcat tags: `RawVideo`, `Recorder`, `HdrCamera`, `DfNet`, `AudioCapture`.
- The RAW pipeline emits a per-window profile line (`RawVideo: .. fps | gpu A/MAXms enc-wait B/MAXms cam-gap MAXms ..`) for locating frame-rate stalls.
- Verify recorded output with `ffprobe` (expect HEVC Main 10, `yuv420p10le`, `bt2020nc/smpte2084/pc` for the RAW PQ path) and play it back on-device.
- **Gotcha that wasted a lot of time once:** when on-device behavior contradicts the source, first confirm the build actually **compiled and installed**. The native build can fail (a C++ error) while a *prior* APK stays installed, so you end up testing stale code. Verify by grepping logcat for a log string you know is new (`adb logcat -d -s DfNet:* AudioCapture:*`) before debugging the algorithm. From Git Bash, `adb pull`/`shell` of `/storage/...` paths needs `MSYS_NO_PATHCONV=1`.
- **Injected taps land ~70 px above where the UI sees them:** `input tap` coordinates are in screen space, but the app's touch handler receives them minus the status-bar inset — a tap at screen y=1462 arrives at app y≈1392. Calibrate against a screencap (find the button's pixel rows, inject those screen coords); the shutter's big circle forgives being off, the PHOTO/VIDEO chip does not, and a missed toggle silently leaves the app in the other mode.
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

The native `cpp/camera/` code (`dng_writer.cc`, `dng_meta_source.cc`) supplies the static DNG metadata reused by the RAW video ISP, and the **live shutter routes natively** through `ndkcam::Session::take_photo` on both RAW and non-RAW devices (Java `HdrCameraSession.takePhoto` remains the fallback for devices the native session can't serve). See **Still photos** below.

### Lifecycle / ownership

`android_main()` (`main.cc`) → `App` (`app.{cc,hh}`) owns the Vulkan `Renderer` + `Canvas`, the `ui::UI`, and the `rec::Recorder` (`cpp/recorder/recorder.{cc,hh}`). `App` distinguishes surface-tied teardown (`destroy_vulkan`, on surface loss/rotate) from full recorder shutdown — the camera keeps running across renderer recreation.

`Recorder` is a 4-state machine — `IDLE, PREVIEW, SAVING, FINALIZING` (`recorder.hh`). The interesting one is **`FINALIZING` (offline finalize):** `stop_saving()` returns immediately and a background `finalize_thread_` keeps draining the slower-than-realtime NLM/ISP backlog through the encoder and muxer after the camera has stopped — the UI shows **"Processing N%"** (`Recorder::finalize_percent()` → `ui/ui.cc`). No new recording can start until it returns to `PREVIEW`.

**Preview stays live during RAW recording (it used to freeze).** Both gates that froze it are gone: the session keeps the preview target in the repeating request, and `app.cc` presents while `SAVING`, throttled to 15 Hz. The ~90 ms compositor stall those gates worked around was mostly an *effect* — the ISP submitted each frame as one 12.5 Mpx `vkCmdDispatch`, and Adreno only preempts at dispatch boundaries, so any present queued behind an uninterruptible block. Each ISP pass is now issued as `RawVideoPipeline::kDispatchBands` row bands via `vkCmdDispatchBase` (loaded through `vkGetDeviceProcAddr` — Android's `libvulkan.so` exports only Vulkan 1.0), which gives the GPU preemption points at the same total cost.

### Two video pipelines (chosen at runtime in `Recorder::choose_video_mode`)

- **`RAW_PQ` (native, preferred, device-verified):** The capture session for this path is **native NDK Camera2** (`cpp/camera/ndk_session.{cc,hh}`): nothing it needs is Java-only, since `setDynamicRangeProfile` (the one API without an NDK equivalent) belongs to the legacy path. `Recorder::start_preview` prefers it and falls back to the Java session when `Session::available()` is false (no back camera with a RAW16 stream). It also removes a per-preview-frame JNI round trip — `AImage_getHardwareBuffer` replaces the Image→HardwareBuffer→global-ref→`releaseImage()` dance. Java streams RAW16 Bayer into a shared `DEVICE_LOCAL|HOST_VISIBLE` staging buffer that the **Vulkan compute ISP** reads directly as a `StructuredBuffer` — **no upload copy** (the camera-thread memcpy stays; AHardwareBuffer import is impossible for RAW16 on Adreno). `cpp/isp/raw_video_pipeline.cc` runs, per frame: a Bayer-domain **NLM denoise** prepass (`nlm_bayer.slang`, on by default), then the demosaic — default **Malvar** (single-pass 5x5 gradient-corrected, in `debayer_isp.slang`), selectable to **HQ directional**, a two-pass RCD-style method (`green_isp.slang` builds a directional green plane → `debayer_isp.slang` reconstructs R/B in color-difference space) via `push.cfa` **bit 8** — then white balance, CCM to BT.2020, PQ ST 2084, P010 pack. An optional **chroma-only denoise** runs *post-ISP*: when on (`push.cfa` **bit 9**), `debayer_isp.slang` diverts the noisy CbCr into a per-slot scratch buffer and `chroma_denoise.slang` runs a luma-guided bilateral over chroma, leaving the **luma plane byte-identical**. Output → `AMediaCodec` HEVC Main10 byte-buffer encoder → native muxer. These passes are switched by `RawVideoPipeline` atomics (`set_denoise/set_demosaic_hq/set_chroma`); **defaults = NLM on, Malvar demosaic, chroma denoise off** — HQ + CD together measured ~57 ms/frame, which holds 30 fps only until the GPU thermally throttles and then halves the frame rate (see the comment on `demosaic_hq_`). **Measured per-pass cost** (Adreno 740, 4080x3060, from the per-window profile line). The 30 fps budget is **33 ms/frame**:

| Pass | Cost | Note |
|---|---|---|
| ISP (demosaic + WB + CCM + PQ + P010), untiled | **33.3 ms** | 30.0 fps, 0 drops — the whole budget, on its own |
| NLM, search radius 2 (24 candidates) | **+71 ms** | 15 fps, ~40% of frames dropped |

**Reading `gpu` correctly:** it measures submit->fence-signalled, and the pipeline thread blocks on the frame store between submits. When the ISP is faster than the camera, that interval *is* the 33 ms capture period — so `gpu 33.3ms` at 30.0 fps with 0 drops means "headroom to spare", not "exactly at the limit". Only values **above** 33 ms are real GPU cost.

**fp16.** The NLM ships in two builds: `nlm_bayer.spv` (fp32) and `nlm_bayer_fp16.spv` (a 2-line wrapper setting `NLM_HALF`), picked at load time from `VkCompute::fp16_supported()` — `ComputeContext` enables `VK_KHR_shader_float16_int8` when the device advertises `shaderFloat16` (resolve `vkGetPhysicalDeviceFeatures2` through `vkGetInstanceProcAddr`; Android's `libvulkan.so` exports 1.0 symbols only, the same trap as `vkCmdDispatchBase`). Only the tile and the patch SSD are half; weights and accumulators stay fp32. This is what bought the thermal headroom: before it a clip held 30 fps for ~7 s then decayed to 52->58 ms of GPU time; after it, 306/306 and 310/310 frames at a flat 33 ms with zero drops.

This is why `kNlmSearchRadius` is **1** (8 candidates), not 2, and why both passes are tiled: at radius 2 there is no real-time denoise on this hardware at this resolution, full stop. It is also why the historical "30.0 fps / 0 drops" figure is only reproducible with `nlm=off` — that benchmark predates the NLM ever executing.

**The NLM is tiled, and that is load-bearing.** The straightforward version reads `srcBuf` **457 times per output pixel** (24 candidates x (9 patch samples x 2 reads + 1) + 1) — 5.7 billion global loads per frame at 4080x3060, measured at ~350 ms/frame on an Adreno 740 (8 fps, 253 of 299 frames dropped). `nlm_bayer.slang` now stages each group's whole footprint (a 28x20 tile, 2 240 B) in `groupshared` once, already normalized and edge-clamped, so the inner loops are pure LDS reads: **4.4 global loads per pixel**. `debayer_isp.slang` stages a 20x20 Bayer tile the same way (a group develops 16x16 px, Malvar reaches +/-2), replacing 3 328 global loads per group with 400. Two rules keep it that way — the 3x3 centre patch lives in three `float3` registers (it is invariant across all 24 candidates), and `S`/`P` are **compile-time** constants, not push constants: a runtime `P` stops the loops unrolling, makes the patch indices dynamic, and spills it to scratch memory, which costs more than the tiling saves. The `.spv` should contain **no `OpVariable %_ptr_Function__arr`** — check with `spirv-dis` after touching this shader. The NLM output buffer is **per-slot** (`InFlight::denoised_buf`); sharing it forced a full-buffer WAR barrier every frame that serialized the two in-flight slots into no GPU-GPU overlap at all.

A motion-adaptive temporal pass used to exist and was **deleted**: its retention constant had been set to 0, making it a full-resolution no-op that also gated the NLM off, so the "denoised" pipeline was in fact running no denoise at all. Don't reintroduce it — temporal was device-verified to smear handheld motion. The ISP uses its **own Vulkan device**, separate from the rendering one (`vce::gpu::ComputeContext`, aliased as `isp::VkCompute` via `cpp/isp/vk_compute.hh`), and a `kInFlight=2` command-buffer ring overlaps GPU compute with the CPU readback+encode.
- **`YUV_NATIVE` (native, for devices without RAW):** `ndkcam::Session` + `ndkcam::Encoder` (`cpp/camera/ndk_encoder.{cc,hh}`). The camera writes straight into `AMediaCodec_createInputSurface`'s window, which is a capture target — **no readback, no copy**, the one real virtue of the Java path it replaces. Two things it does better: the recording size is resolved **empirically** (candidates largest-first, the first the codec actually accepts wins) instead of by a `MediaCodecInfo` heuristic — on the moto g06 that is **1920x1440 instead of 1440x1088, 2.35x the pixels** — and the bitrate is driven to the encoder's 30 Mbit/s ceiling instead of 23.5. Where `MANUAL_POST_PROCESSING` exists it also sets a straight-line tonemap curve with NR and edge enhancement off, so the clip is gradeable rather than pre-cooked by the HAL. It does not attempt 10-bit: devices on this path have no dynamic range profile and (on the verified one) no Main10 encoder. **Device-verified end-to-end on the moto g06:** capability probe, size negotiation, encoder configure + input surface, capture-session creation, and a recorded `.mkv` (HEVC 1920×1440 HLG/bt2020 + FLAC) that `ffprobe` parses and plays; photo↔video mode switches rebuild the session at each REC boundary and stills stay real pixels across them.
- **`LEGACY_HLG` (Java fallback):** still the fallback whenever the native session reports it cannot serve the device — the Java `HdrCameraSession` HLG10 MediaCodec path. **Beware the name.** On the moto g06 it is not HLG10 at all: the device reports **no dynamic range profiles** (`HLG10 supported: false` in logcat) and its HEVC encoders expose only `ProfileMain`, so the path degrades to 8-bit SDR. The Java-only API the whole Java layer exists for therefore buys nothing on the only device that takes this path. Record/preview aspect comes from the sensor's true active array (`sensorAspect()` ← `SENSOR_INFO_ACTIVE_ARRAY_SIZE`), **not** `getOutputSizes()` max-area: the framework moves sub-30 fps modes to the *high-resolution* list, so on some sensors the largest *full-rate* output is a square and the old max-area heuristic recorded a 1:1 crop.

**`RAW_PQ` runs half-resolution (2×2 binned) by default — `kBinnedRawVideo` in `recorder.cc`.** Full resolution held 30 fps only while the GPU was cool; **device-measured on the S23 Ultra**, a full-res take decays to **18.0 fps with ~35 % of frames dropped** (4754 in / 3095 encoded / 1659 dropped, `gpu` 57–59 ms) after ~2.5 minutes of thermal throttling. Frame rate is the one thing a recorder cannot give up, so the binned path is the default and the full-res chain is one constant away.

The binned path is **two** compute passes instead of four, and it is a different algorithm, not a downscale of the old one:

- `bin_isp.slang` — each 2×2 CFA quad becomes one output pixel: `R` from the single red photosite, `G` from the mean of the two greens, `B` from the single blue. **There is no demosaic**: the green prepass, Malvar/RCD and the chroma denoise are all irrelevant here and their pipelines are never even created. That removes interpolation artifacts (zipper/maze) outright, but it has its own trade — the R, G and B samples of a quad sit at *different* sub-pixel positions, and a demosaic is what normally resolves them to a common one, so a hard high-contrast edge falling inside a quad shows a **thin colour fringe** (device-verified, visible on a blown white edge against shadow). It is a one-pixel effect at half resolution and was judged an acceptable price for holding 30 fps; if it ever needs fixing, the place is a half-pixel-aligned chroma reconstruction in `bin_isp.slang`, not a return to full-res demosaic. Output is scene-referred **linear camera RGB** (black-subtracted, normalized) — deliberately before WB/CCM/PQ, so the denoise sees sensor-domain noise. Each quad is read as **two aligned `uint32` words**, not four clamped samples, and its R/G/G/B layout is resolved once from `cfa & 3` (bit-exact against the reference `color_at` for all four patterns; no clamp is needed because `out_w = (raw_w/2) & ~1` guarantees `x0+1 <= raw_w-1`). `init()` refuses the binned path on an odd `raw_w`, since the packed-word read assumes word-aligned rows. `norm_at` clamps only the **top**: sensor noise straddles the declared black level (measured raw minima 43/42 against a BlackLevel of 64), and clipping the negative half of a zero-mean distribution leaves a positive, per-channel DC bias — fixed coloured speckle that no downstream denoise can remove, because it is a bias and not noise. Negatives flow through the bin and the NLM; `develop_rgb`'s `max(lin, 0)`, `pq_encode`'s saturate and `to_p010`'s clamp catch them at the end.
- `nlm_rgb.slang` (+ `nlm_rgb_fp16.slang`) — NLM over that plane, then highlight reconstruction, WB, CCM, PQ and the P010 pack folded into the same dispatch. The patch distance is computed on a **luma plane derived during the tile fill**, and those weights are applied to all three channels: a full RGB patch SSD would triple the work and give back most of the saving. That guide is **white-balanced first** (`c * push.wb.xyz`): raw camera R and B sit at roughly half of G before gains, so an un-gained mix is overwhelmingly green and effectively colour-blind — a boundary where the *colour* changes but green does not reads as "same surface", and the NLM averages both sides' R and B, bleeding colour across the edge. That is the one artifact class this pipeline must never produce, and it is also what would get worse if the search radius were ever widened. Same two non-negotiables as `nlm_bayer.slang` — the 3×3 centre patch in three registers, `S`/`P` compile-time — and the same `spirv-dis` check for `OpVariable %_ptr_Function__arr`.

**The video denoise is driven by the sensor's MEASURED noise model, not constants.** `ACAMERA_SENSOR_NOISE_PROFILE` gives (S, O) per CFA channel where variance at normalised signal `x` is `S*x + O` — exactly the form `nlm_rgb.slang`'s `noiseK`/`noiseFloor` take. `ndk_session.cc` already latched it from every capture result, ungated, including during video; nothing read it. `RawVideoPipeline::set_noise_profile` (fed from the `on_frame` sink, reusing the same `ndk_session_->noise_profile()` call the still DNG path makes) now does, with `kNoiseK`/`kNoiseFloor` as fallback and `kUseSensorNoiseProfile` to A/B. **Device-measured on the S23 Ultra: the constants overstate variance by 12-17x in a dark scene and ~30x (shot) / ~141x (read floor) in a brighter one.** Since the NLM weight is `exp(-ssd/h^2)` with `h = kNlmH*sigma`, an inflated sigma pushes every candidate's weight to ~1 — the "edge-preserving NLM" was arithmetically a **plain 3x3 box blur** in bright scenes. `kNlmH` was raised 1.25 -> 4.0 to be a real "how many sigma is the same surface" threshold now that sigma is true (noise-only neighbour scores `exp(-2/h^2)` = 0.88; a 10-sigma edge scores 0.002). The profile genuinely tracks ISO (measured S = 0.00103 / 0.00131 / 0.00328 across scenes), which is what makes the denoise strength automatically right without a control.

**What the measurements actually showed, including the negative results.** All device-verified on one fixed scene (phone untouched, exposure matched to ~1%, framing correlation checked — an earlier round of comparisons was **invalidated** because the phone moved between takes, so always verify framing before trusting a noise delta):
- **The NLM does real work**: turning it off raises shadow luma noise 7.1 -> 13.0 and mid-tone 2.3 -> 8.9, and drops the edge-to-flat gradient ratio from 2.4 to 1.85.
- **But `kNlmH` 1.25 vs 4 vs 8 measured identical** (luma, colour and edge sharpness all within 0.2%). All three sit at effectively maximum weight, so the knob has no headroom left in a dark scene — a 3x3 search window is the binding constraint, not the strength. Do not expect to tune your way out of dark-scene noise with `kNlmH`.
- **The chroma median is the lever that works** on the "random coloured dots": one pass cut colour speckle ~30%, two passes **38-43%** in the shadows, at no measurable GPU cost (`gpu` unchanged at 33.5-33.9 ms, 30.0 fps, 0 drops).

**`chroma_median.slang` — why a median and not a blur.** A weighted average (bilateral/gaussian/NLM) computes a *new* value by mixing neighbours, which near a high-contrast edge mixes both sides — that is how a chroma denoise produces the coloured halo/"aura" that this project must not ship. A median *selects* one of the nine values already present, so it cannot invent a colour or bleed across an edge, and it is the classic optimal filter for isolated outliers, which is exactly what the speckle is. It runs **twice** (`chroma_buf -> chroma_buf2 -> the P010 CbCr plane`, one generic src->dst pipeline distinguished by `dst_offset`): repeated median drives the signal toward its root and clears clustered speckle a single pass leaves, while staying a median throughout — so the no-halo guarantee is independent of how many passes run, unlike widening a blur. Two 3x3 passes are also cheaper than one 5x5 (2x19 compare-exchanges vs ~99). **Luma is never written by this pass**, so the Y plane is byte-identical with it on or off. Scratches are **per-slot**.

**Reading the profile line: `gpu` is NOT the GPU cost — `gpu-real` is.** `gpu` measures submit->fence-signalled, and `pipeline_loop` blocks in `store_.pop()` waiting for the camera between submits, so whenever the ISP keeps up that interval *is* the ~33 ms capture period. A flat "33.8 ms" therefore means "we keep up", never "this is what it costs". A second line, `gpu-real: bin .. nlm .. med1 .. med2 .. = X ms of 33.3 ms budget (N% used)`, reports **actual** per-pass time from `VK_QUERY_TYPE_TIMESTAMP` query pools (one per in-flight slot, `vkCmdWriteTimestamp` at `BOTTOM_OF_PIPE` after each dispatch, read in `retire()` where the fence is already signalled so it never blocks). `ComputeContext::timestamp_period_ns()` returns 0 when the compute queue family reports `timestampValidBits == 0`, and the whole feature then silently disappears — it must never become a failure path. **Budget any new work against `gpu-real`, never against `gpu`.** **Device-measured on the S23 Ultra (binned, 2040x1530):** `bin 2.7 | nlm 16-18.5 | med1 0.7 | med2 0.7 = ~21.6 ms of 33.3 ms (65%)`, flat across a 3.4-minute take. Two things follow: the two chroma-median passes really are ~4% of budget (previously only inferred from `gpu`), and **the NLM is ~80% of the frame**, so raising `kNlmSearchRadius` to 2 (24 candidates vs 8) would land near 45-50 ms and is NOT affordable even at quarter resolution. That question is now settled with a number rather than a guess.

**A fence timeout must never let an in-flight slot be reused.** `retire()` gives a throttled GPU one long grace period (2 s, then 8 s more), and if the submission still cannot be reclaimed it latches `gpu_lost_` and `pipeline_loop` **breaks before the ring rotates**. The alternative — counting a drop and carrying on, which is what the code used to do — leaves the command buffer PENDING and the fence in use, and two frames later `vkResetCommandBuffer` + `vkQueueSubmit` on those objects is undefined behaviour that faults or wedges the queue, reachable precisely when the GPU is throttled. Losing the tail of a clip is acceptable; corrupting the queue is not. Never "fix" a timeout by resetting the fence — that is the same UB.

**No motion ghosting is structural, not a tuning result.** The whole binned path binds only four buffers — the current frame's raw staging slot plus three per-slot intermediates (`rgb_buf`, `chroma_buf`, `chroma_buf2`) — and a shader can only read what is bound. There is no past-frame binding anywhere, so a trail or coloured aura behind motion is impossible by construction. Keep it that way: the deleted temporal pass needed explicit past-frame bindings.

**Binning is not a denoise.** It halves *green* variance (two samples averaged, −3 dB); R and B are still single photosites and their noise is unchanged. What binning actually buys is a quarter as many pixels for the real denoise to work on. `RawVideoPipeline::bin_noise_scale()` rescales the sensor's per-photosite (S, O) to the plane `nlm_rgb` actually measures — the **white-balanced** guide luma over binned RGB. Two factors fall out, not one: with `A = Σ wᵢ²gᵢ²vᵢ` (vᵢ = 1 for R/B, ½ for the averaged greens) and `B = Σ wᵢgᵢ`, the shot slope scales by **A/B** and the read floor by **A** (~0.37 and ~0.49 at typical gains). A single 0.28 was correct only while the guide was un-gained; `kBinNoiseScale` survives as the no-WB fallback. The shader's `kLumaW` and this derivation **must change together**.

Bitrate uses its own `kTargetBppBinned` = **2.60** (not `kTargetBpp`'s 0.80), landing at **243 Mbps** — deliberately the same order as the full-res path's 299 Mbps, so a quarter of the pixels get ~3× the bits each and the encoder stops spending its budget describing noise `nlm_rgb` already removed. File size per minute is roughly unchanged (~1.8 GB/min).

Both intermediates (`InFlight::rgb_buf`) are **per-slot**, for the same reason `denoised_buf` is: a shared one forces a full-buffer WAR barrier that serializes the in-flight ring into no GPU–GPU overlap.

**The ENCODED picture is CTU-padded, and that is a real domain split — `out_w_/out_h_` is NOT what goes into the file.** HEVC codes in 64x64 CTUs anchored top-left, so a picture that is not a multiple of 64 leaves partial CTUs along the right and bottom edges; the encoder fills them itself and signals a conformance window, and **an HEVC conformance window crops only from the right and the bottom**. That asymmetry was the fingerprint of a streaked ~1 mm border artifact present on every clip the project had ever produced — horizontal streaks down the right edge, vertical along the bottom, never top or left (which also rules out our own shader edge clamps, since every one of them clamps identically at both ends). Both native geometries were unaligned: 2040/64 = 31.875, 1530/64 = 23.9, and full-res 4080/64 = 63.75, 3060/64 = 47.8.

`init()` therefore rounds the encoded size **up** to `kCtuAlign` (64) into `pad_w_/pad_h_`: 2040x1530 -> **2048x1536**, 4080x3060 -> **4096x3072** (+0.79 % pixels, and both stay exactly 4:3 so nothing is rescaled or distorted). **Nothing is cropped** — the sensor's whole field of view survives and the frame merely gains a few columns/rows of *replicated* border, which is the standard edge extension: a motion vector pointing into the pad then predicts from real picture content instead of invented data. The pad costs no extra pass, because the fill falls out of a clamp that already existed.

The split to keep straight — get it backwards and you either read `rgb_buf` out of bounds or shred the chroma pitch:

| domain | what lives there |
|---|---|
| **real** `out_w_/out_h_` | `rgb_buf` sizing, the `bin_isp` push + dispatch, and `nlm_rgb`'s new `srcDim` |
| **padded** `pad_w_/pad_h_` | the P010 `out_buf` and chroma scratches, every stride/`uv_word_offset`, the `nlm_rgb`/`debayer_isp`/`chroma_median`/`chroma_denoise` dispatch grids, the encoder format, `submit_to_encoder`, the bitrate target, and `on_format_` (hence the muxer's track geometry) |

`RawVideoPipeline::out_width()/out_height()` return the **padded** size (it is what the bitstream contains); `src_width()/src_height()` return the real one. `nlm_rgb.slang` gained `srcDim` for this — `load_rgb` clamps to it, which *is* the replication, and `NlmRgbPush` grew 96 -> **112 bytes** with two explicit pad words so the four `float4` rows stay 16-byte aligned (verify with `spirv-dis`: `wb` must sit at offset 48, the CCM rows at 64/80/96). `debayer_isp.slang` needed **no edit at all** — it already clamps its raw read to `rawW/rawH`, so dispatching it over the padded grid replicates for free.

**Reducing the RAW16 stream size instead is not available on this device** — `ndk_session.cc` now logs the whole advertised list, and the S23 Ultra offers exactly one (4080×3060). If a device ever offers more, note the size is chosen **twice and independently** (`ndk_session.cc` sizes the AImageReader, `dng_meta_source.cc`'s `load_static_meta` sizes the ISP); change one without the other and `on_frame` drops every frame on a geometry mismatch.

The RAW_PQ pipeline is **device-verified** on a Galaxy S23 Ultra (SD8g2 / Adreno 740). Binned: **11 355 frames in / 11 355 encoded / 0 dropped** over a 6 min 20 s take at 2040×1530, a flat `gpu` 33.6–34.4 ms throughout, with the OS thermal status rising to 1 mid-take and the frame rate not moving. The historical full-res figure was sustained **30.0 fps / 0 drops** at 4080×3060 — true only while cool (see above). Frame rate (30 fps RAW — sensor-locked) and bitrate (~240 Mbps — HEVC L6.x tier ceiling) are **hardware/encoder walls**, not tunables. See `~/.claude/.../memory/raw-pq-pipeline-status.md` for the full optimization history and the items still worth watching (encoder stride bytes-vs-pixels heuristic, sensor-clock BOOTTIME→MONOTONIC rebase, the `wb_valid_` gate).

**Camera lifecycle gotcha (native session).** `ndkcam::Session::init()` calls `shutdown()` first and `Recorder::stop_preview()` fully releases the session (not just `stopRepeating`). Without both, each preview→stop→preview cycle leaked an open `ACameraDevice`; the HAL then rejects every later client with `connectHelper: Could not initialize client from HAL` — which wedges the camera **device-wide**, defeats the Java fallback too, and only clears on reboot.

**Frame store + offline develop.** RAW16 frames flow from the camera through `cpp/isp/frame_store.{cc,hh}`, a FIFO between the ~30 fps producer and the slower-than-realtime NLM/ISP consumer. It is a **bounded in-RAM queue and never touches disk** (16 frames ≈ 0.4 GB); under sustained overload `push()` drops the *newest* frame past the cap rather than writing tens of GB to storage. The dead disk-spill path — plus the per-recording I/O thread that could never do any work — was **deleted**. When develop falls behind during capture, the backlog is finished after Stop in the `FINALIZING` phase (see Lifecycle). A strong NLM pass (`nlm_bayer.slang`) runs in this offline develop; see `~/.claude/.../memory/offline-finalize-nlm.md`. An experimental NCNN (Vulkan) DNCNN Bayer denoiser used to sit here (`cpp/isp/ai_denoiser.{cc,hh}`); its `run()` was never called, yet its constructor still spun up a whole ncnn Vulkan instance per pipeline, so it and the **ncnn dependency** were removed from the build entirely.

### Still photos (the shutter)

The shutter (`ui::UI::Action::SHUTTER` → `app.cc` → `Recorder::take_photo`) routes
**natively whenever the native session is live** (`ndkcam::Session::take_photo`);
Java `HdrCameraSession.takePhoto` is now only the fallback for devices the native
session can't serve:

- **RAW devices → DNG only.** A 3-shot exposure-bracketed RAW16 burst; each frame is paired with its capture result by sensor timestamp and written via `nativeOnRawFrame` → `cpp/camera/dng_writer.cc`. The bracket is merged by `cpp/camera/hdr_merge.cc` (alignment by `cpp/camera/mtb_align.cc`, Ward median-threshold-bitmap pyramid — exposure-invariant, integer-pixel).

  **The bracket has to be a *real* bracket.** `Session::take_photo` builds one `TEMPLATE_STILL_CAPTURE` request **per shot** at -2 / 0 / +2 EV (index 0 darkest, the order `hdr_merge`'s `BracketFrame::index` assumes), quantizing the EV to `ACAMERA_CONTROL_AE_COMPENSATION_STEP` and clamping to `..._RANGE`; a device that cannot bracket collapses to all-zero compensation. It used to queue `shots` copies of *one* request — three identical exposures, so the merge had no highlight headroom at all. `on_capture_completed` latches each shot's measured `SENSOR_EXPOSURE_TIME`x`SENSITIVITY` into a FIFO consumed alongside `still_paths_` in `on_raw_image`, so `hdr_merge` derives real gain ratios and only falls back to nominal 2-EV steps when AE reports nothing. **No viewable copy is written** — see **Capture-only** below.
- **Non-RAW devices (native path) → full-res YUV → lossless PNG.** `pick_camera()` also picks the largest `AIMAGE_FORMAT_YUV_420_888` size; the idle session is preview + still (the encoder surface joins only at REC — see **Two video pipelines**). `Session::take_photo` queues the output name and fires a one-shot `TEMPLATE_STILL_CAPTURE` at the still target with NR/edge off; `on_still_image` copies the planes synchronously and a **worker thread** encodes via `cam::write_png_yuv420` (8-bit sRGB, orientation baked in, lossless) and registers the file. On these devices that PNG **is** the capture — there is no DNG behind it — so it must be lossless *and* openable everywhere, which JXL is not.
- **Non-RAW devices (Java fallback) → full-res YUV → lossless PNG** (unchanged): `onStillImage` copies the planes into a FIFO-name worker → `cam::write_png_yuv420` (~9 s for 12 MP).

**Gralloc-usage gotcha that black-filled every native still (device-verified on
the moto g06 / MTK P1):** the still reader must be created with
`AImageReader_newWithUsage(..., AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, ...)`.
With usage 0 the capture session configures fine, frames are *delivered*, and
the buffers contain **zero-filled gralloc memory** — no error anywhere. The
framework's `ImageReader` always allocates YUV consumers with CPU-read usage,
which is why only the Java path ever produced pixels. Bisected clean: preview
size, request extras, templates, and an "arming" pre-shot all make no
difference; the usage bits alone are the fix.

### Capture-only: the DNG is the deliverable

**Nothing viewable is written next to a DNG.** Developing, denoising and rendering
happen in a **post app** (ViewMage, extended to read DNGs) — the camera's job ends
at the sensor data. A `.jxl` used to be developed alongside every still; it was a
second, lossier copy of what the DNG already holds, and Android cannot decode JXL
anyway (verified: the platform media scanner reports `width=NULL` for a `.jxl`
while reading an `.avif` fine), so it was not viewable on the device that shot it.

| photo mode | output |
|---|---|
| 0 FAST | 1 DNG |
| 1 STATIC | 1 merged DNG |
| 2 STATIC + RAW set | 3 source DNGs + 1 merged DNG |

**`libjxl` is gone from the build** along with it — with no caller left it was
linking ~47 MB (debug) of encoder plus its vendored highway/brotli/lcms into
every build. Removing it took the debug APK from **113.6 MB to 66.3 MB** and
`libcamera_recorder.so` from **63.1 MB to 15.8 MB**. If it ever returns, mind the
trap it left: AGP's debug variant leaves `CMAKE_*_FLAGS_DEBUG` at `-g` with no
`-O`, so an `add_subdirectory` dependency inherits `-O0` while `camera_recorder`'s
own `target_compile_options` keep it at `-O2` — libjxl+highway at `-O0` encoded a
12 MP still ~20x slower, minutes instead of seconds. Suspect this for any future
`add_subdirectory` dependency that seems inexplicably slow.

**`NoiseProfile` (DNG tag 51041) is written on every RAW still.** One (S, O) pair
per CFA channel from `ACAMERA_SENSOR_NOISE_PROFILE`, latched off each capture
*result* (`ndk_session.cc` — it tracks the ISO actually used, so it is not a
static characteristic) and emitted by `tiny_dng_writer.h`'s `SetNoiseProfile` as
`TIFF_DOUBLE`. Variance at normalised signal `x` is `S*x + O`: the Poisson shot
term scales with signal, the Gaussian read term does not. It has to be DOUBLE —
`O` is order 1e-6 and a rational would quantise it to zero.

This tag is what lets post denoise *correctly* instead of estimating the noise
level from the pixels. It matters most for a learned denoiser: told the wrong
noise level, a model either smears real detail away or synthesises texture the
scene never had. It is also the piece that makes **one model work across phones** —
the raw-domain equivalent of Whisper's log-mel front-end (normalise by black/white
level, canonicalise the CFA order, and *tell the model the noise level*), without
which a model trained on one sensor's 14-bit RGGB produces confident nonsense on
another's 10-bit GBRG.

### Still develop (`cpp/camera/raw_develop.cc`) — scene-referred, no tone curve

The develop applies exactly four things: black subtract, white balance, the CCM,
and the transfer curve. **There is no tone curve and no rendering intent** — the
output is scene-referred, which is why a still looks flat and "washed" next to a
stock-camera JPEG. That is correct for an archival/post pipeline and wrong for a
finished picture; the display rendering belongs in the post app.

Two measured properties of the current develop, both on a real S23 Ultra shot:

- **There is NO black-level pedestal, and the measurement that appeared to show
  one was wrong.** The claim was that the darkest 32x32 *blocks* of a frame mean
  **64.56–65.18** against a declared `BlackLevel` of 64, i.e. black never reaches
  zero. That number is real but measures the *picture*, not the sensor: a block
  mean averages the noise away and so reports the local **signal** level, and the
  darkest region of a normally-lit scene is not pure black. The statistic that
  answers the question is the **whole-frame low percentile**, and on two real S23
  Ultra frames it lands at or *below* the declared black — 63.0 (bright scene) and
  57.0 (night scene), with raw minima of 43 and 42. Noise straddles the declared
  black in both, so the black level is correct and there is nothing to subtract.
  An auto black point was written for this and **removed**; don't re-add it
  without a whole-frame measurement that actually shows a floor above black.
  What makes blacks *look* lifted is the missing display tone curve (below).
- **Beware measuring shadow tint by ranking pixels on luminance.** Luminance is
  68% green, so selecting the darkest individual pixels preferentially picks ones
  where green noise was low, which inflates B:G by construction. That method
  reported B:G = 2.23 in the deep shadows; selecting by **spatial block mean**
  instead gives **1.43** (midtones 0.99). Always select dark regions spatially.

**Nit mapping.** Linear 1.0 (the reference exposure's clip point) → **1000 nits**,
via `cam::kPqScale` in `raw_develop.hh`, deliberately the same constant as the
video ISP's `RawVideoPipeline::kPqScale` so a still and a clip of the same scene
match. **Changing `kPqScale` invalidates the absolute levels of every file already
shot.**

**`hdr_merge` quantization.** `outWhite` used to be `min(65535, refSpan*maxBoost)` — a
clamp that silently discarded headroom on wide brackets. It now scales to fit, with
`MergeResult::dng_scale` carried so the quantization and the DNG agree. On the S23
Ultra's **10-bit** sensor (`white=1023`, `refSpan≈959`, ±2 EV → `fullSpan≈3836`)
`dng_scale` stays 1.0. `BaselineExposure` (50730) is emitted as `log2(maxBoost)`:
the merged plane keeps that many stops of headroom above the reference clip, so
without the tag every compliant developer renders the file exactly that many stops
dark — which is what made STATIC look dimmer than FAST.

**`hdr_merge` clipped-everywhere fallback.** A pixel clipped in *every* bracket
frame is at or above the brightest frame's ceiling, so it falls back to
`maxBoost` (the top of the merged range). It used to fall back to the *reference*
frame's value (~1.0) while a neighbour with even one valid frame was boosted to
`maxBoost` — a `maxBoost`-fold step at the edge of every blown highlight, per
channel. It also sat below the develop's highlight-reconstruction knee
(`0.95*maxBoost`), so that reconstruction never once fired for STATIC.
**Border-invalid pixels are a separate case** (`any_clipped` distinguishes them):
every frame out of range in the alignment-shift margin is *not* blown, and
sending those to `maxBoost` paints a white frame around the picture.

**The NR chip is hidden on the RAW path.** `ACAMERA_NOISE_REDUCTION_MODE` is a
control over the HAL's ISP stage, and RAW16 is by Camera2's definition the sensor
data from *before* that stage — so on a RAW device the chip cannot affect the DNG
whatever it is set to. It used to be drawn regardless (the old comment in `ui.hh`
even said "inert on RAW devices — the chip is drawn regardless"), which put a dead
control on screen on exactly the devices this app targets. `app.cc` now gates it
on `video_mode() != RAW_PQ`. It still does real work on the non-RAW path, which is
why it exists at all.

**PHOTO vs VIDEO is a session split** on the legacy/non-raw path: `preview + still` in PHOTO mode vs `preview + encoder` in VIDEO mode (`setPhotoMode` recreates the session on the UI toggle) — never preview+encoder+still together (not a guaranteed stream combo). RAW-video devices are unaffected (their session is preview+RAW16, and stills come from the same RAW stream).

**No "max resolution" / 50 MP.** A vendor-interpolated 50 MP mode was built and **removed**: budget Quad-Bayer-marketed sensors expose 8160×6144 only via a private ODM tag (`com.ontim.private.metadata.availableStreamInterpolaCfg`, i.e. *interpolated*, not real detail), and the standard `SCALER_STREAM_CONFIGURATION_MAP_MAXIMUM_RESOLUTION` is **null** without the `ULTRA_HIGH_RESOLUTION_SENSOR` capability — the NDK reads the same HAL metadata, so it's unreachable from "pure C++" too. The sensor's real binned max (~12.6 MP on that device) is the ceiling. Don't re-add it.

### Muxing, audio, UI

- **Muxer** (`cpp/muxer/`, libmatroska + libebml): writes `.mkv`. **The video track carries a `DefaultDuration`, patched at `close()` with the interval actually measured over the clip** — that is the field a player reports as the container frame rate. Without it ffmpeg *estimates* from the first few packets, and at ~245 Mbps the default ~5 MB probe covers about five frames, which is how a true 30.000 fps clip came to be reported as 41.8. **Only `write_video` may touch the `vid_first_ns`/`vid_last_ns`/`vid_frames` accounting.** Counting audio blocks into it too inflated the frame count (device-measured: 7154 video + 2982 FLAC = 10136) and made the file declare **42.5 fps for a genuine 30.0 fps stream** — the timestamps were right the whole time, only the divisor was wrong. `TS_SCALE` is **100 µs**, not the 1 ms it used to be: 33.3333 ms is not representable in whole milliseconds. Its hard bound is the **signed 16-bit** block timecode relative to the cluster, so `MAX_CLUSTER_NS / TS_SCALE` must stay under 32767 (a `static_assert` enforces it); 10 µs would be 100 000 and does **not** fit. A **single writer thread muxes both audio and video** (`Recorder::MuxPkt` queue) so disk I/O on cluster flushes never backpressures the encoder drain or the FLAC capture thread. **EBML-head gotcha:** the file's `kWriteDefault` is libebml's `WriteSkipDefault`, which omits any element whose value equals the libebml default — including the EBML **`DocType`** ("matroska" *is* its default). Rendered that way the file has doctype `(none)`: ffmpeg/VLC/mpv cope, but **strict players/editors then build no timeline**. The EBML head must therefore render with `kWriteAll` (`WriteAll`) so `DocType` is emitted; everything else stays `kWriteDefault`. The Segment is intentionally left **unknown-size with no final SeekHead** (streaming-valid); `KaxDuration` is patched in place at `close()`.
- **Audio** (`cpp/audio/audio_capture.cc`): USB DAC capture via `libusb` (from the `audio_engine` submodule), encoded inline to **FLAC** and muxed into the `.mkv` at the native rate; `aaudio` (built-in mic) is the fallback when no DAC is attached. **Source selection + FLAC init (`start()`):** `open_fd()` adopts the USB device's *highest* advertised rate/depth/channels ("max everything"), and `Recorder` falls back to the internal mic if a granted USB device exposes no capture stream (playback-only DAC). The muxer only adds the audio track when `is_capturing() && !codec_private().empty()`, and `codec_private()` is only filled once `FLAC__stream_encoder_init_stream` succeeds — so a **failed FLAC init silently drops the whole audio track** (and with it the `_ai.flac` side-car). libFLAC's *streamable subset* rejects sample rates **≥ 655360 Hz** (705.6/768 kHz high-end ADCs hit this), so `start()` retries init with `streamable_subset(false)` to keep the native raw rate (still lossless; FLAC max 1048575 Hz), after a sanity-guard on the negotiated format (rate>0, ch 1–8, bits 4–32 — also prevents a div-by-0 in the capture loop). It logs `FLAC encoder ready: …Hz …ch …bit (subset|non-subset)`. Sensor PTS vs audio clock domains are reconciled in the RAW pipeline (BOOTTIME→MONOTONIC rebase). **DeepFilterNet denoise** (`cpp/audio/df_net.{cc,hh}`, **device-verified**): a real DeepFilterNet3 speech denoiser on **ONNX Runtime, CPU EP, `ORT_ENABLE_ALL`** — the 3 bundled ONNX models (`assets/models/tmp/export/{enc,erb_dec,df_dec}.onnx`) driven by a hand-ported, libdf-exact front/back-end (vorbis-window STFT, width-based ERB, `erb_norm`/`unit_norm`, ERB mask + optional **post-filter** + 5-tap deep filter, ISTFT; `kiss_fft` for the transforms). Runs **offline over the whole clip in the `FINALIZING` phase** (the exported models have no GRU state I/O, so per-frame streaming is impossible), **chunked-with-warmup**, per-channel (**stereo preserved**). Output is a single **48 kHz/24-bit `<clip>_ai.flac` side-car** — DeepFilterNet **+ post-filter baked in** (`DfNet::process(buf, post_filter)`). **By design the `.mkv`'s own audio stays the untouched native-rate raw track** (the user wants the rawest video; the denoise is a convenience side-car, deliberately NOT muxed in), and **no loudness normalization** is applied (it would crush conversational dynamics). Measured on-device: noise floor −44→−63 dB (**≈+19 dB SNR**), voice level/dynamics preserved. If the model is missing/fails, `finalize_denoise()` falls back to the original audio per channel. **CPU EP only** — XNNPACK/NNAPI/QNN were tried and don't help (model is GRU-bound; the bundled `.so` does contain those providers). `tools/df_validate.py` is the desktop parity/quality **oracle** — it runs the real ONNX models via `libdf` and is the regression check for any front/back-end change. First-chunk `DfNet`-tagged stage diagnostics (input/feat/mask/coefs/output rms·min·max·nan) localize on-device issues. `rnnoise` remains an unused submodule. See `~/.claude/.../memory/deepfilternet-audio-impl.md`. **USB teardown gotcha:** `UsbAudioDriver` (in `audio_engine`) runs a *refcounted* libusb event thread; a **mid-stream device detach** can leak that refcount (`stopCapture()`'s "already torn down" early-return skips `releaseEventThread()`), leaving the thread spinning in `libusb_handle_events` — then `libusb_exit()` aborts destroying a still-held mutex (`pthread_mutex_destroy == 0` assertion) on the next **Stop**. `UsbAudioDriver::close()` force-zeroes the refcount and joins the thread before `libusb_exit`. Separately, a **silent USB-DAC recording is almost always the interface's input gain at zero**, not a bug — capture/encode/mux all work, the samples are just noise-floor.
- **UI** (`cpp/ui/ui.cc`): drawn with `Vk_Canvas_Lb_LAW` + `vulkan_font_engine` (MSDF fonts), composited over the camera preview (which freezes during RAW recording — see Lifecycle). The recording UI is the **shutter + a PHOTO/VIDEO switch**, the photo-mode chip, the still-NR chip (non-RAW only), and the **focus controls** — a manual/auto chip with a diopter scale, a **focus-check loupe** (2x/4x, refused while recording since the preview stream is held fixed), and **focus peaking** (off/normal/high). `ui::UI::Action` is `{NONE, TOGGLE_MODE, SHUTTER, CYCLE_PHOTO_MODE, TOGGLE_STILL_NR, TOGGLE_FOCUS_MODE, CYCLE_LOUPE, CYCLE_PEAKING}` (touch handling in `app.cc`), plus a **"Processing N%"** readout during the `FINALIZING` offline drain. Earlier builds exposed DN/DM/TD/CD RAW-pipeline A/B toggle buttons; those were **retired** once the best config was locked in as the default — the denoise/demosaic knobs now live only as `RawVideoPipeline` atomic defaults, not UI. The `Recorder::set_*`/`RawVideoPipeline::set_*` hooks remain for re-wiring (`set_temporal` is gone with the temporal pass).

## Libraries under `libs/` & the "improve libraries on the fly" rule

Libraries are organized into `libs/firstparty/` (minervarr repos) and `libs/thirdparty/` (external dependencies):

- **First-party submodules** (`libs/firstparty/`): `Vk_Canvas_Lb_LAW` (canvas + font engine, bundles FreeType + msdfgen), `archive_engine`, `audio_engine`, `regen_atlas`.
- **Third-party submodules** (`libs/thirdparty/`): `libebml`, `libmatroska`, `flac`. `libusb` comes via `audio_engine`'s `usb_audio.cpp`. `libjxl`, `ncnn` and `rnnoise` are still checked out as submodules but **none is built or linked** — `ncnn` went out with `AiDenoiser`, `libjxl` when stills became DNG-only (see **Capture-only**).
- **Vendored source (not submodules), also built by CMake:** `libs/thirdparty/kiss_fft` and `libs/thirdparty/soxr` (`CMakeLists.txt` ~lines 23–40).
- **Removed:** `cpp/camera/camera.cc/.hh` (`cam::Camera`) was a complete 495-line NDK Camera2 implementation that `Recorder` constructed and **never called**. `cpp/camera/ndk_session.{cc,hh}` supersedes it. The `cam` namespace still exists for `still_writer.cc` (`cam::write_png_yuv420`).
- **Prebuilt import (not compiled):** `libs/thirdparty/onnxruntime` — an `IMPORTED` `.so` per ABI (`CMakeLists.txt` ~lines 43–48).

Per `GUIDE.md`'s core principle: when this app needs a capability from `Vk_Canvas_Lb_LAW`, `audio_engine`, etc., **the fix belongs in that library's repo**, not in a workaround here, so every consuming project benefits. (So real bug fixes land *inside* `libs/firstparty/<submodule>/…` — e.g. the USB teardown fix above is in `audio_engine`, and the camera-preview compositing — `Renderer::update_camera_frame`/`clear_camera_frames`, AHardwareBuffer→Vulkan YCbCr import — lives in `Vk_Canvas_Lb_LAW`'s `renderer.cc`.) **Renderer-teardown gotcha:** `Renderer::cleanup_hwb_resources()` destroys the descriptor pool *and then* calls `clear_camera_frames()`, which `vkResetDescriptorPool`s it again → a Mali `pthread_mutex_destroy` abort on every background/surface-loss. Handles must be nulled right after `vkDestroy*` so the second pass skips them.

**Exception to note:** the `archive_engine` submodule's native API changed and no longer builds against this project, so its native usage was replaced by a local restored copy at `cpp/archive.cc` (`archive::get_documents_path`); only its C++ *headers* are still on the include path (CMake). Its **Java was unused and is no longer on the source path** (dropped when the camera became a library). `audio_engine`'s Java moved to the `:engine-audio` module. Don't assume the `archive_engine` submodule native code is in use.

## Repo hygiene notes

- `camera/.cxx/` (CMake/Ninja build cache) is committed and shows up churned in `git status` — generally not something to hand-edit; don't include its noise in feature commits.
- `tools/script.py` / `tools/script_writer.py` / `tools/patch_hwb_cache.py` are standalone Python dev/bundling helpers, not part of the Gradle build. `tools/build_model.py`, `tools/convert_dncnn.py`, `tools/convert_dncnn_bayer.py`, and `tools/dump_erb.py` are likewise standalone helpers for the AI work (model build/conversion and ERB-filterbank dump). `tools/df_validate.py` is the DeepFilterNet audio **parity/quality oracle** (needs the `.venv_313` venv with `onnxruntime`/`libdf`/`soundfile`); `tmp_df_golden/` holds its scratch/golden fixtures (dev-only, ignorable).
- The old release `signingConfig` (checked-in keystore password) was **removed** when `app/` became the `:camera` library — the `.aar` is unsigned (the host app signs), and `:demo` uses debug signing. `bruno.jks` remains at the repo root but is no longer referenced by any `build.gradle`. Do not reintroduce the password into new files or logs.
