package io.nava.camera;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;

/**
 * Zero-dependency entry point for launching the embeddable camera scene and
 * reading back the files captured during a session.
 *
 * <p>Plain {@code startActivityForResult} flow:
 * <pre>{@code
 * startActivityForResult(CameraLauncher.createIntent(context), REQ_CAMERA);
 * // ...
 * String[] files = CameraLauncher.parseResult(resultCode, data);
 * }</pre>
 *
 * <p>androidx hosts can wrap this in an {@code ActivityResultContract} in a few
 * lines (see the library README) — this class deliberately adds no androidx
 * dependency so the camera {@code .aar} stays minimal.
 */
public final class CameraLauncher {

    /** Optional {@link String} extra: absolute directory to write captures into.
     *  Defaults to {@code getExternalFilesDir(null)/camera} (scoped-storage-safe). */
    public static final String EXTRA_OUTPUT_DIR = "io.nava.camera.OUTPUT_DIR";

    /** Optional {@code boolean} extra: start in VIDEO mode (default: PHOTO). The
     *  PHOTO/VIDEO toggle remains available either way. */
    public static final String EXTRA_START_VIDEO = "io.nava.camera.START_VIDEO";

    /** Optional {@code int} extra: RAW photo output mode (see PHOTO_MODE_* below;
     *  default {@link #PHOTO_MODE_RAW_BRACKET}). RAW-sensor devices only — non-RAW
     *  devices always save a single image. */
    public static final String EXTRA_PHOTO_MODE = "io.nava.camera.PHOTO_MODE";

    /** Optional {@code boolean} extra: show the in-scene photo-mode selector
     *  (default true). Set false to lock the mode to {@link #EXTRA_PHOTO_MODE}. */
    public static final String EXTRA_PHOTO_MODE_UI = "io.nava.camera.PHOTO_MODE_UI";

    /** 3 source DNGs + the merged HDR DNG (4 files). */
    public static final int PHOTO_MODE_RAW_BRACKET = 0;
    /** Only the merged HDR DNG (1 file). */
    public static final int PHOTO_MODE_RAW_MERGED  = 1;
    /** Only a developed scene-referred HDR image (Radiance .hdr, 1 file). */
    public static final int PHOTO_MODE_HDR_IMAGE   = 2;

    /** Result {@code String[]} extra: absolute paths of the files captured. */
    public static final String EXTRA_RESULT_FILES = "io.nava.camera.RESULT_FILES";

    private CameraLauncher() {}

    /** Intent that opens the camera scene with default options. */
    public static Intent createIntent(Context ctx) {
        return new Intent(ctx, CameraActivity.class);
    }

    /**
     * Intent that opens the camera scene.
     *
     * @param outputDir absolute output directory, or {@code null} for the default
     * @param startVideo {@code true} to open in VIDEO mode
     */
    public static Intent createIntent(Context ctx, String outputDir, boolean startVideo) {
        Intent i = new Intent(ctx, CameraActivity.class);
        if (outputDir != null && !outputDir.isEmpty()) i.putExtra(EXTRA_OUTPUT_DIR, outputDir);
        i.putExtra(EXTRA_START_VIDEO, startVideo);
        return i;
    }

    /**
     * Intent that opens the camera scene with a chosen RAW photo output mode.
     *
     * @param photoMode one of PHOTO_MODE_* (RAW-sensor devices)
     * @param photoModeUi whether the in-scene photo-mode selector is shown
     */
    public static Intent createIntent(Context ctx, String outputDir, boolean startVideo,
                                      int photoMode, boolean photoModeUi) {
        Intent i = createIntent(ctx, outputDir, startVideo);
        i.putExtra(EXTRA_PHOTO_MODE, photoMode);
        i.putExtra(EXTRA_PHOTO_MODE_UI, photoModeUi);
        return i;
    }

    /**
     * Extracts the captured file paths from an activity result. Returns an empty
     * array if the session was cancelled or produced nothing.
     */
    public static String[] parseResult(int resultCode, Intent data) {
        if (resultCode != Activity.RESULT_OK || data == null) return new String[0];
        String[] files = data.getStringArrayExtra(EXTRA_RESULT_FILES);
        return files != null ? files : new String[0];
    }
}
