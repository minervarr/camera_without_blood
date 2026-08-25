package io.nava.camera.demo;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.os.Environment;
import android.util.Log;
import android.widget.Toast;

import java.io.File;

import io.nava.camera.CameraLauncher;

/**
 * Minimal reference host: a single button that opens the embeddable camera scene
 * and logs/toasts the files it returns when the user backs out. Uses the plain
 * {@link Activity#startActivityForResult} flow (no androidx) — see the camera
 * library README for the idiomatic androidx ActivityResultContract wrapper.
 */
public class DemoLauncherActivity extends Activity {

    private static final String TAG = "CameraDemo";
    private static final int REQ_CAMERA = 1001;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        // Skip the launcher UI entirely — go straight to the camera with
        // default options (PHOTO mode, FAST HDR, photo-mode chip enabled).
        startActivityForResult(
                CameraLauncher.createIntent(this, outputDir(), /*startVideo=*/false,
                                            CameraLauncher.PHOTO_MODE_FAST_HDR,
                                            /*photoModeUi=*/true),
                REQ_CAMERA);
    }

    // Save into the public Documents/camera_without_blood folder, matching the old
    // standalone camera app. Works because this demo targets SDK 28 (legacy
    // storage) and the camera requests WRITE_EXTERNAL_STORAGE.
    private String outputDir() {
        return new File(Environment.getExternalStoragePublicDirectory(
                Environment.DIRECTORY_DOCUMENTS), "camera_without_blood").getAbsolutePath();
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != REQ_CAMERA) return;

        String[] files = CameraLauncher.parseResult(resultCode, data);
        Toast.makeText(this, "Captured " + files.length + " file(s)", Toast.LENGTH_LONG).show();
        for (String f : files) Log.i(TAG, "result file: " + f);
        // This host has no UI of its own: don't leave an empty white window
        // behind the camera scene — leaving the camera leaves the app.
        finish();
    }
}
