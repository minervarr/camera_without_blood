#include "recorder.hh"
#include "../camera/camera.hh"
#include "../audio/audio_capture.hh"
#include "../muxer/muxer.hh"
#include "../muxer/hevc_bitstream.hh"
#include "../camera/dng_meta_source.hh"
#include "../jni/jni_camera.hh"
#include "../isp/raw_video_pipeline.hh"

#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <media/NdkImage.h>
#include "../cpu_affinity.hh"
#include "../logger.hh"
#include <chrono>
#include <deque>
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
    // A background finalize may still be draining the NLM backlog into the file —
    // wait for it (and its use of our members) to complete before we tear down.
    if (finalize_thread_.joinable()) finalize_thread_.join();
    if (vm_ && activity_) {
        JNIEnv* env = nullptr;
        if (vm_->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK && env)
            env->DeleteGlobalRef(activity_);
        activity_ = nullptr;
    }
}

// First codec config from whichever pipeline is active: build hvcC and open
// the muxer with colour tags matching what that pipeline actually encodes.
void Recorder::on_video_format(const uint8_t* csd, int len, int w, int h) {
    std::lock_guard<std::mutex> lock(muxer_open_mutex_);
    if (muxer_opened_) return;
    if (!hevc::build_hvcc(csd, len, hvcc_)) {
        LOGE("Failed to build hvcC from csd");
        return;
    }
    mux::VideoTrackConfig vcfg{};
    vcfg.width             = w;
    vcfg.height            = h;
    // Use the pipeline's configured fps so the container matches the actual
    // recording rate (was hardcoded to 30, causing sped-up playback when the
    // camera/pipeline delivered fewer fps).
    vcfg.fps               = configured_fps_;
    vcfg.codec_id          = "V_MPEGH/ISO/HEVC";
    vcfg.private_data      = hvcc_.data();
    vcfg.private_data_size = static_cast<int>(hvcc_.size());
    vcfg.color             = (video_mode_ == VideoMode::RAW_PQ)
                                 ? mux::Color::HDR_PQ_FULL
                                 : mux::Color::HDR_HLG_LIMITED;

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
        LOGI("Muxer opened (%s): %s",
             video_mode_ == VideoMode::RAW_PQ ? "PQ full" : "HLG limited",
             pending_output_path_.c_str());
    } else {
        LOGE("Muxer open failed");
    }
}

// Each access unit: copy onto the mux queue and return immediately so the
// encoder drain thread never blocks on muxer disk I/O.
void Recorder::on_video_packet(const uint8_t* data, int len, int64_t pts_us, bool key) {
    {
        std::lock_guard<std::mutex> lk(video_q_mtx_);
        video_q_.push(MuxPkt{ std::vector<uint8_t>(data, data + len), pts_us, key, /*is_audio*/false });
    }
    video_q_cv_.notify_one();
}

