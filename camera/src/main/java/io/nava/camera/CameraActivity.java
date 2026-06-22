package io.nava.camera;

import android.app.NativeActivity;
import android.content.Intent;

import java.io.File;

/**
 * The embeddable camera scene. A {@link NativeActivity} whose C++ side owns the
 * Vulkan UI and capture pipeline; this subclass adds the host integration:
 * launch options (output dir, initial mode) and a result handed back when the
 * user backs out.
 *
 * <p>Launch it via {@link CameraLauncher}; do not reference it directly.
 */
public class CameraActivity extends NativeActivity {

    // A NativeActivity loads the native lib via the manifest android.app.lib_name,
    // but that path does NOT register explicitly-declared native methods or make
    // the JNI up-calls below resolvable from native — so load it here too.
    // (See vulkan_canvas_engine/USAGE.md for the full gotcha.)
    static {
        System.loadLibrary("camera_recorder");
    }

    // ── JNI down-calls: native reads launch config from these (by name) ──────────

    /** Output directory for captures (created if needed). Native appends filenames. */
    @SuppressWarnings("unused") // called from native via JNI
    public String cameraOutputDir() {
        String custom = getIntent() != null
                ? getIntent().getStringExtra(CameraLauncher.EXTRA_OUTPUT_DIR) : null;
        File dir = (custom != null && !custom.isEmpty())
                ? new File(custom)
                : new File(getExternalFilesDir(null), "camera");
        //noinspection ResultOfMethodCallIgnored
        dir.mkdirs();
        return dir.getAbsolutePath();
    }

    /** Whether to open directly in VIDEO mode (the toggle still works either way). */
    @SuppressWarnings("unused") // called from native via JNI
    public boolean cameraStartVideo() {
        return getIntent() != null
                && getIntent().getBooleanExtra(CameraLauncher.EXTRA_START_VIDEO, false);
    }

    /** Initial RAW photo output mode (0=bracket, 1=merged, 2=HDR image). */
    @SuppressWarnings("unused") // called from native via JNI
    public int cameraPhotoMode() {
        return getIntent() != null
                ? getIntent().getIntExtra(CameraLauncher.EXTRA_PHOTO_MODE,
                                          CameraLauncher.PHOTO_MODE_RAW_BRACKET)
                : CameraLauncher.PHOTO_MODE_RAW_BRACKET;
    }

    /** Whether the in-scene photo-mode selector is shown (default true). */
    @SuppressWarnings("unused") // called from native via JNI
    public boolean cameraPhotoModeUiEnabled() {
        return getIntent() == null
                || getIntent().getBooleanExtra(CameraLauncher.EXTRA_PHOTO_MODE_UI, true);
    }

    // ── JNI up-call: native ends the session and returns the captured files ──────

    /**
     * Invoked from native once the session has ended (back pressed and any video
     * finalize completed). Returns the captured file paths to the host and
     * finishes the activity.
     */
    @SuppressWarnings("unused") // called from native via JNI
    public void finishWithResults(final String[] paths) {
        runOnUiThread(() -> {
            final String[] files = (paths != null) ? paths : new String[0];
            Intent data = new Intent();
            data.putExtra(CameraLauncher.EXTRA_RESULT_FILES, files);
            setResult(files.length > 0 ? RESULT_OK : RESULT_CANCELED, data);
            finish();
        });
    }
}
