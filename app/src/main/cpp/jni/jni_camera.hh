#pragma once

#include <jni.h>
#include <android/hardware_buffer.h>

#include <functional>

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
// the per-shot dynamic DNG tags (may be null if unavailable). Invoked on the
// camera background thread.
struct RawSink {
    std::function<void(const char* path, const uint16_t* data, int width, int height,
                       int stride_bytes, const float* neutral3, const float* black4,
                       int white)> on_raw;
};

// Constructs the Java session object. `activity` is the NativeActivity jobject
// (a Context). Must be called once before start/stop. Returns false on failure.
bool hdr_init(JavaVM* vm, jobject activity, PreviewSink preview, RecordSink record, RawSink raw);

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

} // namespace jni
