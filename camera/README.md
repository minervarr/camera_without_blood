# `:camera` — embeddable full-screen camera scene

This module packages the whole camera (Vulkan UI, PHOTO + VIDEO, the PHOTO/VIDEO
toggle) as an Android **library** (`.aar`). A host app launches it as a
full-screen scene; when the user backs out, the host receives the files captured
during that session.

## Use it from another app

```kotlin
// 1. Open the camera scene (plain Activity Result API, no androidx required):
startActivityForResult(CameraLauncher.createIntent(this), REQ_CAMERA)

// optional: choose an output dir / start in video mode
startActivityForResult(
    CameraLauncher.createIntent(this, /*outputDir=*/null, /*startVideo=*/false),
    REQ_CAMERA)

// 2. Read back the captured files:
override fun onActivityResult(req: Int, res: Int, data: Intent?) {
    if (req == REQ_CAMERA) {
        val files: Array<String> = CameraLauncher.parseResult(res, data) // absolute paths
    }
}
```

The user shoots photos/videos with the full camera UI, then presses **Back** to
return. Photos save immediately; a video runs an offline finalize first (the
scene shows "Processing N%") and is returned once complete.

### androidx `ActivityResultContract` (optional)

The library adds no androidx dependency. If your host uses androidx, wrap the
helper in a few lines:

```kotlin
class CameraContract : ActivityResultContract<Unit, Array<String>>() {
    override fun createIntent(ctx: Context, input: Unit) = CameraLauncher.createIntent(ctx)
    override fun parseResult(resultCode: Int, intent: Intent?) =
        CameraLauncher.parseResult(resultCode, intent)
}
// val cam = registerForActivityResult(CameraContract()) { files -> ... }
// cam.launch(Unit)
```

## Output location

Captures go to the host's **app-specific** external dir
(`getExternalFilesDir(null)/camera`) by default — scoped-storage-safe, so it
works regardless of the host's `targetSdk` and needs no storage permission. Pass
`CameraLauncher.EXTRA_OUTPUT_DIR` to override.

## Sharing engines across apps (avoiding duplicate classes)

All the heavy native engines (vulkan_canvas_engine, ncnn, FreeType, libusb,
flac, matroska, ONNX Runtime, …) are compiled into a single
`libcamera_recorder.so` and sealed inside it — a host that also uses them
natively keeps its own copy with **no conflict**.

The **only** shared *Java* is `audio_engine` (`com.nerio.audioengine`), exposed
as its own coordinate via the **`:engine-audio`** module (a transitive
dependency of this library). If your host app *also* uses `audio_engine`:

- Depend on the **same `:engine-audio` coordinate** — Gradle keeps one copy, or
- `exclude` it from the camera dependency and provide your own:
  ```groovy
  implementation(project(':camera')) { exclude group: 'com.nerio.audioengine' }
  ```

(`archive_engine`'s Java is intentionally not bundled — it was unused.)

## Constraints

- **arm64-v8a only.** The host app must ship `arm64-v8a`.
- Large native footprint (the whole camera stack) is inherited by the host.
- Long video clips take a few seconds to return after Back (offline finalize).

## Standalone / reference app

The `:demo` module is a thin launcher (one button) that opens this scene and
logs the returned files — also the build used for on-device verification.
