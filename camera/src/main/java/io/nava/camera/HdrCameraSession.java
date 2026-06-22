package io.nava.camera;

import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.graphics.ImageFormat;
import android.graphics.SurfaceTexture;
import android.hardware.HardwareBuffer;
import android.hardware.usb.UsbConstants;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbDeviceConnection;
import android.hardware.usb.UsbManager;
import android.hardware.camera2.CameraAccessException;
import android.hardware.camera2.CameraCaptureSession;
import android.hardware.camera2.CameraCharacteristics;
import android.hardware.camera2.CameraDevice;
import android.hardware.camera2.CameraManager;
import android.hardware.camera2.CaptureRequest;
import android.hardware.camera2.CaptureResult;
import android.hardware.camera2.TotalCaptureResult;
import android.hardware.camera2.params.DynamicRangeProfiles;
import android.hardware.camera2.params.OutputConfiguration;
import android.hardware.camera2.params.SessionConfiguration;
import android.hardware.camera2.params.StreamConfigurationMap;
import android.media.Image;
import android.media.ImageReader;
import android.media.MediaCodec;
import android.media.MediaCodecInfo;
import android.media.MediaFormat;
import android.os.Build;
import android.os.Handler;
import android.os.HandlerThread;
import android.util.Log;
import android.util.Range;
import android.util.Rational;
import android.util.Size;
import android.view.Surface;

import java.nio.ByteBuffer;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.concurrent.ConcurrentLinkedQueue;
import java.util.concurrent.Executor;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/**
 * Owns the Camera2 capture session. Exists because 10-bit HDR (HLG10) can only
 * be enabled via OutputConfiguration.setDynamicRangeProfile, which has no NDK
 * equivalent. Delivers preview frames to native as HardwareBuffers and, while
 * recording, drives an HEVC Main10 HLG encoder whose output is forwarded to the
 * native muxer.
 */
public class HdrCameraSession {
    private static final String TAG = "HdrCamera";

    static {
        // NativeActivity dlopen's the native library itself, which does NOT
        // register it with this class's ClassLoader for JNI resolution. Loading
        // it here (a no-op load of the already-mapped lib) registers its symbols
        // so our native methods resolve. Without this: UnsatisfiedLinkError.
        System.loadLibrary("camera_recorder");
    }

    private final long nativeCtx;
    private final Context appCtx;
    private final CameraManager manager;

    // USB DAC access
    private static final String ACTION_USB_PERMISSION = "io.nava.camera.USB_PERMISSION";
    private UsbManager usbManager;
    private UsbDeviceConnection usbConn;   // held open so the fd stays valid
    private BroadcastReceiver usbReceiver;

    private HandlerThread bgThread;
    private Handler bgHandler;
    // Dedicated thread for the RAW reader so its per-frame 24MB staging copy
    // doesn't jostle preview delivery (both used to share bgHandler).
    private HandlerThread rawThread;
    private Handler rawHandler;

    private CameraDevice device;
    private CameraCaptureSession session;
    // Bumped on every (re)configure; async session callbacks compare against it
    // so a stale onConfigured firing after a pause/resume bounce is ignored
    // instead of touching a closed CameraDevice (which throws IllegalState).
    private int sessionGen = 0;
    private ImageReader previewReader;
    private String cameraId;
    private boolean hdrSupported;

    private int previewWidth, previewHeight;

    // AE target frame-rate range, locked so auto-exposure can't lower the frame
    // rate for brightness (which otherwise sags the camera below 30fps in dim
    // light). Chosen at startPreview from the device's supported ranges.
    private Range<Integer> aeFpsRange;

    // Recording
    private MediaCodec encoder;
    private Surface persistentInputSurface;  // created once in startPreview; outlives encoder instances
    private boolean inputSurfaceSized;       // true once an encoder configure has given the surface dims
    private Thread drainThread;
    private volatile boolean draining;
    private volatile boolean recording;
    private boolean sentFormat;
    private int recordWidth, recordHeight, recordBitrate;

    // RAW still (bracketed)
    private ImageReader rawReader;
    private int rawWidth, rawHeight;
    private CameraCharacteristics characteristics;
    private String photoBase;
    // RAW frames the still-photo path still expects to acquire for the active
    // burst. 0 = no burst → onRawImage discards stray frames (e.g. those that
    // arrive in-flight just after recording stops) instead of leaking the
    // ImageReader's buffers. Guarded by rawLock.
    private int photoFramesWanted = 0;
    // Stable size of the current still burst (1 = FAST single shot, 3 = STATIC
    // bracket). Unlike photoFramesWanted (which counts down), this stays put so the
    // native side learns the burst size and the single-shot file gets a clean name.
    private int photoBurstSize = BRACKET_COUNT;
    private final Object rawLock = new Object();
    private final HashMap<Long, Image> pendingImages = new HashMap<>();
    private final HashMap<Long, TotalCaptureResult> pendingResults = new HashMap<>();
    private final HashMap<Long, Integer> pendingIndex = new HashMap<>();
    private static final int BRACKET_COUNT = 3;
    private static final int MAX_VIDEO_WIDTH = 100000;  // effectively uncapped (max sensor res)

    // RAW video mode: the session is preview + streaming RAW16 and recording
    // delivers Bayer frames to the native ISP/encoder. No MediaCodec, no
    // DynamicRangeProfiles — native owns the entire pipeline. Selected by
    // native before startPreview.
    private volatile boolean rawVideoMode;
    private volatile boolean rawRecording;

    // Non-RAW still fallback: on devices without a RAW_SENSOR stream, the photo
    // path captures a full-resolution YUV_420_888 frame and saves it as a lossless
    // PNG (the rawest, highest-quality image the ISP will hand us).
    private ImageReader stillReader;
    private int stillWidth, stillHeight;
    private int sensorOrientation;
    // The PNG encode is slow (~seconds for 12 MP); run it off the camera reader
    // thread so back-to-back shots never block image delivery. A FIFO of output
    // names pairs each delivered frame with the shot that requested it.
    private ExecutorService stillEncoder;
    private final ConcurrentLinkedQueue<String> stillNames = new ConcurrentLinkedQueue<>();
    // Which still stream is wired into the *current* session, so takePhoto knows
    // what to capture (decoupled from inputSurfaceSized / mode flags).
    private static final int STILL_NONE = 0, STILL_RAW = 1, STILL_YUV = 2;
    private volatile int activeStill = STILL_NONE;
    // PHOTO vs VIDEO session for the legacy (non-rawVideo) path. PHOTO = preview +
    // still stream, VIDEO = preview + encoder. Default PHOTO matches the UI default;
    // toggling recreates the session. Ignored while rawVideoMode owns the session.
    private volatile boolean photoMode = true;

    public HdrCameraSession(Context ctx, long nativeCtx) {
        this.nativeCtx = nativeCtx;
        this.appCtx = ctx;
        this.manager = (CameraManager) ctx.getSystemService(Context.CAMERA_SERVICE);
        this.usbManager = (UsbManager) ctx.getSystemService(Context.USB_SERVICE);
    }

