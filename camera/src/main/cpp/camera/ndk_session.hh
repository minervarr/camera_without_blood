#pragma once

#include <camera/NdkCameraManager.h>
#include <camera/NdkCameraDevice.h>
#include <camera/NdkCameraCaptureSession.h>
#include <media/NdkImageReader.h>

#include "../jni/jni_camera.hh"
#include "ndk_encoder.hh"
#include "dng_writer.hh"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
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
    //
    // Such a device also gets a full-resolution YUV_420_888 still stream in the
    // same session, and take_photo() fires it: without it the shutter had no
    // target on non-RAW devices (raw_target_ is null there) and silently
    // produced nothing. Stills come back as lossless JPEG XL.
    bool video_available() const { return video_available_; }

    int32_t video_width()  const { return encoder_.width(); }
    int32_t video_height() const { return encoder_.height(); }

    bool start_preview();
    void stop_preview();

    // Adds the RAW16 target to the repeating request and locks AWB for the
    // clip (one white balance per clip, reported through RawVideoSink).
    bool start_recording();
    void stop_recording();

    // Fires a RAW still burst (1 = single -> "<base>.dng", otherwise
    // exposure-bracketed -> "<base>_<index>.dng"), or — on devices without RAW —
    // a single full-resolution YUV frame written as "<base>.png" (lossless: on a
    // device with no RAW stream that file IS the capture, so it must open
    // everywhere & lose nothing).
    bool take_photo(const char* base_path, int shots);

    // Still-capture noise reduction, toggled live from the UI. Default OFF —
    // raw sensor pixels; on = the best NR the HAL advertises (HIGH_QUALITY,
    // else FAST). Read at shot time, so it applies without a session rebuild.
    // Only the non-RAW YUV still path obeys it: on a RAW device the shutter
    // fires the RAW16 bracket, and NOISE_REDUCTION_MODE does not touch a DNG.
    void set_still_nr(bool on) { still_nr_on_.store(on, std::memory_order_relaxed); }

    // ── Manual focus ────────────────────────────────────────────────────────
    // Available only where the lens can actually move: a fixed-focus module
    // reports LENS_INFO_MINIMUM_FOCUS_DISTANCE == 0, and AF_MODE_OFF is itself
    // optional. Callers must hide the control when this is false.
    bool  manual_focus_available() const { return af_off_ok_ && max_diopters_ > 0.0f; }
    // Closest focus the lens can reach, in diopters (1/m). 0 diopters is
    // infinity; the slider's range is [0, this].
    float max_focus_diopters() const { return max_diopters_; }
    // Arms manual focus (AF_MODE_OFF) and/or moves the lens. Applied to the
    // live repeating request in place — no rebuild, so this is cheap enough to
    // call while a finger is dragging — and mirrored into every one-shot so the
    // HAL cannot refocus for the photo.
    void set_focus(bool manual, float diopters);
    // Where the lens currently sits, straight from the capture results. Arming
    // manual focus starts here so the first drag nudges the autofocus's answer
    // rather than throwing it away.
    float reported_focus_diopters() const {
        return reported_diopters_.load(std::memory_order_relaxed);
    }

    // ── Focus-check loupe ───────────────────────────────────────────────────
    // Swaps the preview stream for a high-resolution one so a magnified inset
    // shows real sensor detail rather than an upscaled 1080p preview. This
    // reconfigures the session (a brief black blink), so it is refused while
    // recording — a 4K preview stream would eat the RAW path's frame budget.
    bool set_loupe(bool on);
    bool loupe_active()    const { return loupe_on_; }
    bool loupe_available() const { return prev_hi_w_ > prev_w_; }

    // The sensor's noise model for the most recent capture result, as (S, O)
    // pairs per CFA channel. False when the HAL never reported one. See
    // dng::DngMeta::noise_profile for what it means and why it is worth keeping.
    bool noise_profile(double out[8], int& count) const;
    // The high-resolution preview's width, so a caller can work out how much
    // magnification is backed by real pixels rather than upscaling.
    int32_t loupe_width() const { return prev_hi_w_; }

    int32_t raw_width()  const { return raw_w_; }
    int32_t raw_height() const { return raw_h_; }

