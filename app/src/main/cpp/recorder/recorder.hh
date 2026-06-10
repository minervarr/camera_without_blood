#pragma once

#include <android/asset_manager.h>
#include <jni.h>
#include <string>
#include <memory>
#include <atomic>
#include <mutex>
#include <vector>
#include <queue>
#include <thread>
#include <condition_variable>

#include "../camera/dng_writer.hh"

namespace cam { class Camera; }
namespace aud { class AudioCapture; }
namespace mux { class Muxer; }
class Renderer;

namespace rec {

enum class State { IDLE, PREVIEW, SAVING };

class Recorder {
public:
    // `vm` and `activity` (the NativeActivity jobject / Context) are needed to
    // construct the Java HdrCameraSession that owns the HDR-capable capture
    // session.
    Recorder(AAssetManager* assets, JavaVM* vm, jobject activity);
    ~Recorder();

    bool start_preview(Renderer* renderer);
    void stop_preview();

    bool start_saving(const std::string& output_path);
    void stop_saving();

    bool take_photo(const std::string& output_path);

    // Re-point (or detach) the renderer that camera frames composite into.
    // Safe to call while recording: the camera keeps running across renderer
    // recreation (e.g. surface destroyed/recreated on rotate or lifecycle churn).
    void set_renderer(Renderer* renderer);

    // Physical device orientation (deg) captured when recording starts. Stored
    // for a future orientation tag in the output (muxer is currently a stub).
    void        set_capture_orientation(int deg) { capture_orientation_ = deg; }
    int         capture_orientation() const { return capture_orientation_; }

    State       state()    const { return state_.load(); }
    int64_t     duration_ms()   const;   // elapsed recording time
    std::string last_error()    const { return last_error_; }

private:
    void negotiate_codec();

    AAssetManager* assets_;
    JavaVM*        vm_       = nullptr;
    jobject        activity_ = nullptr;   // global ref to NativeActivity
    bool           jni_ready_ = false;    // HdrCameraSession constructed
    Renderer*      renderer_ = nullptr;
    mutable std::mutex renderer_mutex_;   // guards renderer_ (read on camera thread)

    std::unique_ptr<cam::Camera>      camera_;
    std::unique_ptr<aud::AudioCapture> audio_;
    std::unique_ptr<mux::Muxer>       muxer_;

    std::atomic<State> state_{State::IDLE};
    int                capture_orientation_ = 0;
    int64_t            start_time_ns_ = 0;
    std::string        last_error_;

    // Recording: muxer is opened lazily once the encoder emits its codec config.
    std::string          pending_output_path_;
    std::vector<uint8_t> hvcc_;            // hvcC built from the encoder csd
    std::atomic<bool>    muxer_opened_{false};
    std::mutex           muxer_open_mutex_;   // guards open()/close() sequencing

    // Encoded video is muxed on its own thread so disk I/O (cluster flushes,
    // several MB on each keyframe) never blocks the encoder drain — blocking it
    // backpressures the camera and stalls the preview once per second.
    struct VideoPkt { std::vector<uint8_t> data; int64_t pts_us; bool key; };
    std::queue<VideoPkt>    video_q_;
    std::mutex              video_q_mtx_;
    std::condition_variable video_q_cv_;
    std::thread             video_writer_;
    bool                    video_writer_run_ = false;

    // Static DNG metadata (CFA, matrices, etc.), loaded once from the camera
    // characteristics and reused for every bracketed RAW frame.
    dng::DngMeta         raw_meta_;
    bool                 raw_meta_loaded_ = false;

    // Codec negotiation result
    const char* video_codec_id_ = nullptr;  // "V_MPEGH/ISO/HEVC" or "V_MPEG4/ISO/AVC"
};

} // namespace rec
