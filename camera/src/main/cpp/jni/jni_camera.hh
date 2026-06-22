#pragma once

#include <jni.h>
#include <android/hardware_buffer.h>

#include <functional>
#include <string>
#include <vector>

// Bridge to the Java HdrCameraSession. 10-bit HDR (HLG10) can only be enabled
// via the Java Camera2 API, so the capture session lives in Java; this layer
// lets native control it and receive preview frames as AHardwareBuffers.
namespace jni {

// Delivers one preview frame. Invoked on the camera's background (Java) thread.
// `release` must be called exactly once when the renderer is done with `buffer`
// (it closes the backing Java Image, returning it to the camera pool); until
// then the buffer must not be reused. Mirrors the AImage_delete lifecycle of
// the old native path.
struct PreviewSink {
    std::function<void(AHardwareBuffer* buffer, std::function<void()> release)> on_frame;
};

// Receives the encoded HDR HEVC stream from the Java MediaCodec. `on_format`
// fires once with the codec config (Annex B VPS/SPS/PPS); `on_packet` fires per
// access unit. Both are invoked on the encoder's drain thread.
struct RecordSink {
    std::function<void(const uint8_t* csd, int len, int width, int height)> on_format;
    std::function<void(const uint8_t* data, int len, int64_t pts_us, bool keyframe)> on_packet;
};

// One RAW frame from a (possibly bracketed) still capture. `path` is the full
// output path for this frame; `data` is 16-bit Bayer. `neutral3`/`black4` are
// the per-shot dynamic DNG tags (may be null if unavailable). `exposure_ns`/`iso`
// are the per-frame exposure (for the bracket HDR merge; 0 if unavailable);
// `index`/`count` identify this frame within the burst. Invoked on the camera
// background thread.
struct RawSink {
    std::function<void(const char* path, const uint16_t* data, int width, int height,
                       int stride_bytes, const float* neutral3, const float* black4,
                       int white, int64_t exposure_ns, int iso,
                       int index, int count)> on_raw;
};

// Streaming RAW16 video frames (raw video mode, while recording). `on_frame`
// is invoked on the camera background thread with a pointer that is only
// valid for the duration of the call — the consumer must copy synchronously.
// `on_neutral` delivers the camera-space neutral (white balance reference)
// captured when the clip's AWB was locked.
struct RawVideoSink {
    std::function<void(const uint8_t* data, int width, int height,
                       int stride_bytes, int64_t ts_ns)> on_frame;
    std::function<void(const float neutral[3])> on_neutral;
};

// Constructs the Java session object. `activity` is the NativeActivity jobject
// (a Context). Must be called once before start/stop. Returns false on failure.
bool hdr_init(JavaVM* vm, jobject activity, PreviewSink preview, RecordSink record,
              RawSink raw, RawVideoSink raw_video);

// Selects the capture pipeline BEFORE startPreview: when true the Java session
// configures preview + streaming RAW16 (no MediaCodec, no HLG10 profile) and
// recording delivers Bayer frames to RawVideoSink; when false the legacy
// HLG10-encoder session is used.
void hdr_set_raw_video(bool enabled);

// Selects PHOTO vs VIDEO mode for the legacy (non-RAW-video) session. In PHOTO
// mode the session is preview + a full-res still stream (RAW16/DNG where the
// device has it, else YUV/PNG); in VIDEO mode it is preview + the HEVC encoder.
// Toggling recreates the session. No-op while a RAW-video device is recording.
void hdr_set_photo_mode(bool photo);

void hdr_start_preview();
void hdr_stop_preview();

// Starts/stops the HDR HEVC encoder (reconfigures the session to add the
// encoder surface as an HLG10 output). Resolution, bitrate, and encoder mode
// are chosen inside HdrCameraSession based on device capabilities.
void hdr_start_recording();
void hdr_stop_recording();

// Fires a 3-shot exposure-bracketed RAW burst; each frame is written as
// `<base_path>_<index>.dng`.
void hdr_take_photo(const char* base_path);

// Asks the Java layer to obtain USB permission for the attached audio DAC and
// open it. The resulting libusb file descriptor (or 0 if none/denied) is then
// available via usb_fd(). Asynchronous — the fd appears once the user grants.
void hdr_request_usb();
int  usb_fd();

// ── Host (CameraActivity) integration ────────────────────────────────────────
// Self-contained JNI calls on the NativeActivity jobject (which is a
// CameraActivity). They attach the calling thread if needed and swallow/clear
// any exception, returning a safe default — so they are no-ops under a plain
// NativeActivity host.

// CameraActivity.cameraOutputDir(): the directory to write captures into
// (already created). Empty string on failure.
std::string host_output_dir(JavaVM* vm, jobject activity);

// CameraActivity.cameraStartVideo(): whether to open in VIDEO mode. false on failure.
bool host_start_video(JavaVM* vm, jobject activity);

// CameraActivity.cameraPhotoMode(): RAW photo output mode
// (0=RAW_BRACKET, 1=RAW_MERGED, 2=HDR_IMAGE). 0 on failure.
int host_photo_mode(JavaVM* vm, jobject activity);

// CameraActivity.cameraPhotoModeUiEnabled(): show the in-scene photo-mode
// selector. true (default-enabled) on failure.
bool host_photo_mode_ui(JavaVM* vm, jobject activity);

// CameraActivity.finishWithResults(String[]): hand the captured files back to the
// host and finish the scene.
void host_finish_with_results(JavaVM* vm, jobject activity,
                              const std::vector<std::string>& paths);

// ── Session capture-file registry ────────────────────────────────────────────
// Exact output paths, recorded as each file is written (DNG/PNG stills from the
// JNI callbacks, the .mkv from the recorder). Thread-safe. Returned to the host
// on exit.
void                     session_files_reset();
void                     session_record_file(const std::string& path);
std::vector<std::string> session_files();

} // namespace jni
