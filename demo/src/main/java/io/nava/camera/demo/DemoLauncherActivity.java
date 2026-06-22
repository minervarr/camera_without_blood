package io.nava.camera.demo;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.os.Environment;
import android.util.Log;
import android.view.Gravity;
import android.widget.Button;
import android.widget.LinearLayout;
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

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setGravity(Gravity.CENTER);

        // One button per photo output mode (the in-scene chip stays enabled so the
        // mode can also be changed inside the camera).
        addModeButton(root, "Camera — FAST HDR (1 DNG, default)", CameraLauncher.PHOTO_MODE_FAST_HDR);
        addModeButton(root, "Camera — STATIC HDR (merged DNG)",   CameraLauncher.PHOTO_MODE_STATIC_HDR);
        addModeButton(root, "Camera — STATIC HDR + RAW set",      CameraLauncher.PHOTO_MODE_STATIC_HDR_RAW);

        setContentView(root);
    }

    // Save into the public Documents/camera_without_blood folder, matching the old
    // standalone camera app. Works because this demo targets SDK 28 (legacy
    // storage) and the camera requests WRITE_EXTERNAL_STORAGE.
    private String outputDir() {
        return new File(Environment.getExternalStoragePublicDirectory(
                Environment.DIRECTORY_DOCUMENTS), "camera_without_blood").getAbsolutePath();
    }

    private void addModeButton(LinearLayout root, String label, int photoMode) {
        Button b = new Button(this);
        b.setText(label);
        b.setOnClickListener(v -> startActivityForResult(
                CameraLauncher.createIntent(this, outputDir(), /*startVideo=*/false,
                                            photoMode, /*photoModeUi=*/true),
                REQ_CAMERA));
        root.addView(b);
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != REQ_CAMERA) return;

        String[] files = CameraLauncher.parseResult(resultCode, data);
        Toast.makeText(this, "Captured " + files.length + " file(s)", Toast.LENGTH_LONG).show();
        for (String f : files) Log.i(TAG, "result file: " + f);
    }
}
