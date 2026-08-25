#pragma once

#include <android/asset_manager.h>
#include <jni.h>
#include <string>
#include <memory>
#include <atomic>
#include <mutex>
#include <vector>
#include <queue>
#include <deque>
#include <functional>

// Pulls in ndkcam::Session and the jni:: sink types the members below use.
#include "../camera/ndk_session.hh"
#include <thread>
#include <condition_variable>

#include "../camera/dng_writer.hh"
#include "../camera/hdr_merge.hh"

namespace cam { class Camera; }
namespace aud { class AudioCapture; }
namespace mux { class Muxer; }
namespace isp { class RawVideoPipeline; }
class Renderer;

namespace rec {

// FINALIZING: the camera has stopped but the offline NLM pipeline is still
// draining its frame backlog into the .mkv (the "Processing…" phase). No new
// recording may start until it returns to PREVIEW.
enum class State { IDLE, PREVIEW, SAVING, FINALIZING };

// Which video pipeline records. RAW_PQ is the native path (RAW16 -> Vulkan
// ISP -> HEVC Main10 PQ full-range); LEGACY_HLG is the Java HLG10 MediaCodec
// path, kept as the fallback for devices without RAW or a P010 encoder.
enum class VideoMode { RAW_PQ, LEGACY_HLG };

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

    // Selects PHOTO vs VIDEO capture for the legacy (non-RAW-video) session: PHOTO
    // brings up preview + a full-res still stream, VIDEO brings up preview + the
    // HEVC encoder. Driven by the UI's PHOTO/VIDEO toggle.
    void set_photo_mode(bool photo);

    // Photo output mode (RAW devices). Everything stays RAW DNG — HDR is developed
    // on a PC:
    //   0 = FAST   — one raw shot -> 1 DNG (instant, motion-proof, full range),
    //   1 = STATIC — 3-shot bracket merged -> 1 DNG (tripod/static, extra stops),
    //   2 = STATIC + RAW set — 3 source DNGs + the merged DNG (PC re-merge).
    // Read at shot time (FAST fires 1 shot, STATIC fires 3).
    void set_photo_output_mode(int m) { photo_output_mode_.store(m); }

    // Still-capture noise reduction (non-RAW devices: the HAL's NR on the
    // full-res YUV still). Default OFF. Remembered here so it survives the
    // session rebuilds a PHOTO/VIDEO toggle causes.
    void set_still_nr(bool on);

    // ── Manual focus ────────────────────────────────────────────────────────
    // Only the native session honours these; on the Java fallback the control
    // is hidden (manual_focus_available() reports false there).
    bool  manual_focus_available() const;
    float max_focus_diopters()     const;
    // `diopters` is 1/m: 0 = infinity, max_focus_diopters() = the closest the
    // lens reaches. Remembered here so it survives the session rebuilds a
    // PHOTO/VIDEO toggle and a REC boundary cause.
    void  set_focus(bool manual, float diopters);
    // Where autofocus has actually left the lens (diopters), for arming MF.
    float reported_focus_diopters() const;
    bool  focus_manual()   const { return focus_manual_.load(); }
    float focus_diopters() const { return focus_diopters_.load(); }

    // ── Focus-check loupe ───────────────────────────────────────────────────
    // Raises the preview stream so a magnified inset shows real sensor detail.
    // Refused while recording or finalizing (it reconfigures the session).
    bool loupe_available() const;
    bool set_loupe(bool on);
    // The largest magnification the high-resolution preview actually backs with
    // real pixels, given how wide the frame is drawn on screen. Callers use it
    // to stop offering a zoom that would only upscale.
    int  loupe_max_factor() const;
    // Display width the preview is drawn at, needed for the ratio above. Set by
    // App when the renderer's surface size is known.
    void set_display_width(int px) { display_w_ = px > 0 ? px : 1; }

    // Re-point (or detach) the renderer that camera frames composite into.
    // Safe to call while recording: the camera keeps running across renderer
    // recreation (e.g. surface destroyed/recreated on rotate or lifecycle churn).
    void set_renderer(Renderer* renderer);

    // Physical device orientation (deg) captured when recording starts. Stored
    // for a future orientation tag in the output (muxer is currently a stub).
    void        set_capture_orientation(int deg) { capture_orientation_ = deg; }

    State       state()    const { return state_.load(); }
    VideoMode   video_mode() const { return video_mode_; }
    int64_t     duration_ms()   const;   // elapsed recording time
    std::string last_error()    const { return last_error_; }

    // Finalize progress [0,100] while state()==FINALIZING (offline NLM drain).
    int         finalize_percent() const;

    // Toggles the RAW pipeline's Bayer denoise pass (A/B comparison). No-op in
    // legacy mode. Toggle between recordings.
    void set_denoise(bool on);

    // Toggles the RAW pipeline's HQ (directional) demosaic vs Malvar (A/B
    // comparison). No-op in legacy mode. Toggle between recordings.
    void set_demosaic_hq(bool on);

    // Toggles the RAW pipeline's chroma-only spatial denoise (A/B comparison).
    // Luma stays byte-identical. No-op in legacy mode. Toggle between recordings.
    void set_chroma(bool on);

private:
    void choose_video_mode();
    // Shared by both pipelines: first codec config opens the muxer; packets
    // are queued for the writer thread.
    void on_video_format(const uint8_t* csd, int len, int w, int h);
    void on_video_packet(const uint8_t* data, int len, int64_t pts_us, bool key);