// Picks RAW_PQ when the device streams RAW16 and has a P010 HEVC Main10
// encoder (probed by RawVideoPipeline::init); LEGACY_HLG otherwise.
void Recorder::choose_video_mode() {
    if (mode_chosen_) return;
    mode_chosen_ = true;

    if (!raw_meta_loaded_)
        raw_meta_loaded_ = dng::load_static_meta(raw_meta_, raw_w_, raw_h_);

    video_mode_ = VideoMode::LEGACY_HLG;
    if (raw_meta_loaded_ && raw_w_ > 0 && raw_h_ > 0) {
        raw_pipeline_ = std::make_unique<isp::RawVideoPipeline>();
        if (raw_pipeline_->init(assets_, raw_meta_, raw_w_, raw_h_, 30)) {
            raw_pipeline_->set_callbacks(
                [this](const uint8_t* csd, int len, int w, int h) {
                    on_video_format(csd, len, w, h);
                },
                [this](const uint8_t* data, int len, int64_t pts_us, bool key) {
                    on_video_packet(data, len, pts_us, key);
                });
            video_mode_ = VideoMode::RAW_PQ;
            configured_fps_ = raw_pipeline_->fps();
        } else {
            raw_pipeline_.reset();
        }
    }
    LOGI("Video mode: %s",
         video_mode_ == VideoMode::RAW_PQ ? "RAW -> native ISP (PQ)" : "legacy HLG10");
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

        // Legacy Java-encoder sink; the RAW pipeline calls the same member
        // functions directly (set in choose_video_mode).
        jni::RecordSink record{};
        record.on_format = [this](const uint8_t* csd, int len, int w, int h) {
            on_video_format(csd, len, w, h);
        };
        record.on_packet = [this](const uint8_t* data, int len, int64_t pts_us, bool key) {
            on_video_packet(data, len, pts_us, key);
        };

        // Bracketed RAW: each frame -> DNG, reusing the native writer. Static
        // tags are read once from the camera characteristics; per-shot neutral/
        // black come from the Java capture result.
        jni::RawSink raw{};
        raw.on_raw = [this](const char* path, const uint16_t* data, int w, int h, int stride,
                            const float* neutral, const float* black, int white) {
            if (!raw_meta_loaded_)
                raw_meta_loaded_ = dng::load_static_meta(raw_meta_, raw_w_, raw_h_);
            dng::DngMeta m = raw_meta_;
            m.width = w; m.height = h; m.stride_bytes = stride;
            if (neutral) { for (int i = 0; i < 3; ++i) m.as_shot_neutral[i] = neutral[i]; m.has_neutral = true; }
            if (black)   { for (int i = 0; i < 4; ++i) m.black_level[i] = black[i]; }
            if (white > 0) m.white_level = white;
            if (!dng::write_dng(path, data, m))
                LOGE("Failed to write bracketed DNG: %s", path);
        };

        // Streaming RAW16 video frames -> the native ISP/encoder (raw mode).
        jni::RawVideoSink raw_video{};
        raw_video.on_frame = [this](const uint8_t* data, int w, int h,
                                    int stride, int64_t ts_ns) {
            if (state_ == State::SAVING && raw_pipeline_)
                raw_pipeline_->on_frame(data, w, h, stride, ts_ns);
        };
        raw_video.on_neutral = [this](const float neutral[3]) {
            if (raw_pipeline_) raw_pipeline_->set_neutral(neutral);
        };

        if (!jni::hdr_init(vm_, activity_, preview, record, raw, raw_video)) {
            last_error_ = "Failed to init HdrCameraSession";
            return false;
        }
        jni_ready_ = true;
    }

    // Decide RAW-vs-legacy before the Java session builds its capture session;
    // raw mode changes the stream configuration (preview + RAW16, no encoder).
    choose_video_mode();
    jni::hdr_set_raw_video(video_mode_ == VideoMode::RAW_PQ);

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

    // A prior recording's finalize must be fully done before reusing the pipeline.
    if (finalize_thread_.joinable()) finalize_thread_.join();

    // RAW mode: spin up the native ISP/encoder before frames start flowing. The
    // frame store spills overflow next to the output file (chunks are deleted as
    // they're consumed).
    if (video_mode_ == VideoMode::RAW_PQ) {
        auto slash = output_path.find_last_of('/');
        std::string spill_dir = (slash == std::string::npos) ? "." : output_path.substr(0, slash);
        if (!raw_pipeline_ || !raw_pipeline_->start(spill_dir)) {
            last_error_ = "RAW pipeline start failed";
            LOGE("%s", last_error_.c_str());
            return false;
        }
    }

    start_time_ns_ = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    // Muxer-writer thread: drains the video queue, converts Annex B → length-
    // prefixed, and writes (including cluster flushes) off the encoder path.
    video_writer_run_ = true;
    video_writer_ = std::thread([this] {
        // Keep cluster flushes + Annex B framing off the little cores so a disk
        // flush can't get parked and backpressure the encoder drain.
        cpuaff::pin_current_thread_to_fast_cores("Recorder");
        // Audio is captured in real time and finishes at Stop, but video packets
        // trickle out during the offline finalize. Buffer audio and emit it gated
        // by the latest video PTS so the muxer's clusters stay timestamp-ordered
        // (without this, finalize would write all audio, then video at far earlier
        // timestamps — a broken interleave). FLAC is tiny, so buffering a whole
        // clip's audio is cheap.
        std::deque<MuxPkt> audio_buf;
        int64_t max_video_ns = INT64_MIN;
        auto flush_audio_upto = [&](int64_t upto_ns) {
            while (!audio_buf.empty() && audio_buf.front().ts <= upto_ns) {
                MuxPkt a = std::move(audio_buf.front());
                audio_buf.pop_front();
                if (muxer_opened_)
                    muxer_->write_audio(a.data.data(), static_cast<int>(a.data.size()), a.ts);
            }
        };
        for (;;) {
            MuxPkt pkt;
            {
                std::unique_lock<std::mutex> lk(video_q_mtx_);
                video_q_cv_.wait(lk, [this] { return !video_q_.empty() || !video_writer_run_; });
                if (video_q_.empty()) { if (!video_writer_run_) break; else continue; }
                pkt = std::move(video_q_.front());
                video_q_.pop();
            }
            if (pkt.is_audio) { audio_buf.push_back(std::move(pkt)); continue; }  // emit later, gated by video PTS
            if (!muxer_opened_) continue;   // muxer stays open until after we join
            const int64_t v_ns = pkt.ts * 1000;          // video MuxPkt.ts is microseconds
            if (v_ns > max_video_ns) max_video_ns = v_ns;
            flush_audio_upto(max_video_ns);              // all audio up to this video frame, in order
            std::vector<uint8_t> framed;
            hevc::annexb_to_length_prefixed(pkt.data.data(), static_cast<int>(pkt.data.size()), framed);
            muxer_->write_video(framed.data(), static_cast<int>(framed.size()), v_ns, pkt.key);
        }
        flush_audio_upto(INT64_MAX);   // drained: write any audio tail past the last video frame
    });

    // External USB-mic FLAC audio. If no mic is attached, record video-only.
    // start() runs the FLAC init so codec_private() is ready before the muxer
    // opens (lazily, on the first video config). Frames arriving before the
    // muxer opens are dropped by the guard below.
    // Prefer the external USB DAC if one was granted; otherwise fall back to the
    // phone's built-in microphone (AAudio).
    int usb_fd = jni::usb_fd();
    bool audio_ok;
    if (usb_fd > 0) {
        audio_ok = audio_->open_fd(usb_fd);
        if (!audio_ok) {
            // A USB device is attached but exposes no usable capture stream (e.g. a
            // playback-only DAC). Don't silently record video-only — fall back to
            // the phone's built-in mic so the clip still gets audio.
            LOGI("USB device has no capture input (fd=%d); using internal mic", usb_fd);
            audio_ok = audio_->open_internal();
        }
    } else {
        audio_ok = audio_->open_internal();
    }
    if (audio_ok) {
        audio_->start([this](const uint8_t* data, int bytes, int64_t ts) {
            // Enqueue for the writer thread — never call write_audio here, or the
            // FLAC capture thread blocks on the muxer mutex during multi-MB video
            // cluster flushes (long enough to overrun the audio device at RAW-PQ
            // bitrates). ts is already nanoseconds.
            {
                std::lock_guard<std::mutex> lk(video_q_mtx_);
                video_q_.push(MuxPkt{ std::vector<uint8_t>(data, data + bytes), ts, false, /*is_audio*/true });
            }
            video_q_cv_.notify_one();
        });
        LOGI("Audio source: %s", usb_fd > 0 ? "USB DAC" : "internal mic");
        
        if (audio_->is_capturing()) {
            // DeepFilterNet denoise side-car (<clip>_ai.flac). Surface the outcome:
            // a silent failure here is indistinguishable from "AI audio doesn't work".
            bool armed = audio_->start_ai_recording(output_path, assets_);
            LOGI("AI denoise: %s", armed
                 ? "ARMED — denoised <clip>_ai.flac will be written at finalize"
                 : "DISABLED — model load/setup failed (recording continues, no side-car)");
        }
    } else {
        LOGE("No audio source — recording video only");
    }

    // Start frame delivery. Legacy: the Java MediaCodec encoder. RAW mode: the
    // Java session just adds the RAW16 stream to the repeating request and the
    // frames land in raw_pipeline_.
    jni::hdr_start_recording();

    state_ = State::SAVING;
    LOGI("Recording started (%s): %s",
         video_mode_ == VideoMode::RAW_PQ ? "RAW PQ" : "HLG", output_path.c_str());
    return true;
}

