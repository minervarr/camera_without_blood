#pragma once

#include <media/NdkMediaCodec.h>
#include <android/native_window.h>

#include "../jni/jni_camera.hh"

#include <atomic>
#include <cstdint>
#include <thread>

namespace ndkcam {

// Native HEVC encoder fed directly from a camera capture target.
//
// This replaces the Java MediaCodec half of the legacy path. It keeps that
// path's one genuine virtue — the camera writes straight into the encoder's
// input surface, so there is no readback and no copy — while fixing what the
// Java side got wrong:
//
//   * Resolution was chosen by a MediaCodecInfo heuristic that came out
//     conservative (1440x1088 on a device whose encoder advertises 2560x1440
//     at 30 fps). Here the size is resolved EMPIRICALLY: candidates are tried
//     largest-first and the first one the codec actually accepts wins, so the
//     answer is what the hardware does rather than what a capability table
//     claims.
//   * Bitrate was left below the encoder's ceiling.
//
// It deliberately does NOT try to produce 10-bit: devices that reach this path
// have no dynamic-range profile and, on the verified one, no Main10 encoder
// either. Claiming HLG10 here (as the mode name used to) was fiction.
class Encoder {
public:
    Encoder() = default;
    ~Encoder();

    Encoder(const Encoder&)            = delete;
    Encoder& operator=(const Encoder&) = delete;

    // Picks the largest candidate size the codec accepts, configures it and
    // creates the input surface. `candidates` is ordered largest-first.
    struct Size { int32_t w, h; };
    bool configure(const Size* candidates, int count, int32_t fps, jni::RecordSink sink);

    // The window to add to the capture request while recording. Null until
    // configure() succeeds.
    ANativeWindow* input_surface() const { return surface_; }

    bool start();
    void stop();

    int32_t width()  const { return w_; }
    int32_t height() const { return h_; }
    int32_t bitrate() const { return bitrate_; }

private:
    void drain_loop();

    AMediaCodec*   codec_   = nullptr;
    ANativeWindow* surface_ = nullptr;
    jni::RecordSink sink_;

    int32_t w_ = 0, h_ = 0, bitrate_ = 0;

    std::atomic<bool> running_{false};
    std::thread       drain_;
};

} // namespace ndkcam