    // ── Native callbacks ─────────────────────────────────────────────────────
    private static native void nativeOnPreviewBuffer(long ctx, HardwareBuffer buffer, Image image);
    private static native void nativeOnVideoFormat(long ctx, byte[] csd, int width, int height);
    private static native void nativeOnVideoPacket(long ctx, byte[] data, int size, long ptsUs, boolean keyframe);
    private static native void nativeOnRawFrame(long ctx, String path, ByteBuffer data,
            int width, int height, int rowStride, float[] neutral, float[] black, int white,
            long exposureNs, int iso, int index, int count);
    private static native void nativeOnRawVideoFrame(ByteBuffer data,
            int width, int height, int rowStride, long timestampNs);
    private static native void nativeOnRawVideoNeutral(float[] neutral);
    private static native void nativeOnUsbFd(int fd);
    // Full-res YUV_420_888 still (non-RAW devices) -> lossless PNG, written natively.
    private static native void nativeOnStillFrame(String path, byte[] y, byte[] u, byte[] v,
            int width, int height, int yStride, int uStride, int vStride,
            int uvPixelStride, int orientationDeg);

    // ── USB DAC permission + open ────────────────────────────────────────────

    /** Finds the attached USB audio device and obtains a libusb fd for it,
     *  prompting the user for USB permission if needed. Async: the fd is
     *  reported to native via nativeOnUsbFd once available. */
    public void requestUsbAccess() {
        if (usbManager == null) return;
        UsbDevice dac = findAudioDevice();
        if (dac == null) { Log.i(TAG, "No USB audio device attached"); return; }

        if (usbManager.hasPermission(dac)) {
            openUsb(dac);
            return;
        }
        if (usbReceiver == null) {
            usbReceiver = new BroadcastReceiver() {
                @Override public void onReceive(Context c, Intent intent) {
                    if (!ACTION_USB_PERMISSION.equals(intent.getAction())) return;
                    UsbDevice dev = intent.getParcelableExtra(UsbManager.EXTRA_DEVICE);
                    if (dev != null && intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false)) {
                        openUsb(dev);
                    } else {
                        Log.e(TAG, "USB permission denied");
                    }
                }
            };
            IntentFilter filter = new IntentFilter(ACTION_USB_PERMISSION);
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                appCtx.registerReceiver(usbReceiver, filter, Context.RECEIVER_NOT_EXPORTED);
            } else {
                appCtx.registerReceiver(usbReceiver, filter);
            }
        }
        int flags = PendingIntent.FLAG_IMMUTABLE;
        Intent intent = new Intent(ACTION_USB_PERMISSION).setPackage(appCtx.getPackageName());
        PendingIntent pi = PendingIntent.getBroadcast(appCtx, 0, intent, flags);
        usbManager.requestPermission(dac, pi);
        Log.i(TAG, "Requested USB permission for " + dac.getDeviceName());
    }

    private UsbDevice findAudioDevice() {
        for (UsbDevice d : usbManager.getDeviceList().values()) {
            for (int i = 0; i < d.getInterfaceCount(); i++) {
                if (d.getInterface(i).getInterfaceClass() == UsbConstants.USB_CLASS_AUDIO) return d;
            }
        }
        return null;
    }

    private void openUsb(UsbDevice dev) {
        try {
            UsbDeviceConnection conn = usbManager.openDevice(dev);
            if (conn == null) { Log.e(TAG, "openDevice returned null"); return; }
            usbConn = conn;                       // keep alive — fd is owned by it
            int fd = conn.getFileDescriptor();
            Log.i(TAG, "USB DAC opened, fd=" + fd);
            nativeOnUsbFd(fd);
        } catch (Exception e) {
            Log.e(TAG, "openUsb failed", e);
        }
    }

    // ── Control (called from native) ─────────────────────────────────────────

    /** Must be called before startPreview. true = stream RAW16 to the native
     *  ISP for video; false = legacy HLG10 MediaCodec path. */
    public void setRawVideoMode(boolean enabled) {
        rawVideoMode = enabled;
        Log.i(TAG, "raw video mode: " + enabled);
    }

    /** PHOTO vs VIDEO session selection for the legacy (non-rawVideo) path. PHOTO
     *  brings up preview + the still stream, VIDEO brings up preview + the encoder;
     *  switching recreates the session. No-op for RAW-video devices and while
     *  recording (the session must keep the encoder then). */
    public synchronized void setPhotoMode(boolean photo) {
        if (rawVideoMode || photoMode == photo) return;
        photoMode = photo;
        Log.i(TAG, "photo mode: " + photo);
        if (recording) return;
        if (device != null && session != null) {
            try { recreateSession(); } catch (Exception e) { Log.e(TAG, "setPhotoMode", e); }
        }
    }

    // Creates the still ImageReader at the largest YUV size the sensor exposes to
    // the standard API (incl. the high-res set). This device's true sensor max is
    // ~12.6 MP — the "50 MP" the vendor advertises is interpolated/upscaled and not
    // reachable through Camera2, so there's nothing higher to use here.
    private void setupStillReader() {
        if (stillReader != null) { stillReader.close(); stillReader = null; }
        StreamConfigurationMap scm =
                characteristics.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP);
        ArrayList<Size> yuv = new ArrayList<>();
        if (scm != null) {
            Size[] a = scm.getOutputSizes(ImageFormat.YUV_420_888);
            if (a != null) java.util.Collections.addAll(yuv, a);
            Size[] hi = scm.getHighResolutionOutputSizes(ImageFormat.YUV_420_888);
            if (hi != null) java.util.Collections.addAll(yuv, hi);
        }
        Size best = null;
        for (Size s : yuv)
            if (best == null || (long) s.getWidth() * s.getHeight()
                    > (long) best.getWidth() * best.getHeight()) best = s;
        if (best == null) { Log.e(TAG, "no YUV_420_888 sizes for still capture"); return; }
        stillWidth = best.getWidth();
        stillHeight = best.getHeight();
        stillReader = ImageReader.newInstance(stillWidth, stillHeight, ImageFormat.YUV_420_888, 3);
        stillReader.setOnImageAvailableListener(this::onStillImage, rawHandler);
        if (stillEncoder == null) stillEncoder = Executors.newSingleThreadExecutor();
        Log.i(TAG, "still YUV reader " + stillWidth + "x" + stillHeight);
    }

    public synchronized void startPreview() {
        // Bring up the foreground service FIRST (while we're still foreground), so
        // Android keeps the camera legal once the app is backgrounded / screen off.
        startCameraService();
        try {
            bgThread = new HandlerThread("HdrCamera");
            bgThread.start();
            bgHandler = new Handler(bgThread.getLooper());

            rawThread = new HandlerThread("HdrCameraRaw");
            rawThread.start();
            rawHandler = new Handler(rawThread.getLooper());

            cameraId = pickBackCamera();
            if (cameraId == null) { Log.e(TAG, "No back camera"); return; }

            characteristics = manager.getCameraCharacteristics(cameraId);
            hdrSupported = supportsHlg10(characteristics);
            Log.i(TAG, "HLG10 supported: " + hdrSupported);
            Integer so = characteristics.get(CameraCharacteristics.SENSOR_ORIENTATION);
            sensorOrientation = so != null ? so : 0;

            // Use the sensor's native aspect ratio — taken from the active pixel
            // ARRAY, not from getOutputSizes(): the framework drops sub-rate
            // "high-resolution" sizes from that list, so on some sensors its
            // largest-area entry is a square crop (which made video record 1:1).
            // The active array is the true full-FOV aspect, so nothing is cropped.
            // Video records the largest size the HEVC encoder supports at that
            // aspect; preview is a small ~720-wide reference at the same aspect.
            Size[] sizes = streamSizes(characteristics);
            double aspect = sensorAspect(characteristics, sizes);
            // Max-resolution 4:3 the encoder supports (resolution was ruled out as
            // the preview-stutter cause; the fix is pinning the display frame rate).
            Size vd = pickLargestEncodable(sizes, aspect, MAX_VIDEO_WIDTH);
            recordWidth = vd.getWidth();
            recordHeight = vd.getHeight();
            Size pv = pickAspectNearWidth(sizes, aspect, 720);
            previewWidth = pv.getWidth();
            previewHeight = pv.getHeight();
            Log.i(TAG, String.format("aspect %.3f  preview %dx%d  video %dx%d",
                       aspect, previewWidth, previewHeight, recordWidth, recordHeight));

            previewReader = ImageReader.newInstance(
                    previewWidth, previewHeight, ImageFormat.PRIVATE, 4,
                    HardwareBuffer.USAGE_GPU_SAMPLED_IMAGE);
            previewReader.setOnImageAvailableListener(this::onPreviewImage, bgHandler);

            // RAW_SENSOR reader for bracketed stills (largest size). Optional —
            // absent on devices without RAW.
            StreamConfigurationMap scm =
                    characteristics.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP);
            Size[] rawSizes = scm != null ? scm.getOutputSizes(ImageFormat.RAW_SENSOR) : null;
            if (rawSizes != null && rawSizes.length > 0) {
                Size best = rawSizes[0];
                for (Size s : rawSizes)
                    if ((long) s.getWidth() * s.getHeight() > (long) best.getWidth() * best.getHeight()) best = s;
                rawWidth = best.getWidth();
                rawHeight = best.getHeight();
                // Streaming video needs more in-flight buffers than a still burst.
                int maxImages = rawVideoMode ? 6 : BRACKET_COUNT + 1;
                rawReader = ImageReader.newInstance(rawWidth, rawHeight, ImageFormat.RAW_SENSOR, maxImages);
                rawReader.setOnImageAvailableListener(this::onRawImage, rawHandler);
                Log.i(TAG, "RAW reader " + rawWidth + "x" + rawHeight + " (max " + maxImages + ")");
                // Diagnostic: the hardware ceiling for full-res RAW. If this is
                // < 30, no AE lock can reach 30fps and we'd need a smaller RAW.
                long minDur = scm.getOutputMinFrameDuration(ImageFormat.RAW_SENSOR, best);
                Log.i(TAG, String.format("RAW max fps (hw): %.1f  (min frame duration %.2f ms)",
                           minDur > 0 ? 1e9 / minDur : 0.0, minDur / 1e6));
                // Diagnostic: enumerate EVERY RAW mode + its readout ceiling, to
                // see whether any smaller/binned RAW beats 33.3 ms (i.e. whether
                // 4K60 RAW is even physically possible — full-res is 30fps-locked).
                for (Size s : rawSizes) {
                    long d = scm.getOutputMinFrameDuration(ImageFormat.RAW_SENSOR, s);
                    Log.i(TAG, String.format("  RAW mode %dx%d  minDur %.2f ms -> %.1f fps max",
                               s.getWidth(), s.getHeight(), d / 1e6, d > 0 ? 1e9 / d : 0.0));
                }
                // High-speed (CONSTRAINED_HIGH_SPEED) is a YUV path, not RAW, but
                // shows whether any high-fps capture exists on this sensor at all.
                try {
                    for (Size s : scm.getHighSpeedVideoSizes())
                        Log.i(TAG, "  high-speed YUV " + s + " fps " +
                               java.util.Arrays.toString(scm.getHighSpeedVideoFpsRangesFor(s)));
                } catch (Exception e) { Log.i(TAG, "  high-speed: none (" + e + ")"); }
            }

            // No RAW on this device → set up a YUV_420_888 still stream for lossless
            // PNG photos (binned 12 MP by default, or full-res 50 MP if selected).
            if (rawReader == null && !rawVideoMode) {
                setupStillReader();
            }

            // Lock AE to a 30fps-capable range so exposure can't float the frame
            // rate down. Prefer fixed [30,30]; else highest fixed; else widest.
            aeFpsRange = pickAeFpsRange(characteristics);
            Log.i(TAG, "AE target fps range: " + aeFpsRange);

            // Create the persistent encoder surface once. It is added to the camera
            // session permanently so recording start/stop never needs recreateSession().
            // A freshly created persistent surface has NO dimensions until an encoder
            // is configured on it — and the camera session can't be configured with an
            // unsized surface. So configure the encoder now (Configured state, not
            // started) to size the surface before openCamera builds the session.
            // RAW video mode needs none of this: the session is preview + RAW16
            // and the native ISP/encoder consumes the Bayer frames directly.
            if (!rawVideoMode) {
                if (persistentInputSurface == null) {
                    persistentInputSurface = MediaCodec.createPersistentInputSurface();
                }
                if (encoder == null) configureEncoder();
            }

            manager.openCamera(cameraId, deviceCallback, bgHandler);
            Log.i(TAG, "startPreview  " + previewWidth + "x" + previewHeight
                       + "  video " + recordWidth + "x" + recordHeight);
        } catch (CameraAccessException | SecurityException e) {
            Log.e(TAG, "startPreview failed", e);
        }
    }

    public synchronized void stopPreview() {
        sessionGen++;   // invalidate any in-flight session-configured callbacks
        try {
            stopRecording();
            if (session != null) { session.close(); session = null; }
            if (device != null) { device.close(); device = null; }
            if (previewReader != null) { previewReader.close(); previewReader = null; }
            if (rawReader != null) { rawReader.close(); rawReader = null; }
            if (stillReader != null) { stillReader.close(); stillReader = null; }
            // shutdown() (not shutdownNow) lets already-queued PNG encodes finish —
            // its worker thread keeps the process alive until they're written.
            if (stillEncoder != null) { stillEncoder.shutdown(); stillEncoder = null; }
            stillNames.clear();
            activeStill = STILL_NONE;
            releaseEncoder();   // drops the configured-but-never-started encoder, if any
            if (persistentInputSurface != null) { persistentInputSurface.release(); persistentInputSurface = null; }
            inputSurfaceSized = false;
        } catch (Exception e) {
            Log.e(TAG, "stopPreview", e);
        } finally {
            if (bgThread != null) { bgThread.quitSafely(); bgThread = null; bgHandler = null; }
            if (rawThread != null) { rawThread.quitSafely(); rawThread = null; rawHandler = null; }
            if (usbReceiver != null) {
                try { appCtx.unregisterReceiver(usbReceiver); } catch (Exception ignored) {}
                usbReceiver = null;
            }
            if (usbConn != null) { try { usbConn.close(); } catch (Exception ignored) {} usbConn = null; }
            nativeOnUsbFd(0);
            stopCameraService();   // camera fully closed — drop the foreground service + wake lock
        }
    }

    // ── Foreground service control (keeps the camera alive in the background) ──
    private void startCameraService() {
        try {
            Intent i = new Intent(appCtx, RecordingService.class);
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                appCtx.startForegroundService(i);
            } else {
                appCtx.startService(i);
            }
        } catch (Exception e) {
            Log.e(TAG, "startCameraService", e);
        }
    }

    private void stopCameraService() {
        try {
            appCtx.stopService(new Intent(appCtx, RecordingService.class));
        } catch (Exception e) {
            Log.e(TAG, "stopCameraService", e);
        }
    }

    /**
     * Creates and configures the HEVC encoder, binding it to the persistent input
     * surface (which gives the surface its dimensions). Leaves the encoder in the
     * Configured state — NOT started. Safe to call repeatedly (recreates a fresh
     * encoder each recording for clean per-file CSD/keyframe boundaries). Returns
     * false on failure, in which case the encoder is left null.
     */
    private boolean configureEncoder() {
        try {
            // Prefer the dedicated hardware CQ encoder (Qualcomm c2.qti.hevc.encoder.cq);
            // fall back to the system-default HEVC encoder on other SoCs.
            boolean usingCqEncoder = false;
            try {
                encoder = MediaCodec.createByCodecName("c2.qti.hevc.encoder.cq");
                usingCqEncoder = true;
            } catch (Exception ignored) {
                encoder = MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_VIDEO_HEVC);
            }

            // Probe capabilities from the chosen encoder.
            MediaCodecInfo.VideoCapabilities vc = null;
            MediaCodecInfo.EncoderCapabilities ec = null;
            try {
                MediaCodecInfo.CodecCapabilities caps = encoder.getCodecInfo()
                        .getCapabilitiesForType(MediaFormat.MIMETYPE_VIDEO_HEVC);
                vc = caps.getVideoCapabilities();
                ec = caps.getEncoderCapabilities();
            } catch (Exception e) {
                Log.w(TAG, "getCapabilitiesForType failed: " + e.getMessage());
            }
            boolean supportsCq = ec != null && ec.isBitrateModeSupported(
                    MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CQ);
            // Use max bitrate the encoder reports; cap at 200 Mbps to avoid runaway.
            int maxBr = (vc != null) ? vc.getBitrateRange().getUpper() : 200_000_000;
            // Target ~0.5 bpp (2× the old default) for near-visually-lossless HEVC.
            recordBitrate = (int) Math.min(maxBr,
                    Math.min(200_000_000L, (long) recordWidth * recordHeight * 30L / 2L));

            MediaFormat fmt = MediaFormat.createVideoFormat(
                    MediaFormat.MIMETYPE_VIDEO_HEVC, recordWidth, recordHeight);
            fmt.setInteger(MediaFormat.KEY_COLOR_FORMAT,
                    MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface);
            fmt.setInteger(MediaFormat.KEY_FRAME_RATE, 30);
            // 1-second GOP. All-intra (0) overwhelmed the ISP memory bus at 4K30,
            // causing ~1Hz frame drops in both the preview and the recorded file.
            fmt.setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 1);

            if (supportsCq) {
                fmt.setInteger(MediaFormat.KEY_BITRATE_MODE,
                        MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CQ);
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
                    fmt.setInteger(MediaFormat.KEY_QUALITY, 85);
                }
                // Some encoders require KEY_BIT_RATE even in CQ mode; supply it.
                fmt.setInteger(MediaFormat.KEY_BIT_RATE, recordBitrate);
            } else {
                fmt.setInteger(MediaFormat.KEY_BIT_RATE, recordBitrate);
                fmt.setInteger(MediaFormat.KEY_BITRATE_MODE,
                        MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_VBR);
            }

            if (hdrSupported) {
                fmt.setInteger(MediaFormat.KEY_PROFILE,
                        MediaCodecInfo.CodecProfileLevel.HEVCProfileMain10);
                fmt.setInteger(MediaFormat.KEY_COLOR_STANDARD, MediaFormat.COLOR_STANDARD_BT2020);
                fmt.setInteger(MediaFormat.KEY_COLOR_TRANSFER, MediaFormat.COLOR_TRANSFER_HLG);
                // Limited range: must match the muxer's BROADCAST_RANGE tag —
                // a full-range bitstream under a limited-range container tag is
                // what made Samsung's player reject the files.
                fmt.setInteger(MediaFormat.KEY_COLOR_RANGE, MediaFormat.COLOR_RANGE_LIMITED);
            }
            encoder.configure(fmt, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE);
            // Bind to the persistent surface; this is what gives the surface its
            // dimensions so the camera session can be configured with it.
            encoder.setInputSurface(persistentInputSurface);
            inputSurfaceSized = true;
            Log.i(TAG, String.format("encoder configured %dx%d @%d %s %s",
                    recordWidth, recordHeight, recordBitrate,
                    usingCqEncoder ? "cq.encoder" : "default",
                    supportsCq ? "CQ q=85" : "VBR"));
            return true;
        } catch (Exception e) {
            Log.e(TAG, "configureEncoder failed", e);
            releaseEncoder();
            return false;
        }
    }

    public synchronized void startRecording() {
        if (recording || device == null) return;
        if (rawVideoMode) {
            // No encoder here: just add the RAW stream to the repeating request
            // (no session rebuild) and lock AWB so white balance can't pump
            // mid-clip. Frames flow to native via onRawImage.
            if (rawReader == null || session == null) {
                Log.e(TAG, "startRecording: RAW stream unavailable");
                return;
            }
            // Free any RAW buffers a prior still burst might still hold, so the
            // streaming path starts with the reader's full slot count.
            synchronized (rawLock) { flushPendingStills(); }
            rawRecording = true;
            recording = true;
            startRepeating(true);
            Log.i(TAG, "startRecording (raw video -> native ISP)");
            return;
        }
        sentFormat = false;
        try {
            // Encoder is normally already configured (from startPreview, or the
            // previous recording reconfigures one). Recreate if it was released.
            if (encoder == null && !configureEncoder()) return;

            // Just start the encoder and add its surface to the repeating request —
            // NO recreateSession(), so the camera/ISP pipeline never stalls.
            encoder.start();
            startRepeating(true);

            draining = true;
            drainThread = new Thread(this::drainLoop, "HevcDrain");
            drainThread.start();
            recording = true;
            Log.i(TAG, "startRecording (encoder started, no session rebuild)");
        } catch (Exception e) {
            Log.e(TAG, "startRecording failed", e);
            releaseEncoder();
        }
    }

    public synchronized void stopRecording() {
        if (!recording) return;
        if (rawVideoMode) {
            recording = false;
            rawRecording = false;
            startRepeating(false);   // back to preview-only
            Log.i(TAG, "stopRecording (raw video)");
            return;
        }
        recording = false;
        // Stop the camera feeding the encoder surface first (drop it from the
        // repeating request), so no frames arrive after we signal end-of-stream.
        startRepeating(false);
        try {
            if (encoder != null) encoder.signalEndOfInputStream();
        } catch (Exception e) {
            Log.e(TAG, "signalEndOfInputStream", e);
        }
        draining = false;
        if (drainThread != null) {
            try { drainThread.join(500); } catch (InterruptedException ignored) {}
            drainThread = null;
        }
        // Release the encoder; the persistent surface keeps its dimensions, so the
        // next recording just reconfigures a fresh encoder onto it (no session rebuild).
        releaseEncoder();
        Log.i(TAG, "stopRecording");
    }

    // ── Internals ────────────────────────────────────────────────────────────

    private void releaseEncoder() {
        try { if (encoder != null) { encoder.stop(); encoder.release(); } } catch (Exception ignored) {}
        encoder = null;
    }

    private byte[] drainBuf = new byte[0];   // reused per-packet to avoid per-frame GC

    private void drainLoop() {
        MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
        while (draining) {
            int idx;
            try { idx = encoder.dequeueOutputBuffer(info, 10000); }
            catch (Exception e) { Log.e(TAG, "dequeue", e); break; }

            if (idx == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                MediaFormat of = encoder.getOutputFormat();
                ByteBuffer csd0 = of.getByteBuffer("csd-0");
                if (csd0 != null && !sentFormat) {
                    byte[] csd = new byte[csd0.remaining()];
                    csd0.get(csd);
                    nativeOnVideoFormat(nativeCtx, csd, recordWidth, recordHeight);
                    sentFormat = true;
                }
            } else if (idx >= 0) {
                ByteBuffer ob = encoder.getOutputBuffer(idx);
                boolean config = (info.flags & MediaCodec.BUFFER_FLAG_CODEC_CONFIG) != 0;
                if (ob != null && info.size > 0) {
                    ob.position(info.offset);
                    ob.limit(info.offset + info.size);
                    if (config) {
                        // csd is read by native via the array length — pass exact size.
                        // Rare (once), so a dedicated allocation is fine.
                        if (!sentFormat) {
                            byte[] csd = new byte[info.size];
                            ob.get(csd);
                            nativeOnVideoFormat(nativeCtx, csd, recordWidth, recordHeight);
                            sentFormat = true;
                        }
                    } else {
                        // Reuse a single growable buffer — a fresh byte[] per frame
                        // (MBs for a 12MP I-frame) triggered a ~1Hz GC that stalled
                        // the preview during recording. Native reads `info.size`.
                        if (drainBuf.length < info.size) drainBuf = new byte[info.size];
                        ob.get(drainBuf, 0, info.size);
                        boolean key = (info.flags & MediaCodec.BUFFER_FLAG_KEY_FRAME) != 0;
                        nativeOnVideoPacket(nativeCtx, drainBuf, info.size, info.presentationTimeUs, key);
                    }
                }
                encoder.releaseOutputBuffer(idx, false);
                if ((info.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0) break;
            }
        }
    }

    private void onPreviewImage(ImageReader reader) {
        Image img = reader.acquireLatestImage();
        if (img == null) return;
        HardwareBuffer hb = img.getHardwareBuffer();
        if (hb == null) { img.close(); return; }
        nativeOnPreviewBuffer(nativeCtx, hb, img);
        hb.close();
    }

    public void releaseImage(Image img) {
        if (img != null) img.close();
    }

    // ── Bracketed RAW still ──────────────────────────────────────────────────

    public synchronized void takePhoto(String basePath, int shots) {
        // Capture the still stream that the current session actually has wired in
        // (activeStill): DNG on RAW devices, full-res YUV→PNG otherwise. In VIDEO
        // mode the session holds the encoder, not a still stream → STILL_NONE.
        if (session == null || device == null || rawRecording || activeStill == STILL_NONE) {
            Log.e(TAG, "takePhoto: unavailable (still=" + activeStill + " recording=" + rawRecording + ")");
            return;
        }
        photoBase = basePath;
        if (activeStill == STILL_YUV) { captureYuvStill(); return; }
        // STILL_RAW: arm the still path to accept exactly this burst's frames (and
        // clear any stale state from a previous, possibly-incomplete burst).
        final int shotCount = Math.max(1, shots);
        synchronized (rawLock) {
            flushPendingStills();
            photoFramesWanted = shotCount;
            photoBurstSize    = shotCount;
        }
        try {
            // FAST = one shot at EV 0 (motion-proof). STATIC = -2 / 0 / +2 EV
            // exposure bracket, clamped to the device's compensation range.
            int[] evComp;
            if (shotCount <= 1) {
                evComp = new int[]{ 0 };
            } else {
                Rational step = characteristics.get(CameraCharacteristics.CONTROL_AE_COMPENSATION_STEP);
                double stepEv = step != null ? step.doubleValue() : (1.0 / 3.0);
                int two = (int) Math.round(2.0 / (stepEv > 0 ? stepEv : (1.0 / 3.0)));
                evComp = new int[]{ -two, 0, two };
            }
            Range<Integer> range = characteristics.get(CameraCharacteristics.CONTROL_AE_COMPENSATION_RANGE);

            List<CaptureRequest> burst = new ArrayList<>();
            for (int i = 0; i < shotCount; i++) {
                CaptureRequest.Builder b = device.createCaptureRequest(CameraDevice.TEMPLATE_STILL_CAPTURE);
                b.addTarget(rawReader.getSurface());
                int comp = evComp[i];
                if (range != null) comp = Math.max(range.getLower(), Math.min(range.getUpper(), comp));
                b.set(CaptureRequest.CONTROL_AE_EXPOSURE_COMPENSATION, comp);
                // Keep the RAW as untouched as possible.
                b.set(CaptureRequest.NOISE_REDUCTION_MODE, CaptureRequest.NOISE_REDUCTION_MODE_OFF);
                b.set(CaptureRequest.EDGE_MODE, CaptureRequest.EDGE_MODE_OFF);
                b.setTag(i);
                burst.add(b.build());
            }
            session.captureBurst(burst, captureCallback, bgHandler);
            Log.i(TAG, "takePhoto burst x" + shotCount + " -> " + basePath);
        } catch (CameraAccessException e) {
            Log.e(TAG, "takePhoto failed", e);
        }
    }

    // Diagnostic callback for the YUV still so a HAL-side failure isn't silent.
    private final CameraCaptureSession.CaptureCallback stillCb =
            new CameraCaptureSession.CaptureCallback() {
        @Override public void onCaptureStarted(CameraCaptureSession s, CaptureRequest r,
                                               long timestamp, long frameNumber) {
            Log.i(TAG, "still capture started");
        }
        @Override public void onCaptureCompleted(CameraCaptureSession s, CaptureRequest r,
                                                 TotalCaptureResult res) {
            Log.i(TAG, "still capture completed");
        }
        @Override public void onCaptureFailed(CameraCaptureSession s, CaptureRequest r,
                                              android.hardware.camera2.CaptureFailure f) {
            // No frame will arrive for this shot → drop its queued name so the FIFO
            // stays aligned with delivered images.
            stillNames.poll();
            Log.e(TAG, "still capture FAILED reason=" + f.getReason()
                       + " imageCaptured=" + f.wasImageCaptured());
        }
    };

    // Non-RAW still: one full-resolution YUV_420_888 frame with the ISP minimised
    // (NR/edge off) to stay closest to the sensor. Delivered to onStillImage.
    private void captureYuvStill() {
        try {
            CaptureRequest.Builder b =
                    device.createCaptureRequest(CameraDevice.TEMPLATE_STILL_CAPTURE);
            b.addTarget(stillReader.getSurface());
            b.set(CaptureRequest.NOISE_REDUCTION_MODE, CaptureRequest.NOISE_REDUCTION_MODE_OFF);
            b.set(CaptureRequest.EDGE_MODE, CaptureRequest.EDGE_MODE_OFF);
            stillNames.add(photoBase);   // paired FIFO with the delivered image
            session.capture(b.build(), stillCb, bgHandler);
            Log.i(TAG, "takePhoto YUV still " + stillWidth + "x" + stillHeight + " -> " + photoBase);
        } catch (CameraAccessException e) {
            Log.e(TAG, "takePhoto YUV failed", e);
        }
    }

    // Copies each delivered YUV frame out (so the Image is released immediately and
    // the reader thread never blocks), then queues the slow RGB→PNG encode onto the
    // dedicated worker. Drains every pending image so a burst can't strand one.
    private void onStillImage(ImageReader reader) {
        Image img;
        while ((img = reader.acquireNextImage()) != null) {
            try {
                Image.Plane[] p = img.getPlanes();
                final int w = img.getWidth(), h = img.getHeight();
                ByteBuffer yb = p[0].getBuffer();
                ByteBuffer ub = p[1].getBuffer();
                ByteBuffer vb = p[2].getBuffer();
                final byte[] y = new byte[yb.remaining()]; yb.get(y);
                final byte[] u = new byte[ub.remaining()]; ub.get(u);
                final byte[] v = new byte[vb.remaining()]; vb.get(v);
                final int yStride = p[0].getRowStride();
                final int uStride = p[1].getRowStride();
                final int vStride = p[2].getRowStride();
                final int uvPix   = p[1].getPixelStride();
                final int orient  = sensorOrientation;
                String name = stillNames.poll();
                final String path = (name != null ? name : photoBase) + ".png";
                ExecutorService enc = stillEncoder;
                if (enc != null && !enc.isShutdown()) {
                    enc.execute(() -> {
                        nativeOnStillFrame(path, y, u, v, w, h, yStride, uStride, vStride, uvPix, orient);
                        Log.i(TAG, "still saved: " + path);
                    });
                }
            } catch (Exception e) {
                Log.e(TAG, "onStillImage failed", e);
            } finally {
                img.close();
            }
        }
    }

    // Repeating-request callback in raw video mode (preview AND record): feeds
    // the latest neutral point (white balance reference) to the native ISP,
    // throttled. Running during preview means the ISP already has a valid WB
    // before recording starts — no green/blue first frame, no warm→cold jump.
    // During record AWB is locked, so this just re-reports the same stable value.
    private int rawNeutralCounter;
    private final CameraCaptureSession.CaptureCallback rawVideoCaptureCallback =
            new CameraCaptureSession.CaptureCallback() {
        @Override public void onCaptureCompleted(CameraCaptureSession s, CaptureRequest request,
                                                 TotalCaptureResult result) {
            // ~ every 15th frame is plenty for a slowly-drifting white balance.
            if ((rawNeutralCounter++ % 15) != 0) return;
            Rational[] n = result.get(CaptureResult.SENSOR_NEUTRAL_COLOR_POINT);
            if (n == null || n.length < 3) return;
            nativeOnRawVideoNeutral(new float[]{
                    n[0].floatValue(), n[1].floatValue(), n[2].floatValue() });
        }
    };

    private final CameraCaptureSession.CaptureCallback captureCallback =
            new CameraCaptureSession.CaptureCallback() {
        @Override public void onCaptureCompleted(CameraCaptureSession s, CaptureRequest request,
                                                 TotalCaptureResult result) {
            Long ts = result.get(CaptureResult.SENSOR_TIMESTAMP);
            Object tag = request.getTag();
            if (ts == null || !(tag instanceof Integer)) return;
            synchronized (rawLock) {
                pendingResults.put(ts, result);
                pendingIndex.put(ts, (Integer) tag);
                tryEmit(ts);
            }
        }
    };

    private void onRawImage(ImageReader reader) {
        if (rawRecording) {
            // Streaming video: hand each Bayer frame to the native ISP. The native
            // side copies it synchronously into the GPU input ring, so the Image
            // is closed before the next acquire — slots never pile up.
            Image img;
            while ((img = reader.acquireNextImage()) != null) {
                try {
                    Image.Plane plane = img.getPlanes()[0];
                    nativeOnRawVideoFrame(plane.getBuffer(), img.getWidth(),
                            img.getHeight(), plane.getRowStride(), img.getTimestamp());
                } finally {
                    img.close();
                }
            }
            return;
        }
        Image img = reader.acquireNextImage();
        if (img == null) return;
        synchronized (rawLock) {
            // Only hold frames for an active bracketed-still burst. Anything else
            // (e.g. frames still in flight right after stopRecording flips
            // rawRecording false) is a stray we must close, or it leaks the
            // reader's buffers and the next recording crashes on acquire.
            if (photoFramesWanted <= 0) { img.close(); return; }
            photoFramesWanted--;
            long ts = img.getTimestamp();
            pendingImages.put(ts, img);
            tryEmit(ts);
        }
    }

    // Closes any images still held for a (failed/abandoned) still burst and
    // resets the pairing maps. Call under rawLock before reusing the RAW reader.
    private void flushPendingStills() {
        for (Image i : pendingImages.values()) { try { i.close(); } catch (Exception ignored) {} }
        pendingImages.clear();
        pendingResults.clear();
        pendingIndex.clear();
        photoFramesWanted = 0;
    }

    // Pairs a RAW image with its capture result (by sensor timestamp) and emits
    // a DNG via native. Called under rawLock.
    private void tryEmit(long ts) {
        Image img = pendingImages.get(ts);
        TotalCaptureResult res = pendingResults.get(ts);
        Integer idx = pendingIndex.get(ts);
        if (img == null || res == null || idx == null) return;
        pendingImages.remove(ts); pendingResults.remove(ts); pendingIndex.remove(ts);

        try {
            float[] neutral = null;
            Rational[] n = res.get(CaptureResult.SENSOR_NEUTRAL_COLOR_POINT);
            if (n != null && n.length >= 3) {
                neutral = new float[]{ n[0].floatValue(), n[1].floatValue(), n[2].floatValue() };
            }
            float[] black = res.get(CaptureResult.SENSOR_DYNAMIC_BLACK_LEVEL);
            Integer white = res.get(CaptureResult.SENSOR_DYNAMIC_WHITE_LEVEL);
            // Per-frame exposure for the bracket HDR merge (linearizes each shot).
            Long expNs = res.get(CaptureResult.SENSOR_EXPOSURE_TIME);
            Integer iso = res.get(CaptureResult.SENSOR_SENSITIVITY);

            Image.Plane plane = img.getPlanes()[0];
            ByteBuffer buf = plane.getBuffer();
            // FAST single shot gets a clean name; a bracket gets per-frame indices.
            String path = (photoBurstSize <= 1) ? (photoBase + ".dng")
                                                : (photoBase + "_" + idx + ".dng");
            nativeOnRawFrame(nativeCtx, path, buf, rawWidth, rawHeight,
                    plane.getRowStride(), neutral, black, white != null ? white : 0,
                    expNs != null ? expNs : 0L, iso != null ? iso : 0, idx, photoBurstSize);
        } catch (Exception e) {
            Log.e(TAG, "emit RAW failed", e);
        } finally {
            img.close();
        }
    }

    private final CameraDevice.StateCallback deviceCallback = new CameraDevice.StateCallback() {
        @Override public void onOpened(CameraDevice cam) {
            device = cam;
            try { recreateSession(); } catch (Exception e) { Log.e(TAG, "session", e); }
        }
        @Override public void onDisconnected(CameraDevice cam) { Log.e(TAG, "disconnected"); cam.close(); }
        @Override public void onError(CameraDevice cam, int error) { Log.e(TAG, "error " + error); cam.close(); }
    };

    /**
     * Builds the capture session once (called only from onOpened).
     * Always registers: preview + persistentInputSurface (HLG10) + RAW (if available).
     * Recording start/stop is handled by setRepeatingRequest() alone — no session rebuild,
     * no ISP pipeline stall.
     */
    private void recreateSession() throws CameraAccessException {
        if (device == null) return;
        if (session != null) { session.close(); session = null; }

        final int gen = ++sessionGen;
        final CameraDevice dev = device;

        List<OutputConfiguration> outputs = new ArrayList<>();
        // Preview stays SDR (displayed on an sRGB swapchain → correct colors).
        outputs.add(new OutputConfiguration(previewReader.getSurface()));
        // The second stream depends on mode/capabilities (each is a guaranteed
        // 2-stream combo — never preview + encoder + still together):
        //   · RAW video → preview + RAW16 (native ISP; also serves stills)
        //   · PHOTO     → preview + still (RAW16/DNG if present, else YUV/PNG)
        //   · VIDEO     → preview + HLG10 encoder surface
        if (rawVideoMode && rawReader != null) {
            outputs.add(new OutputConfiguration(rawReader.getSurface()));
            activeStill = STILL_RAW;
        } else if (photoMode && rawReader != null) {
            outputs.add(new OutputConfiguration(rawReader.getSurface()));
            activeStill = STILL_RAW;
        } else if (photoMode && stillReader != null) {
            outputs.add(new OutputConfiguration(stillReader.getSurface()));
            activeStill = STILL_YUV;
        } else if (inputSurfaceSized && persistentInputSurface != null) {
            // Encoder surface registered so recording starts with just a
            // setRepeatingRequest() — no session close/reopen, no stall.
            outputs.add(makeOutput(persistentInputSurface));
            activeStill = STILL_NONE;
        } else if (rawReader != null) {
            // Fallback (encoder couldn't be configured): preview + RAW for stills.
            outputs.add(new OutputConfiguration(rawReader.getSurface()));
            activeStill = STILL_RAW;
        } else {
            activeStill = STILL_NONE;
        }

        Executor exec = cmd -> { if (bgHandler != null) bgHandler.post(cmd); };
        SessionConfiguration cfg = new SessionConfiguration(
                SessionConfiguration.SESSION_REGULAR, outputs, exec,
                new CameraCaptureSession.StateCallback() {
                    @Override public void onConfigured(CameraCaptureSession s) {
                        if (gen != sessionGen || device != dev) {
                            try { s.close(); } catch (Exception ignored) {}
                            return;
                        }
                        session = s;
                        startRepeating(recording);
                    }
                    @Override public void onConfigureFailed(CameraCaptureSession s) {
                        Log.e(TAG, "session configure failed");
                    }
                });
        device.createCaptureSession(cfg);
    }

    private OutputConfiguration makeOutput(Surface s) {
        OutputConfiguration oc = new OutputConfiguration(s);
        if (hdrSupported && Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            oc.setDynamicRangeProfile(DynamicRangeProfiles.HLG10);
        }
        return oc;
    }

    private void startRepeating(boolean withEncoder) {
        if (device == null || session == null) return;
        try {
            int template = withEncoder ? CameraDevice.TEMPLATE_RECORD : CameraDevice.TEMPLATE_PREVIEW;
            CaptureRequest.Builder b = device.createCaptureRequest(template);
            // While RAW recording, capture ONLY the RAW stream — drop the preview
            // target. No preview frames means the renderer goes idle (no swapchain
            // present / compositor hold), so the native ISP compute owns the whole
            // GPU instead of being periodically stalled ~90ms by the present. The
            // on-screen preview freezes during recording (intended).
            boolean rawRecord = withEncoder && rawVideoMode && rawReader != null;
            if (!rawRecord) b.addTarget(previewReader.getSurface());
            if (withEncoder) {
                if (rawRecord) {
                    b.addTarget(rawReader.getSurface());
                    // One white balance per clip: lock AWB and report the locked
                    // neutral to the native ISP (rawVideoCaptureCallback below).
                    b.set(CaptureRequest.CONTROL_AWB_LOCK, true);
                } else if (persistentInputSurface != null) {
                    b.addTarget(persistentInputSurface);
                }
            }
            // Disable video (electronic) stabilization. TEMPLATE_RECORD turns EIS
            // on; its gyro/frame timestamp sync hiccups ~1Hz and stalls the camera
            // pipeline → the once-per-second preview micro-stop while recording.
            b.set(CaptureRequest.CONTROL_VIDEO_STABILIZATION_MODE,
                  CaptureRequest.CONTROL_VIDEO_STABILIZATION_MODE_OFF);
            // Pin the frame rate so AE raises gain (not exposure time) in dim
            // light — without this the camera delivers < 30fps regardless of how
            // fast the ISP/encoder run.
            if (aeFpsRange != null)
                b.set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, aeFpsRange);
            // In raw mode, keep the neutral callback on BOTH preview and record
            // so the native ISP has a valid white balance from frame 0.
            CameraCaptureSession.CaptureCallback cb =
                    rawVideoMode ? rawVideoCaptureCallback : null;
            session.setRepeatingRequest(b.build(), cb, bgHandler);
            Log.i(TAG, "repeating request (encoder=" + withEncoder
                       + " raw=" + rawVideoMode + ")");
        } catch (CameraAccessException | IllegalStateException e) {
            // Device/session may have been torn down by a concurrent pause/resume.
            Log.e(TAG, "repeating (ignored, session torn down): " + e.getMessage());
        }
    }

    private boolean supportsHlg10(CameraCharacteristics chars) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU) return false;
        DynamicRangeProfiles p = chars.get(CameraCharacteristics.REQUEST_AVAILABLE_DYNAMIC_RANGE_PROFILES);
        return p != null && p.getSupportedProfiles().contains(DynamicRangeProfiles.HLG10);
    }

    // Best AE target fps range for steady 30fps: prefer one whose upper bound is
    // 30 (so AE tops out at 30 and, when light allows, holds it); among those the
    // highest lower bound (least room to sag). Falls back to the highest upper
    // bound, then to [30,30] if the device reports nothing.
    private Range<Integer> pickAeFpsRange(CameraCharacteristics chars) {
        Range<Integer>[] ranges = chars.get(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES);
        if (ranges == null || ranges.length == 0) return new Range<>(30, 30);
        Range<Integer> best = ranges[0];
        for (Range<Integer> r : ranges) {
            boolean b30 = best.getUpper() == 30, r30 = r.getUpper() == 30;
            if (b30 != r30)        best = r30 ? r : best;
            else if (b30)          best = r.getLower() > best.getLower() ? r : best;
            else                   best = r.getUpper() > best.getUpper() ? r : best;
        }
        return best;
    }

    private String pickBackCamera() throws CameraAccessException {
        for (String id : manager.getCameraIdList()) {
            CameraCharacteristics c = manager.getCameraCharacteristics(id);
            Integer facing = c.get(CameraCharacteristics.LENS_FACING);
            if (facing != null && facing == CameraCharacteristics.LENS_FACING_BACK) return id;
        }
        return null;
    }

    private Size[] streamSizes(CameraCharacteristics chars) {
        StreamConfigurationMap scm = chars.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP);
        Size[] s = scm != null ? scm.getOutputSizes(SurfaceTexture.class) : null;
        return (s != null && s.length > 0) ? s : new Size[]{ new Size(1280, 960) };
    }

    // The sensor's true full-FOV aspect ratio, from the active pixel array. Used
    // as the record/preview aspect so nothing is cropped. We deliberately do NOT
    // use getOutputSizes() max-area for this: the framework moves sub-30fps
    // "high-resolution" sizes out of that list, so on sensors whose top 4:3 modes
    // are slow, its largest remaining entry can be a square — which is exactly what
    // made this device record 1:1. Falls back to the largest output's aspect.
    private double sensorAspect(CameraCharacteristics chars, Size[] sizes) {
        android.graphics.Rect a = chars.get(CameraCharacteristics.SENSOR_INFO_ACTIVE_ARRAY_SIZE);
        if (a != null && a.height() > 0) return (double) a.width() / a.height();
        Size px = chars.get(CameraCharacteristics.SENSOR_INFO_PIXEL_ARRAY_SIZE);
        if (px != null && px.getHeight() > 0) return (double) px.getWidth() / px.getHeight();
        return nativeAspect(sizes);
    }

    // Aspect ratio of the largest-area output = the sensor's native / full-FOV aspect.
    private double nativeAspect(Size[] sizes) {
        Size full = sizes[0];
        for (Size s : sizes)
            if ((long) s.getWidth() * s.getHeight() > (long) full.getWidth() * full.getHeight()) full = s;
        return (double) full.getWidth() / full.getHeight();
    }

    private static boolean sameAspect(Size s, double aspect) {
        return Math.abs((double) s.getWidth() / s.getHeight() - aspect) < 0.02;
    }

    // Largest output size at `aspect` that the HEVC encoder can encode at 30fps —
    // i.e. the highest-resolution recording with no FOV crop.
    private Size pickLargestEncodable(Size[] sizes, double aspect, int maxWidth) {
        MediaCodecInfo.VideoCapabilities vc = hevcVideoCaps();
        Size best = null;
        for (Size s : sizes) {
            if (!sameAspect(s, aspect)) continue;
            if (s.getWidth() > maxWidth) continue;
            boolean ok = (vc == null) || vc.areSizeAndRateSupported(s.getWidth(), s.getHeight(), 30.0);
            if (!ok) continue;
            if (best == null ||
                (long) s.getWidth() * s.getHeight() > (long) best.getWidth() * best.getHeight()) best = s;
        }
        if (best != null) return best;
        // Fall back to the largest at-aspect size regardless of encoder report.
        for (Size s : sizes)
            if (sameAspect(s, aspect) && (best == null ||
                (long) s.getWidth() * s.getHeight() > (long) best.getWidth() * best.getHeight())) best = s;
        return best != null ? best : sizes[0];
    }

    // Size at `aspect` whose width is closest to `targetWidth` (small preview).
    private Size pickAspectNearWidth(Size[] sizes, double aspect, int targetWidth) {
        Size best = null;
        int bestDelta = Integer.MAX_VALUE;
        for (Size s : sizes) {
            if (!sameAspect(s, aspect)) continue;
            int d = Math.abs(s.getWidth() - targetWidth);
            if (d < bestDelta) { bestDelta = d; best = s; }
        }
        return best != null ? best : sizes[sizes.length - 1];
    }

    private MediaCodecInfo.VideoCapabilities hevcVideoCaps() {
        try {
            MediaCodec enc = MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_VIDEO_HEVC);
            MediaCodecInfo.VideoCapabilities vc = enc.getCodecInfo()
                    .getCapabilitiesForType(MediaFormat.MIMETYPE_VIDEO_HEVC).getVideoCapabilities();
            enc.release();
            return vc;
        } catch (Exception e) {
            Log.e(TAG, "hevcVideoCaps failed", e);
            return null;
        }
    }
}