private:
    bool pick_camera();
    bool configure_session(const bool with_video);
    bool build_repeating(bool with_raw);
    void teardown_session();
    void release_still_stream();

    static void on_disconnected(void* ctx, ACameraDevice* dev);
    static void on_device_error(void* ctx, ACameraDevice* dev, int err);
    static void on_session_ready(void* ctx, ACameraCaptureSession* s);
    static void on_session_active(void* ctx, ACameraCaptureSession* s);
    static void on_session_closed(void* ctx, ACameraCaptureSession* s);
    static void on_preview_image(void* ctx, AImageReader* r);
    static void on_raw_image(void* ctx, AImageReader* r);
    static void on_still_image(void* ctx, AImageReader* r);
    static void on_capture_completed(void* ctx, ACameraCaptureSession* s,
                                     ACaptureRequest* req, const ACameraMetadata* result);

    void still_worker_loop();

    // ACameraCaptureSession_close() is asynchronous: it returns while the HAL
    // is still draining, and its onClosed callback lands later. Anything that
    // frees a stream's consumer (deleting the preview AImageReader for the
    // loupe's size change, say) must wait for that callback first, or the HAL
    // spends the gap failing to queue buffers into a window that no longer
    // exists — visible as a long preview blink, and the same kind of lifecycle
    // overlap that can wedge the camera device-wide.
    void close_session_and_wait();

    // Writes the current AF mode / lens position into a request. Used by the
    // repeating request and by every one-shot, so a still is taken at exactly
    // the focus the preview was showing.
    void apply_focus_entries(ACaptureRequest* req) const;
    // Re-submits the existing repeating request after its focus entries change.
    void resubmit_repeating();

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
    // Still-capture processing: the HAL's best noise reduction / sharpening,
    // resolved against what the device advertises (HIGH_QUALITY if present).
    uint8_t                still_nr_   = ACAMERA_NOISE_REDUCTION_MODE_FAST;
    // The "off" end of the toggle: OFF where the HAL offers it, else FAST
    // (OFF is optional per device — see the probe in pick_camera()).
    uint8_t                still_nr_off_ = ACAMERA_NOISE_REDUCTION_MODE_FAST;
    uint8_t                still_edge_ = ACAMERA_EDGE_MODE_FAST;
    std::atomic<bool>      still_nr_on_{false};
    // Encoder-size candidates, largest first. Encoder::configure walks these and
    // the codec itself picks the winner.
    std::vector<Encoder::Size> video_sizes_;

    // Still (non-RAW) path: full-resolution YUV_420_888 -> lossless JXL. The
    // encode is far too slow for the camera callback thread (12 MP of modular
    // entropy coding), so frames are copied out and handed to a worker.
    struct StillJob {
        std::string            path;
        std::vector<uint8_t>   y, u, v;
        int32_t                w = 0, h = 0;
        int32_t                y_stride = 0, u_stride = 0, v_stride = 0, uv_pix = 0;
    };
    AImageReader*          still_reader_ = nullptr;
    ANativeWindow*         still_window_ = nullptr;
    ACaptureSessionOutput* still_output_ = nullptr;
    ACameraOutputTarget*   still_target_ = nullptr;
    int32_t                still_w_ = 0, still_h_ = 0;
    int32_t                sensor_orientation_ = 0;
    std::thread              still_worker_;
    std::mutex               still_job_mtx_;
    std::condition_variable  still_job_cv_;
    std::deque<StillJob>     still_jobs_;
    bool                     still_worker_quit_ = false;

    ACaptureRequest*       request_ = nullptr;

    std::string camera_id_;
    bool        available_  = false;
    int32_t     raw_w_ = 0, raw_h_ = 0;
    int32_t     prev_w_ = 0, prev_h_ = 0;
    // The loupe's high-resolution preview size, and the size the live reader
    // was actually created at (they differ only while the loupe is open).
    int32_t     prev_hi_w_ = 0, prev_hi_h_ = 0;
    int32_t     prev_live_w_ = 0, prev_live_h_ = 0;
    bool        loupe_on_ = false;

    // Manual focus. max_diopters_ is LENS_INFO_MINIMUM_FOCUS_DISTANCE (the
    // closest the lens focuses, expressed as 1/m); 0 means a fixed-focus lens.
    bool               af_off_ok_    = false;
    float              max_diopters_ = 0.0f;
    std::atomic<bool>  focus_manual_{false};
    std::atomic<float> focus_diopters_{0.0f};
    std::atomic<float> reported_diopters_{0.0f};
    int32_t     fps_min_ = 0, fps_max_ = 0;

    // Signalled by on_session_closed(); see close_session_and_wait().
    std::mutex              session_close_mtx_;
    std::condition_variable session_close_cv_;
    bool                    session_closed_ = true;

    std::atomic<bool> recording_{false};
    std::atomic<bool> neutral_sent_{false};

    // Most recent as-shot neutral from the capture results. Stills need it as
    // much as video does: without it a developed RAW has no white balance and
    // comes out green, because a Bayer sensor is about twice as sensitive in
    // green. Written on the camera callback thread, read there too.
    std::atomic<bool>  have_neutral_{false};
    std::atomic<float> last_neutral_[3]{ {1.0f}, {1.0f}, {1.0f} };

    // Latest ACAMERA_SENSOR_NOISE_PROFILE, kept the same way as last_neutral_:
    // it is a per-RESULT value (it tracks the ISO the shot actually used), not a
    // static characteristic, so it has to be latched off capture results.
    std::atomic<double> noise_profile_[8]{};
    std::atomic<int>    noise_profile_count_{0};

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
    AImageReader_ImageListener           still_listener_{};
    ACameraDevice_StateCallbacks         dev_cbs_{};
    ACameraCaptureSession_stateCallbacks sess_cbs_{};
};

} // namespace ndkcam