    // Accumulates one frame of a RAW still bracket; once the whole burst has
    // arrived, merges it into a single HDR DNG on a worker thread. No-op for a
    // single-shot (count < 2). The per-shot source DNGs are still written.
    void accumulate_bracket(const char* path, const uint16_t* data, int w, int h, int stride,
                            const float* neutral, const float* black, int white,
                            int64_t exposure_ns, int iso, int index, int count);

    AAssetManager* assets_;
    JavaVM*        vm_       = nullptr;
    jobject        activity_ = nullptr;   // global ref to NativeActivity
    bool           jni_ready_ = false;    // HdrCameraSession constructed
    Renderer*      renderer_ = nullptr;
    mutable std::mutex renderer_mutex_;   // guards renderer_ (read on camera thread)

    // Native NDK capture session, used for the RAW path when the device can
    // serve it (see start_preview). Null means the Java session is driving.
    std::unique_ptr<ndkcam::Session>  ndk_session_;
    std::atomic<bool>                 still_nr_{false};
    std::atomic<bool>                 focus_manual_{false};
    std::atomic<float>                focus_diopters_{0.0f};
    std::atomic<int>                  display_w_{1080};
    bool                              use_ndk_ = false;
    jni::RawSink                      pending_raw_sink_{};
    jni::RawVideoSink                 pending_raw_video_sink_{};
    jni::RecordSink                   pending_record_sink_{};
    std::unique_ptr<aud::AudioCapture> audio_;
    std::unique_ptr<mux::Muxer>       muxer_;
    std::unique_ptr<isp::RawVideoPipeline> raw_pipeline_;

    VideoMode video_mode_       = VideoMode::LEGACY_HLG;
    bool      mode_chosen_      = false;
    int       configured_fps_   = 30;   // actual fps for the muxer container tag

    std::atomic<State> state_{State::IDLE};
    int                capture_orientation_ = 0;
    int64_t            start_time_ns_ = 0;
    std::string        last_error_;

    // Recording: muxer is opened lazily once the encoder emits its codec config.
    std::string          pending_output_path_;
    std::vector<uint8_t> hvcc_;            // hvcC built from the encoder csd
    std::atomic<bool>    muxer_opened_{false};
    std::mutex           muxer_open_mutex_;   // guards open()/close() sequencing

    // Both video AND audio are muxed on this one thread so disk I/O (cluster
    // flushes, several MB on each keyframe at RAW-PQ bitrates) never blocks the
    // encoder drain OR the FLAC capture thread — blocking either backpressures
    // its source and drops frames/samples. `ts` is nanoseconds for audio and
    // microseconds for video (converted at write); `key` is video-only.
    struct MuxPkt { std::vector<uint8_t> data; int64_t ts; bool key; bool is_audio; };
    std::queue<MuxPkt>      video_q_;
    // Consumed packet buffers, recycled back to the producers. At RAW-PQ bitrates
    // a video access unit is ~1 MB and arrives 30x a second on the encoder drain
    // thread; allocating a fresh vector per packet churned the allocator right
    // next to the code path that must not stall.
    std::vector<std::vector<uint8_t>> pkt_pool_;
    // Both are guarded by video_q_mtx_. Call with the lock held.
    std::vector<uint8_t> take_pkt_buffer(const uint8_t* data, int len);
    void                 recycle_pkt_buffer(std::vector<uint8_t>&& buf);
    static constexpr size_t kPktPoolMax = 24;
    std::mutex              video_q_mtx_;
    std::condition_variable video_q_cv_;
    std::thread             video_writer_;
    bool                    video_writer_run_ = false;

    // Offline finalize: stop_saving() returns immediately and this thread drains
    // the NLM backlog, flushes the encoder, and closes the muxer in the background.
    std::thread             finalize_thread_;

    // Static DNG metadata (CFA, matrices, etc.), loaded once from the camera
    // characteristics; reused for bracketed RAW stills AND the RAW video ISP.
    dng::DngMeta         raw_meta_;
    bool                 raw_meta_loaded_ = false;
    int                  raw_w_ = 0, raw_h_ = 0;

    // RAW still HDR bracket: frames accumulate here until the burst is complete,
    // then merge_raw_bracket() runs on bracket_worker_ (off the camera thread).
    // The worker is a single long-lived consumer of bracket_jobs_ — it both keeps
    // bursts serialized and keeps the camera thread from ever blocking on a
    // merge in progress (a plain join() there stalled capture for hundreds of ms).
    // Develops a linear Bayer plane and writes the BT.2020/PQ JXL viewable copy.
    // `lin` is black-subtracted, stride == width, normalised so 1.0 is the
    // reference exposure's clip point; values above 1.0 are recovered highlights.
    // Synchronous — call it from code already running on bracket_worker_.
    // `clip_level` is where the source saturated, in `lin`'s units: 1.0 for a
    // single shot, a merged bracket's max_boost for the merge. It drives the
    // highlight reconstruction in cam::develop_linear_bayer.

    void bracket_worker_loop();
    void enqueue_bracket_job(std::function<void()> job);
    void stop_bracket_worker();

    std::deque<std::function<void()>> bracket_jobs_;
    std::mutex                        bracket_jobs_mtx_;
    std::condition_variable           bracket_jobs_cv_;
    bool                              bracket_quit_ = false;
    std::vector<hdr::BracketFrame> bracket_frames_;
    std::string                    bracket_base_;     // merged output base (no _idx.dng)
    int                            bracket_count_ = 0;// expected frames in the burst
    std::atomic<int>               photo_output_mode_{0};  // 0=FAST 1=STATIC 2=STATIC+RAW
    std::mutex                     bracket_mtx_;
    std::thread                    bracket_worker_;

    // Codec negotiation result
};

} // namespace rec