void Recorder::stop_saving() {
    if (state_ != State::SAVING) return;

    jni::hdr_stop_recording();   // stops frame delivery (and the Java encoder, legacy mode)

    audio_->stop();              // audio capture is real-time — it's complete at Stop
    audio_->close();

    // Enter the offline finalize phase: the camera has stopped, but the NLM
    // pipeline still has a frame backlog to develop. Drain it on a background
    // thread so the UI thread stays responsive (it shows "Processing…%"); the
    // muxer + writer stay alive until every video packet has been emitted.
    state_ = State::FINALIZING;
    if (finalize_thread_.joinable()) finalize_thread_.join();
    finalize_thread_ = std::thread([this] {
        cpuaff::pin_current_thread_to_fast_cores("Finalize");
        // Seals the store, drains the whole backlog through NLM+ISP+encode, EOS-
        // flushes, and joins the pipeline threads. Blocks for the finalize duration
        // — that's the whole point, and why this runs off the UI thread.
        if (video_mode_ == VideoMode::RAW_PQ && raw_pipeline_)
            raw_pipeline_->stop();

        // All video packets are now queued. Drain + stop the writer (it flushes the
        // buffered audio tail on exit), then close the muxer.
        { std::lock_guard<std::mutex> lk(video_q_mtx_); video_writer_run_ = false; }
        video_q_cv_.notify_one();
        if (video_writer_.joinable()) video_writer_.join();
        {
            std::lock_guard<std::mutex> lock(muxer_open_mutex_);
            if (muxer_opened_) { muxer_->close(); muxer_opened_ = false; }
        }
        // Offline DeepFilterNet denoise of the whole clip -> <clip>_ai.flac side-car.
        // Heavy + slower-than-realtime, so it runs here on the finalize thread (the
        // UI keeps showing "Processing…"). The .mkv's clean audio track is already
        // written; this produces the denoised companion track.
        audio_->finalize_denoise();
        state_ = State::PREVIEW;
        LOGI("Recording finalized");
    });
    LOGI("Recording stopped — finalizing in background");
}

int Recorder::finalize_percent() const {
    if (video_mode_ != VideoMode::RAW_PQ || !raw_pipeline_) return 100;
    const uint64_t total = raw_pipeline_->backlog_total();
    const uint64_t done  = raw_pipeline_->backlog_done();
    if (total == 0) return 0;
    return done >= total ? 100 : static_cast<int>(done * 100 / total);
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

void Recorder::set_denoise(bool on) {
    if (raw_pipeline_) raw_pipeline_->set_denoise(on);
}

void Recorder::set_demosaic_hq(bool on) {
    if (raw_pipeline_) raw_pipeline_->set_demosaic_hq(on);
}

void Recorder::set_temporal(bool on) {
    if (raw_pipeline_) raw_pipeline_->set_temporal(on);
}

void Recorder::set_chroma(bool on) {
    if (raw_pipeline_) raw_pipeline_->set_chroma(on);
}


int64_t Recorder::duration_ms() const {
    if (state_ == State::IDLE) return 0;
    auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    return (now - start_time_ns_) / 1'000'000;
}

} // namespace rec
