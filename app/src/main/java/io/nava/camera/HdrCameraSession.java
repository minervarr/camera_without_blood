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
import java.util.concurrent.Executor;

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
    private final Object rawLock = new Object();
    private final HashMap<Long, Image> pendingImages = new HashMap<>();
    private final HashMap<Long, TotalCaptureResult> pendingResults = new HashMap<>();
    private final HashMap<Long, Integer> pendingIndex = new HashMap<>();
    private static final int BRACKET_COUNT = 3;
    private static final int MAX_VIDEO_WIDTH = 100000;  // effectively uncapped (max sensor res)

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
            int width, int height, int rowStride, float[] neutral, float[] black, int white);
    private static native void nativeOnUsbFd(int fd);

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

    public synchronized void startPreview() {
        try {
            bgThread = new HandlerThread("HdrCamera");
            bgThread.start();
            bgHandler = new Handler(bgThread.getLooper());

            cameraId = pickBackCamera();
            if (cameraId == null) { Log.e(TAG, "No back camera"); return; }

            characteristics = manager.getCameraCharacteristics(cameraId);
            hdrSupported = supportsHlg10(characteristics);
            Log.i(TAG, "HLG10 supported: " + hdrSupported);

            // Use the sensor's native aspect ratio (the aspect of its largest
            // output) so nothing is cropped — maximum field of view / information.
            // Video records at the largest size the HEVC encoder supports at that
            // aspect; preview is a small ~720-wide reference at the same aspect.
            Size[] sizes = streamSizes(characteristics);
            double aspect = nativeAspect(sizes);
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
                rawReader = ImageReader.newInstance(rawWidth, rawHeight, ImageFormat.RAW_SENSOR, BRACKET_COUNT + 1);
                rawReader.setOnImageAvailableListener(this::onRawImage, bgHandler);
                Log.i(TAG, "RAW reader " + rawWidth + "x" + rawHeight);
            }

            // Create the persistent encoder surface once. It is added to the camera
            // session permanently so recording start/stop never needs recreateSession().
            // A freshly created persistent surface has NO dimensions until an encoder
            // is configured on it — and the camera session can't be configured with an
            // unsized surface. So configure the encoder now (Configured state, not
            // started) to size the surface before openCamera builds the session.
            if (persistentInputSurface == null) {
                persistentInputSurface = MediaCodec.createPersistentInputSurface();
            }
            if (encoder == null) configureEncoder();

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
            releaseEncoder();   // drops the configured-but-never-started encoder, if any
            if (persistentInputSurface != null) { persistentInputSurface.release(); persistentInputSurface = null; }
            inputSurfaceSized = false;
        } catch (Exception e) {
            Log.e(TAG, "stopPreview", e);
        } finally {
            if (bgThread != null) { bgThread.quitSafely(); bgThread = null; bgHandler = null; }
            if (usbReceiver != null) {
                try { appCtx.unregisterReceiver(usbReceiver); } catch (Exception ignored) {}
                usbReceiver = null;
            }
            if (usbConn != null) { try { usbConn.close(); } catch (Exception ignored) {} usbConn = null; }
            nativeOnUsbFd(0);
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
                // Full range (0-1023) preserves more headroom than broadcast limited
                // (64-940). May not be honored for HLG; log and let encoder decide.
                fmt.setInteger(MediaFormat.KEY_COLOR_RANGE, MediaFormat.COLOR_RANGE_FULL);
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

    public synchronized void takePhoto(String basePath) {
        // RAW is not in the session while the (10-bit) encoder stream is registered
        // — preview + 10-bit encode + RAW is not a guaranteed stream combination.
        if (rawReader == null || session == null || device == null || inputSurfaceSized) {
            Log.e(TAG, "takePhoto: RAW capture unavailable (encoder stream active)");
            return;
        }
        photoBase = basePath;
        try {
            // Exposure-compensation steps for -2 / 0 / +2 EV, clamped to range.
            Rational step = characteristics.get(CameraCharacteristics.CONTROL_AE_COMPENSATION_STEP);
            Range<Integer> range = characteristics.get(CameraCharacteristics.CONTROL_AE_COMPENSATION_RANGE);
            double stepEv = step != null ? step.doubleValue() : (1.0 / 3.0);
            int two = (int) Math.round(2.0 / (stepEv > 0 ? stepEv : (1.0 / 3.0)));
            int[] evComp = { -two, 0, two };

            List<CaptureRequest> burst = new ArrayList<>();
            for (int i = 0; i < BRACKET_COUNT; i++) {
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
            Log.i(TAG, "takePhoto burst x" + BRACKET_COUNT + " -> " + basePath);
        } catch (CameraAccessException e) {
            Log.e(TAG, "takePhoto failed", e);
        }
    }

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
        Image img = reader.acquireNextImage();
        if (img == null) return;
        long ts = img.getTimestamp();
        synchronized (rawLock) {
            pendingImages.put(ts, img);
            tryEmit(ts);
        }
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

            Image.Plane plane = img.getPlanes()[0];
            ByteBuffer buf = plane.getBuffer();
            String path = photoBase + "_" + idx + ".dng";
            nativeOnRawFrame(nativeCtx, path, buf, rawWidth, rawHeight,
                    plane.getRowStride(), neutral, black, white != null ? white : 0);
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
        if (inputSurfaceSized && persistentInputSurface != null) {
            // Encoder surface (HLG10) is registered permanently so recording starts
            // with just a setRepeatingRequest() — no session close/reopen, no stall.
            // RAW is omitted here: preview + 10-bit encoder + RAW is not a guaranteed
            // stream combination and RAW photo isn't needed while recording.
            outputs.add(makeOutput(persistentInputSurface));
        } else if (rawReader != null) {
            // Fallback (encoder couldn't be configured): preview + RAW for stills.
            outputs.add(new OutputConfiguration(rawReader.getSurface()));
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
            b.addTarget(previewReader.getSurface());
            if (withEncoder && persistentInputSurface != null) b.addTarget(persistentInputSurface);
            // Disable video (electronic) stabilization. TEMPLATE_RECORD turns EIS
            // on; its gyro/frame timestamp sync hiccups ~1Hz and stalls the camera
            // pipeline → the once-per-second preview micro-stop while recording.
            b.set(CaptureRequest.CONTROL_VIDEO_STABILIZATION_MODE,
                  CaptureRequest.CONTROL_VIDEO_STABILIZATION_MODE_OFF);
            session.setRepeatingRequest(b.build(), null, bgHandler);
            Log.i(TAG, "repeating request (encoder=" + withEncoder + ")");
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
