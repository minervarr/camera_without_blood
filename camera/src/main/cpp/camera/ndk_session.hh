#pragma once

#include <camera/NdkCameraManager.h>
#include <camera/NdkCameraDevice.h>
#include <camera/NdkCameraCaptureSession.h>
#include <media/NdkImageReader.h>

#include "../jni/jni_camera.hh"
#include "ndk_encoder.hh"
#include "dng_writer.hh"

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

namespace ndkcam {

// Native (NDK Camera2) replacement for the Java HdrCameraSession, for the
// RAW_PQ path only.
//
// Why this can exist at all: the one Camera2 API with no NDK equivalent is
// OutputConfiguration.setDynamicRangeProfile (HLG10), and the RAW path never
// used it — it streams RAW16 Bayer and does all the colour work in the ISP
// compute shaders. Everything the RAW session needs (stream configuration,
// AImageReader, AWB lock, SENSOR_NEUTRAL_COLOR_POINT) has a direct ACAMERA_*
// equivalent, so the Java round-trip bought nothing.
//
// It also removes a per-preview-frame cost: the Java path handed each frame
// across JNI as an Image, wrapped a HardwareBuffer, held a global ref, and
// called back into Java to release it. Here AImage_getHardwareBuffer returns
// the buffer directly.
//
// `available()` reports whether this device can use the path (back camera with
// a RAW capability and a RAW16 stream). Callers must fall back to the Java
// session when it is false.
class Session {
public:
    Session() = default;
    ~Session();

    Session(const Session&)            = delete;
    Session& operator=(const Session&) = delete;

    // Opens the camera and resolves stream sizes. Safe to call when the device
    // has no RAW support: returns false and leaves available() false.
    bool init(jni::PreviewSink preview, jni::RawSink raw, jni::RawVideoSink raw_video,
              jni::RecordSink record);
    void shutdown();

    bool available() const { return available_; }

    // True when this device has no RAW16 stream but the native video path can
    // still drive it: preview + the encoder's input surface, zero-copy. This is
    // the replacement for the Java "LEGACY_HLG" path, which on the verified
    // device was never HLG at all — it reports no dynamic range profiles and no
    // Main10 encoder, so that path was plain 8-bit SDR behind a misleading name.
    bool video_available() const { return video_available_; }

    int32_t video_width()  const { return encoder_.width(); }
    int32_t video_height() const { return encoder_.height(); }

    bool start_preview();
    void stop_preview();

    // Adds the RAW16 target to the repeating request and locks AWB for the
    // clip (one white balance per clip, reported through RawVideoSink).
    bool start_recording();
    void stop_recording();

    // Fires a RAW still burst: 1 = single -> "<base>.dng", otherwise
    // exposure-bracketed -> "<base>_<index>.dng".
    bool take_photo(const char* base_path, int shots);

    int32_t raw_width()  const { return raw_w_; }
    int32_t raw_height() const { return raw_h_; }

private:
    bool pick_camera();
    bool configure_session();
    bool build_repeating(bool with_raw);
    void teardown_session();

    static void on_disconnected(void* ctx, ACameraDevice* dev);
    static void on_device_error(void* ctx, ACameraDevice* dev, int err);
    static void on_session_ready(void* ctx, ACameraCaptureSession* s);
    static void on_session_active(void* ctx, ACameraCaptureSession* s);
    static void on_session_closed(void* ctx, ACameraCaptureSession* s);
    static void on_preview_image(void* ctx, AImageReader* r);
    static void on_raw_image(void* ctx, AImageReader* r);
    static void on_capture_completed(void* ctx, ACameraCaptureSession* s,
                                     ACaptureRequest* req, const ACameraMetadata* result);

    jni::PreviewSink  preview_sink_;
    jni::RawSink      raw_sink_;
    jni::RawVideoSink raw_video_sink_;
    jni::RecordSink   record_sink_;

    ACameraManager*        manager_ = nullptr;
    ACameraDevice*         device_  = nullptr;
    ACameraCaptureSession* session_ = nullptr;
    ACaptureSessionOutputContainer* outputs_ = nullptr;

    AImageReader*          preview_reader_ = nullptr;
    ANativeWindow*         preview_window_ = nullptr;
    ACaptureSessionOutput* preview_output_ = nullptr;
    ACameraOutputTarget*   preview_target_ = nullptr;

    AImageReader*          raw_reader_ = nullptr;
    ANativeWindow*         raw_window_ = nullptr;
    ACaptureSessionOutput* raw_output_ = nullptr;
    ACameraOutputTarget*   raw_target_ = nullptr;

    // Video (non-RAW) path: the encoder's input surface is a capture target, so
    // frames never touch the CPU.
    Encoder                encoder_;
    ACaptureSessionOutput* video_output_ = nullptr;
    ACameraOutputTarget*   video_target_ = nullptr;
    bool                   video_available_ = false;
    bool                   has_manual_post_ = false;
    // Encoder-size candidates, largest first. Encoder::configure walks these and
    // the codec itself picks the winner.
    std::vector<Encoder::Size> video_sizes_;

    ACaptureRequest*       request_ = nullptr;

    std::string camera_id_;
    bool        available_  = false;
    int32_t     raw_w_ = 0, raw_h_ = 0;
    int32_t     prev_w_ = 0, prev_h_ = 0;
    int32_t     fps_min_ = 0, fps_max_ = 0;

    std::atomic<bool> recording_{false};
    std::atomic<bool> neutral_sent_{false};

    // Most recent as-shot neutral from the capture results. Stills need it as
    // much as video does: without it a developed RAW has no white balance and
    // comes out green, because a Bayer sensor is about twice as sensitive in
    // green. Written on the camera callback thread, read there too.
    std::atomic<bool>  have_neutral_{false};
    std::atomic<float> last_neutral_[3]{ {1.0f}, {1.0f}, {1.0f} };

    // Still-burst state. `still_paths_` is a FIFO so a burst cannot mislabel
    // its frames (same reason the Java YUV path kept one).
    std::mutex               still_mtx_;
    std::vector<std::string> still_paths_;
    int                      still_total_ = 0;

    dng::DngMeta static_meta_{};

    // Members, not locals: the NDK does not document whether it copies the
    // listener struct (AOSP happens to), and a dangling one would fire on a
    // camera thread.
    AImageReader_ImageListener           preview_listener_{};
    AImageReader_ImageListener           raw_listener_{};
    ACameraDevice_StateCallbacks         dev_cbs_{};
    ACameraCaptureSession_stateCallbacks sess_cbs_{};
};

} // namespace ndkcam
