#include "recorder.hh"
#include "../camera/camera.hh"
#include "../audio/audio_capture.hh"
#include "../muxer/muxer.hh"
#include "../muxer/hevc_bitstream.hh"
#include "../camera/dng_meta_source.hh"
#include "../jni/jni_camera.hh"

#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <media/NdkImage.h>
#include "../logger.hh"
#include <chrono>
#include <vector>
#include "renderer.hh"

#undef LOG_TAG
#define LOG_TAG "Recorder"

namespace rec {

Recorder::Recorder(AAssetManager* assets, JavaVM* vm, jobject activity)
    : assets_(assets)
    , vm_(vm)
    , camera_(std::make_unique<cam::Camera>())
    , audio_ (std::make_unique<aud::AudioCapture>())
    , muxer_ (std::make_unique<mux::Muxer>())
{
    // Hold a global ref to the activity so it survives across JNI calls.
    if (vm_ && activity) {
        JNIEnv* env = nullptr;
        if (vm_->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK && env)
            activity_ = env->NewGlobalRef(activity);
    }
}

Recorder::~Recorder() {
    stop_preview();
    if (vm_ && activity_) {
        JNIEnv* env = nullptr;
        if (vm_->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK && env)
            env->DeleteGlobalRef(activity_);
        activity_ = nullptr;
    }
}

void Recorder::negotiate_codec() {
    // Try hardware HEVC first, fall back to AVC.
    // AMediaCodec_createEncoderByType returns nullptr if unavailable.
    AMediaCodec* test = AMediaCodec_createEncoderByType("video/hevc");
    if (test) {
        AMediaCodec_delete(test);
        video_codec_id_ = "V_MPEGH/ISO/HEVC";
        LOGI("Codec: HEVC (hardware)");
    } else {
        video_codec_id_ = "V_MPEG4/ISO/AVC";
        LOGI("Codec: AVC (fallback)");
    }
}

bool Recorder::start_preview(Renderer* renderer) {
    if (state_ != State::IDLE) return false;
    if (!vm_ || !activity_) { last_error_ = "No JNI context"; return false; }

    set_renderer(renderer);

    // Construct the Java HdrCameraSession once. Preview frames arrive as
    // AHardwareBuffers and are composited by the renderer; the release closure
    // returns the backing Image to the camera pool when the renderer is done.
    if (!jni_ready_) {
        jni::PreviewSink preview{};
        preview.on_frame = [this](AHardwareBuffer* hb, std::function<void()> release) {
            std::lock_guard<std::mutex> lock(renderer_mutex_);
            if (renderer_) renderer_->update_camera_frame(hb, std::move(release));
            else           release();
        };

        jni::RecordSink record{};
        // First codec config: build hvcC and open the muxer's HDR video track.
        record.on_format = [this](const uint8_t* csd, int len, int w, int h) {
            std::lock_guard<std::mutex> lock(muxer_open_mutex_);
            if (muxer_opened_) return;
            if (!hevc::build_hvcc(csd, len, hvcc_)) {
                LOGE("Failed to build hvcC from csd");
                return;
            }
            mux::VideoTrackConfig vcfg{};
            vcfg.width             = w;
            vcfg.height            = h;
            vcfg.fps               = 30;
            vcfg.codec_id          = "V_MPEGH/ISO/HEVC";
            vcfg.private_data      = hvcc_.data();
            vcfg.private_data_size = static_cast<int>(hvcc_.size());
            vcfg.hdr_hlg           = true;

            // Add the external-mic FLAC track when audio is live. Its CodecPrivate
            // (fLaC + STREAMINFO) is ready as soon as AudioCapture::start() ran.
            mux::AudioTrackConfig acfg{};
            if (audio_->is_capturing() && !audio_->codec_private().empty()) {
                acfg.sample_rate       = audio_->config().sample_rate;
                acfg.channels          = audio_->config().channels;
                acfg.bit_depth         = audio_->config().bit_depth;
                acfg.codec_id          = "A_FLAC";
                acfg.private_data      = audio_->codec_private().data();
                acfg.private_data_size = static_cast<int>(audio_->codec_private().size());
            }
            if (muxer_->open(pending_output_path_, vcfg, acfg)) {
                muxer_opened_ = true;
                LOGI("Muxer opened for HDR HEVC: %s", pending_output_path_.c_str());
            } else {
                LOGE("Muxer open failed");
            }
        };
        // Each access unit: copy onto the video queue and return immediately so
        // the encoder drain (Java) thread never blocks on muxer disk I/O.
        record.on_packet = [this](const uint8_t* data, int len, int64_t pts_us, bool key) {
            {
                std::lock_guard<std::mutex> lk(video_q_mtx_);
                video_q_.push(VideoPkt{ std::vector<uint8_t>(data, data + len), pts_us, key });
            }
            video_q_cv_.notify_one();
        };

        // Bracketed RAW: each frame -> DNG, reusing the native writer. Static
        // tags are read once from the camera characteristics; per-shot neutral/
        // black come from the Java capture result.
        jni::RawSink raw{};
        raw.on_raw = [this](const char* path, const uint16_t* data, int w, int h, int stride,
                            const float* neutral, const float* black, int white) {
            if (!raw_meta_loaded_) {
                int rw = 0, rh = 0;
                raw_meta_loaded_ = dng::load_static_meta(raw_meta_, rw, rh);
            }
            dng::DngMeta m = raw_meta_;
            m.width = w; m.height = h; m.stride_bytes = stride;
            if (neutral) { for (int i = 0; i < 3; ++i) m.as_shot_neutral[i] = neutral[i]; m.has_neutral = true; }
            if (black)   { for (int i = 0; i < 4; ++i) m.black_level[i] = black[i]; }
            if (white > 0) m.white_level = white;
            if (!dng::write_dng(path, data, m))
                LOGE("Failed to write bracketed DNG: %s", path);
        };

        if (!jni::hdr_init(vm_, activity_, preview, record, raw)) {
            last_error_ = "Failed to init HdrCameraSession";
            return false;
        }
        jni_ready_ = true;
    }

    jni::hdr_start_preview();
    // Prompt for USB DAC access now so the fd is ready by the time we record.
    jni::hdr_request_usb();
    state_ = State::PREVIEW;
    return true;
}

void Recorder::stop_preview() {
    if (state_ == State::SAVING) {
        stop_saving();
    }
    if (state_ == State::IDLE) return;

    jni::hdr_stop_preview();

    {
        std::lock_guard<std::mutex> lock(renderer_mutex_);
        if (renderer_) renderer_->clear_camera_frames();
        renderer_ = nullptr;
    }

    state_ = State::IDLE;
}

bool Recorder::start_saving(const std::string& output_path) {
    if (state_ != State::PREVIEW) return false;

    // The muxer is opened lazily by the record sink's on_format callback once
    // the encoder emits its codec config (we need the hvcC for the video track).
    pending_output_path_ = output_path;
    muxer_opened_        = false;
    hvcc_.clear();

    start_time_ns_ = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    // Muxer-writer thread: drains the video queue, converts Annex B → length-
    // prefixed, and writes (including cluster flushes) off the encoder path.
    video_writer_run_ = true;
    video_writer_ = std::thread([this] {
        for (;;) {
            VideoPkt pkt;
            {
                std::unique_lock<std::mutex> lk(video_q_mtx_);
                video_q_cv_.wait(lk, [this] { return !video_q_.empty() || !video_writer_run_; });
                if (video_q_.empty()) { if (!video_writer_run_) break; else continue; }
                pkt = std::move(video_q_.front());
                video_q_.pop();
            }
            if (!muxer_opened_) continue;   // muxer stays open until after we join
            std::vector<uint8_t> framed;
            hevc::annexb_to_length_prefixed(pkt.data.data(), static_cast<int>(pkt.data.size()), framed);
            muxer_->write_video(framed.data(), static_cast<int>(framed.size()), pkt.pts_us * 1000, pkt.key);
        }
    });

    // External USB-mic FLAC audio. If no mic is attached, record video-only.
    // start() runs the FLAC init so codec_private() is ready before the muxer
    // opens (lazily, on the first video config). Frames arriving before the
    // muxer opens are dropped by the guard below.
    // Prefer the external USB DAC if one was granted; otherwise fall back to the
    // phone's built-in microphone (AAudio).
    int usb_fd = jni::usb_fd();
    bool audio_ok = (usb_fd > 0) ? audio_->open_fd(usb_fd) : audio_->open_internal();
    if (audio_ok) {
        audio_->start([this](const uint8_t* data, int bytes, int64_t ts) {
            if (muxer_opened_) muxer_->write_audio(data, bytes, ts);  // muxer is internally locked
        });
        LOGI("Audio source: %s", usb_fd > 0 ? "USB DAC" : "internal mic");
    } else {
        LOGE("No audio source — recording video only");
    }

    // Start the HDR HEVC encoder. Resolution/bitrate/mode are chosen by
    // HdrCameraSession based on device capabilities probed at startPreview.
    jni::hdr_start_recording();

    state_ = State::SAVING;
    LOGI("Recording started: %s", output_path.c_str());
    return true;
}

void Recorder::stop_saving() {
    if (state_ != State::SAVING) return;

    jni::hdr_stop_recording();   // stops encoder + joins its drain thread (no more packets)

    audio_->stop();              // stop FLAC frames before closing the muxer
    audio_->close();

    // Drain and stop the muxer-writer thread (writes any queued tail), then close.
    { std::lock_guard<std::mutex> lk(video_q_mtx_); video_writer_run_ = false; }
    video_q_cv_.notify_one();
    if (video_writer_.joinable()) video_writer_.join();

    {
        std::lock_guard<std::mutex> lock(muxer_open_mutex_);
        if (muxer_opened_) { muxer_->close(); muxer_opened_ = false; }
    }

    state_ = State::PREVIEW;
    LOGI("Recording stopped");
}

bool Recorder::take_photo(const std::string& output_path) {
    if (!jni_ready_ || state_ == State::IDLE) return false;
    // Java appends "_<index>.dng" per bracketed frame; strip our ".dng" suffix.
    std::string base = output_path;
    auto dot = base.rfind(".dng");
    if (dot != std::string::npos) base.erase(dot);
    jni::hdr_take_photo(base.c_str());
    return true;
}

void Recorder::set_renderer(Renderer* renderer) {
    std::lock_guard<std::mutex> lock(renderer_mutex_);
    renderer_ = renderer;
}

int64_t Recorder::duration_ms() const {
    if (state_ == State::IDLE) return 0;
    auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    return (now - start_time_ns_) / 1'000'000;
}

} // namespace rec
