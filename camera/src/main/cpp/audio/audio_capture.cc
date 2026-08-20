#include "audio_capture.hh"
#include "usb_audio.h"

#include <FLAC/stream_encoder.h>
#include <aaudio/AAudio.h>
#include <android/asset_manager.h>
#include "df_net.hh"

#include "logger.hh"
#include <algorithm>
#include <chrono>
#include <ctime>
#include <thread>
#include <cstring>
#undef LOG_TAG
#define LOG_TAG "AudioCapture"

namespace aud {

// Samples per FLAC frame — 3840 is a good balance of latency vs overhead.
// It is exactly 8 chunks of 480 samples, required by RNNoise.
static constexpr int FRAME_SAMPLES = 3840;

// Audio input-latency compensation. The first buffer the app reads holds sound
// captured ~one input buffer earlier (ADC + USB/driver buffering), so anchoring
// to monotonic_ns() at first read stamps audio LATER than the real acoustic
// event — picture leads sound. Advance the anchor by this to realign. Calibrate
// per device: raise it if sound still trails the picture, lower it if it leads.
static constexpr int64_t kInputLatencyNs = 50'000'000;  // 50 ms

// CLOCK_MONOTONIC nanoseconds (== std::chrono::steady_clock on Android). This is
// the domain the video encoder PTS actually lands in on this pipeline: although the
// camera advertises SENSOR timestamp source REALTIME, anchoring audio to BOOTTIME
// made audio diverge from video by the device's (large) suspend time, overflowing
// the Matroska 16-bit block delta and aborting. MONOTONIC keeps A/V on one clock.
static int64_t monotonic_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}


AudioCapture::~AudioCapture() { close(); stop_ai_recording(); }

bool AudioCapture::open_fd(int fd, const AudioConfig& cfg) {
    cfg_ = cfg;
    if (fd <= 0) { LOGE("open_fd: invalid fd %d", fd); return false; }

    auto* drv = new UsbAudioDriver();
    if (!drv->open(fd) || (drv->parseDescriptors(), !drv->hasCaptureFormats())) {
        // close() before delete: a successful open(fd) has adopted the fd and a
        // libusb event-thread reference, and ~UsbAudioDriver does not release
        // them — dropping straight to delete leaked both.
        drv->close();
        delete drv;
        LOGE("open_fd: no USB capture formats (fd=%d)", fd);
        return false;
    }

    // Pick the HIGHEST quality the ADC advertises (debug: max everything).
    auto rates  = drv->getCaptureRates();
    auto depths = drv->getCaptureBitDepths();
    auto chans  = drv->getCaptureChannelCounts();
    int rate  = rates.empty()  ? cfg_.sample_rate : *std::max_element(rates.begin(),  rates.end());
    int depth = depths.empty() ? cfg_.bit_depth   : *std::max_element(depths.begin(), depths.end());
    int ch    = chans.empty()  ? cfg_.channels    : *std::max_element(chans.begin(),  chans.end());
    drv->configureCapture(rate, ch, depth);

    // Adopt the device's actual negotiated format. Crucially use the SUBSLOT size
    // (the on-wire bytes per sample) to derive bit depth, so the PCM stride and
    // FLAC bits_per_sample exactly match the bytes the ADC delivers.
    cfg_.sample_rate = drv->getConfiguredCaptureRate();
    cfg_.channels    = drv->getConfiguredCaptureChannels();
    int subslot      = drv->getConfiguredCaptureSubslotSize();
    cfg_.bit_depth   = subslot > 0 ? subslot * 8 : drv->getConfiguredCaptureBitDepth();
    driver_ = drv;
    LOGI("USB audio (max) fd=%d → %dHz %dch %dbit (subslot=%d)",
         fd, cfg_.sample_rate, cfg_.channels, cfg_.bit_depth, subslot);
    return true;
}

bool AudioCapture::open_internal(const AudioConfig& cfg) {
    cfg_ = cfg;
    AAudioStreamBuilder* builder = nullptr;
    if (AAudio_createStreamBuilder(&builder) != AAUDIO_OK || !builder) {
        LOGE("AAudio_createStreamBuilder failed");
        return false;
    }
    AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_INPUT);
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setSampleRate(builder, cfg_.sample_rate);
    AAudioStreamBuilder_setChannelCount(builder, 2);   // request stereo; may get mono
    // (Input preset left at the default — setInputPreset is API 28; minSdk is 26.)
    AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);

    AAudioStream* stream = nullptr;
    aaudio_result_t r = AAudioStreamBuilder_openStream(builder, &stream);
    AAudioStreamBuilder_delete(builder);
    if (r != AAUDIO_OK || !stream) {
        LOGE("AAudio openStream failed: %s", AAudio_convertResultToText(r));
        return false;
    }

    aaudio_stream_   = stream;
    use_internal_    = true;
    cfg_.sample_rate = AAudioStream_getSampleRate(stream);
    cfg_.channels    = AAudioStream_getChannelCount(stream);
    cfg_.bit_depth   = 16;  // PCM_I16
    LOGI("Internal mic opened: %dHz %dch 16bit", cfg_.sample_rate, cfg_.channels);
    return true;
}

void AudioCapture::close() {
    stop();
    if (aaudio_stream_) {
        AAudioStream_close(static_cast<AAudioStream*>(aaudio_stream_));
        aaudio_stream_ = nullptr;
    }
    use_internal_ = false;
    if (encoder_) { FLAC__stream_encoder_delete(encoder_); encoder_ = nullptr; }
    if (driver_)  { driver_->close(); delete driver_; driver_ = nullptr; }
}

bool AudioCapture::start(FlacFrameCallback cb) {
    if (capturing_) return true;
    callback_ = std::move(cb);

    // Reset the sample-accurate timestamp state for this recording.
    samples_emitted_ = 0;
    anchored_        = false;
    audio_base_ns_   = 0;

    // ── Configure + init the FLAC encoder ───────────────────────────────────────
    // Guard a degenerate negotiated format (e.g. a USB capture alt configureCapture
    // couldn't actually set, leaving rate/bits at 0). A 0 rate would also
    // divide-by-zero in capture_loop's timestamp, and 0 bits fails init outright.
    //
    // The depth is restricted to the widths capture_loop actually decodes (16/24/
    // 32-bit little-endian PCM). The old "4..32" range let two broken formats
    // through: below 8 bits, bytes_per_sample floored to 0 and capture_loop
    // divided by zero on frame_stride; at 8 bits, no conversion branch matched
    // and every sample stayed 0 — a silently silent recording.
    if (cfg_.sample_rate <= 0 || cfg_.channels < 1 || cfg_.channels > 8 ||
        (cfg_.bit_depth != 16 && cfg_.bit_depth != 24 && cfg_.bit_depth != 32)) {
        LOGE("Refusing audio: bad format rate=%d ch=%d bits=%d (need 16/24/32-bit)",
             cfg_.sample_rate, cfg_.channels, cfg_.bit_depth);
        return false;
    }

    // (Re)create + init the encoder. streamable_subset=true gives the most
    // compatible stream, but its subset forbids sample rates >= 655360 Hz — which
    // high-end USB DACs (705.6/768 kHz) advertise and we negotiate as "max
    // everything". On any subset rejection, retry with the subset off: the stream
    // stays valid + lossless (FLAC handles up to 1048575 Hz / 32-bit), just outside
    // the "every decoder must handle this" subset. Without this the init fails,
    // codec_private() stays empty, and the muxer drops the whole audio track (no
    // .mkv audio, hence no AI side-car).
    header_buf_.clear();
    auto init_encoder = [&](bool subset) -> FLAC__StreamEncoderInitStatus {
        encoder_ = FLAC__stream_encoder_new();
        if (!encoder_) return FLAC__STREAM_ENCODER_INIT_STATUS_ENCODER_ERROR;
        FLAC__stream_encoder_set_channels(encoder_, cfg_.channels);
        FLAC__stream_encoder_set_bits_per_sample(encoder_, cfg_.bit_depth);
        FLAC__stream_encoder_set_sample_rate(encoder_, cfg_.sample_rate);
        FLAC__stream_encoder_set_compression_level(encoder_, 5); // 0=fast … 8=best
        FLAC__stream_encoder_set_blocksize(encoder_, FRAME_SAMPLES);
        FLAC__stream_encoder_set_streamable_subset(encoder_, subset);
        return FLAC__stream_encoder_init_stream(
            encoder_, on_flac_write,
            nullptr, nullptr, nullptr,  // seek/tell/metadata — streaming, none needed
            this);
    };

    bool subset = true;
    auto status = init_encoder(/*subset=*/true);
    if (status != FLAC__STREAM_ENCODER_INIT_STATUS_OK) {
        if (encoder_) { FLAC__stream_encoder_delete(encoder_); encoder_ = nullptr; }
        header_buf_.clear();
        subset = false;
        status = init_encoder(/*subset=*/false);
    }
    if (status != FLAC__STREAM_ENCODER_INIT_STATUS_OK) {
        LOGE("FLAC encoder init failed (rate=%d ch=%d bits=%d): status %d",
             cfg_.sample_rate, cfg_.channels, cfg_.bit_depth, status);
        if (encoder_) { FLAC__stream_encoder_delete(encoder_); encoder_ = nullptr; }
        return false;
    }
    LOGI("FLAC encoder ready: %dHz %dch %dbit (%s)",
         cfg_.sample_rate, cfg_.channels, cfg_.bit_depth,
         subset ? "subset" : "non-subset");

    capturing_ = true;
    soxr_in_buf_.resize(FRAME_SAMPLES * cfg_.channels);
    
    if (use_internal_ && aaudio_stream_)
        AAudioStream_requestStart(static_cast<AAudioStream*>(aaudio_stream_));
    else if (driver_) driver_->startCapture();
    capture_thread_ = std::thread([this]{ capture_loop(); });
    LOGI("Audio capture + FLAC encoding started");
    return true;
}

void AudioCapture::stop() {
    if (!capturing_) return;
    capturing_ = false;
    if (use_internal_ && aaudio_stream_)
        AAudioStream_requestStop(static_cast<AAudioStream*>(aaudio_stream_));
    else if (driver_) driver_->stopCapture();
    
    if (capture_thread_.joinable()) capture_thread_.join();

    // The denoise runs later in finalize_denoise() (heavy, off the UI thread); the
    // capture buffer in df_buf_ is complete now that the capture thread has joined.
    if (encoder_) FLAC__stream_encoder_finish(encoder_);
    LOGI("Audio capture stopped");
}

bool AudioCapture::start_ai_recording(const std::string& path, ::AAssetManager* assets) {
    std::lock_guard<std::mutex> lock(ai_mutex_);
    if (df_) return true;

    // Load DeepFilterNet (enc/erb_dec/df_dec). If the model is unavailable the
    // recording proceeds without the denoise side-car (the .mkv clean track is
    // unaffected). The FLAC encoders are created later, in finalize.
    df_ = std::make_unique<DfNet>();
    if (!df_->init(assets)) {
        LOGE("DeepFilterNet model load failed — no denoise side-car this clip");
        df_.reset();
        return false;
    }

    // Resampler: native capture rate -> 48 kHz (DeepFilterNet's fixed rate).
    soxr_error_t err;
    soxr_io_spec_t io_spec = soxr_io_spec(SOXR_FLOAT32_I, SOXR_FLOAT32_I);
    soxr_quality_spec_t q_spec = soxr_quality_spec(SOXR_HQ, 0);
    soxr_ = soxr_create(cfg_.sample_rate, 48000, cfg_.channels, &err, &io_spec, &q_spec, nullptr);
    if (err) {
        LOGE("SoX Resampler init failed: %s", soxr_strerror(err));
        df_.reset();
        return false;
    }

    // Per-block resample scratch (soxr_in_buf_ is pre-sized in start()), and the
    // whole-clip 48 kHz capture buffer the model runs over at finalize.
    size_t max_out_samples = static_cast<size_t>((FRAME_SAMPLES * 48000.0) / cfg_.sample_rate) + 100;
    soxr_out_buf_.resize(max_out_samples * cfg_.channels);
    df_buf_.assign(cfg_.channels, std::vector<float>());

    // Derive the two A/B side-car paths from the .mkv output path.
    auto derive = [&](const char* suffix) {
        std::string p = path;
        const std::string ext = ".mkv";
        if (p.length() >= ext.length() &&
            p.compare(p.length() - ext.length(), ext.length(), ext) == 0)
            p = p.substr(0, p.length() - ext.length());
        return p + suffix;
    };
    ai_path_ = derive("_ai.flac");   // DeepFilterNet + post-filter baked in

    LOGI("AI denoise armed -> %s", ai_path_.c_str());
    return true;
}

// Encode per-channel 48 kHz float audio to a standalone 24-bit FLAC. No gain or
// normalization is applied — the denoised voice keeps its natural dynamic range.
bool AudioCapture::write_flac_file(const std::string& path,
                                   const std::vector<std::vector<float>>& den, size_t n) {
    const int ch = cfg_.channels;
    FLAC__StreamEncoder* enc = FLAC__stream_encoder_new();
    if (!enc) { LOGE("FLAC new failed: %s", path.c_str()); return false; }
    FLAC__stream_encoder_set_channels(enc, ch);
    FLAC__stream_encoder_set_bits_per_sample(enc, 24);
    FLAC__stream_encoder_set_sample_rate(enc, 48000);
    FLAC__stream_encoder_set_compression_level(enc, 5);
    FLAC__stream_encoder_set_blocksize(enc, 4096);

    ai_file_ = std::fopen(path.c_str(), "w+b");
    if (!ai_file_) {
        LOGE("Failed to open AI file: %s (errno: %d)", path.c_str(), errno);
        FLAC__stream_encoder_delete(enc);
        return false;
    }

    auto write_cb = [](const FLAC__StreamEncoder*, const FLAC__byte buffer[], size_t bytes, unsigned, unsigned, void* cd) -> FLAC__StreamEncoderWriteStatus {
        auto* self = static_cast<AudioCapture*>(cd);
        if (bytes > 0 && std::fwrite(buffer, 1, bytes, self->ai_file_) != bytes)
            return FLAC__STREAM_ENCODER_WRITE_STATUS_FATAL_ERROR;
        return FLAC__STREAM_ENCODER_WRITE_STATUS_OK;
    };
    auto seek_cb = [](const FLAC__StreamEncoder*, FLAC__uint64 off, void* cd) -> FLAC__StreamEncoderSeekStatus {
        auto* self = static_cast<AudioCapture*>(cd);
        return std::fseek(self->ai_file_, static_cast<long>(off), SEEK_SET) == 0
            ? FLAC__STREAM_ENCODER_SEEK_STATUS_OK : FLAC__STREAM_ENCODER_SEEK_STATUS_ERROR;
    };
    auto tell_cb = [](const FLAC__StreamEncoder*, FLAC__uint64* off, void* cd) -> FLAC__StreamEncoderTellStatus {
        auto* self = static_cast<AudioCapture*>(cd);
        long pos = std::ftell(self->ai_file_);
        if (pos < 0) return FLAC__STREAM_ENCODER_TELL_STATUS_ERROR;
        *off = static_cast<FLAC__uint64>(pos);
        return FLAC__STREAM_ENCODER_TELL_STATUS_OK;
    };

    if (FLAC__stream_encoder_init_stream(enc, write_cb, seek_cb, tell_cb, nullptr, this)
            != FLAC__STREAM_ENCODER_INIT_STATUS_OK) {
        LOGE("FLAC AI encoder init failed: %s", path.c_str());
        FLAC__stream_encoder_delete(enc);
        std::fclose(ai_file_); ai_file_ = nullptr;
        return false;
    }

    constexpr size_t BLK = 4096;
    std::vector<FLAC__int32> inter(BLK * ch);
    for (size_t off = 0; off < n; off += BLK) {
        const size_t m = std::min(BLK, n - off);
        for (size_t i = 0; i < m; ++i)
            for (int c = 0; c < ch; ++c) {
                float v = std::clamp(den[c][off + i], -1.0f, 1.0f);
                inter[i * ch + c] = static_cast<FLAC__int32>(v * 8388607.0f);
            }
        if (!FLAC__stream_encoder_process_interleaved(enc, inter.data(), static_cast<unsigned>(m)))
            LOGE("AI FLAC process failed (%s): state %d", path.c_str(), FLAC__stream_encoder_get_state(enc));
    }

    FLAC__stream_encoder_finish(enc);
    FLAC__stream_encoder_delete(enc);
    std::fclose(ai_file_); ai_file_ = nullptr;
    LOGI("AI denoise written: %zu samples/ch -> %s", n, path.c_str());
    return true;
}

void AudioCapture::finalize_denoise() {
    std::lock_guard<std::mutex> lock(ai_mutex_);
    ai_written_path_.clear();   // nothing written yet for this clip
    if (!df_ || df_buf_.empty()) { stop_ai_recording_locked(); return; }

    const int ch = cfg_.channels;

    // Drain any samples still held inside the resampler (flush with null input).
    if (soxr_) {
        size_t odone = 0;
        do {
            soxr_process(soxr_, nullptr, 0, nullptr,
                         soxr_out_buf_.data(), soxr_out_buf_.size() / ch, &odone);
            for (int c = 0; c < ch; ++c)
                for (size_t i = 0; i < odone; ++i)
                    df_buf_[c].push_back(soxr_out_buf_[i * ch + c]);
        } while (odone > 0);
    }

    // DeepFilterNet + post-filter, per channel (mono model; stereo preserved).
    std::vector<std::vector<float>> den(ch);
    size_t n = df_buf_[0].size();
    for (int c = 0; c < ch; ++c) {
        den[c] = df_->process(df_buf_[c], /*post_filter=*/true);
        if (den[c].size() != df_buf_[c].size()) { LOGE("denoise ch%d failed; using original", c); den[c] = df_buf_[c]; }
        n = std::min(n, den[c].size());
    }

    if (write_flac_file(ai_path_, den, n)) ai_written_path_ = ai_path_;

    stop_ai_recording_locked();
}

void AudioCapture::stop_ai_recording() {
    std::lock_guard<std::mutex> lock(ai_mutex_);
    stop_ai_recording_locked();
}

void AudioCapture::stop_ai_recording_locked() {
    if (soxr_) {
        soxr_delete(soxr_);
        soxr_ = nullptr;
    }
    if (ai_file_) {   // safety net; write_flac_file closes it on the normal path
        std::fclose(ai_file_);
        ai_file_ = nullptr;
    }
    df_.reset();
    df_buf_.clear();
    df_buf_.shrink_to_fit();
}

void AudioCapture::capture_loop() {
    if (!driver_ && !use_internal_) {
        LOGE("capture_loop: no source, exiting thread");
        return;
    }

    // Bytes per sample (bit_depth / 8) × channels × FRAME_SAMPLES
    const int bytes_per_sample = cfg_.bit_depth / 8;
    const int frame_bytes      = bytes_per_sample * cfg_.channels * FRAME_SAMPLES;

    std::vector<uint8_t>       raw(frame_bytes);
    int                        raw_filled = 0;
    int                        read_errors = 0;
    constexpr int              kMaxReadErrors = 200;   // ~1 s of failed reads

    // De-interleave scratch buffers, allocated ONCE and reused every block.
    // (Per-block allocation here caused recurring ~85ms latency spikes that could
    // stall the next read and drop samples — itself a rhythm defect.)
    std::vector<std::vector<FLAC__int32>> channels(cfg_.channels,
        std::vector<FLAC__int32>(FRAME_SAMPLES));
    std::vector<const FLAC__int32*> ch_ptrs(cfg_.channels);
    for (int c = 0; c < cfg_.channels; ++c) ch_ptrs[c] = channels[c].data();

    while (capturing_) {
        int needed = frame_bytes - raw_filled;
        int got;
        if (use_internal_) {
            const int frame_stride = bytes_per_sample * cfg_.channels;  // bytes per audio frame
            int frames_needed = needed / frame_stride;
            if (frames_needed <= 0) frames_needed = 1;
            aaudio_result_t fr = AAudioStream_read(
                static_cast<AAudioStream*>(aaudio_stream_),
                raw.data() + raw_filled, frames_needed, 100'000'000 /*100ms*/);
            got = (fr > 0) ? fr * frame_stride : (fr < 0 ? -1 : 0);
        } else {
            got = driver_->readCapture(raw.data() + raw_filled, needed);
        }

        if (got <= 0) {
            // A hard read error repeats forever (AAudio returns <0 for good once
            // the stream is disconnected), so a flat 200us retry span the CPU at
            // ~5 kHz for the rest of the recording. Back off, and give up rather
            // than spin if the source never recovers.
            if (got < 0 && ++read_errors > kMaxReadErrors) {
                LOGE("audio source failed %d consecutive reads — stopping capture", read_errors);
                break;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(200 * (got < 0 ? 25 : 1)));
            continue;
        }
        read_errors = 0;

        // Anchor the audio timeline to the first sample's capture instant, in the
        // same clock domain as the video encoder PTS (MONOTONIC), so A/V align.
        if (!anchored_) { audio_base_ns_ = monotonic_ns() - kInputLatencyNs; anchored_ = true; }

        raw_filled += got;

        if (raw_filled < frame_bytes) continue;  // accumulate a full frame

        // Sample-accurate timestamp: each block holds exactly FRAME_SAMPLES, so its
        // time is the running sample count / sample_rate from the anchor. This makes
        // the audio timeline perfectly uniform (preserves the music's rhythm) and
        // leaves every PCM sample byte untouched.
        frame_ts_ = audio_base_ns_ +
            static_cast<int64_t>(samples_emitted_ * 1'000'000'000ULL /
                                 static_cast<uint64_t>(cfg_.sample_rate));
        samples_emitted_ += FRAME_SAMPLES;

        // Convert raw bytes → FLAC__int32 directly into the per-channel buffers.
        // USB audio PCM is LITTLE-ENDIAN, sign-extended to the sample's bit depth.
        // Also convert to float for SoX (interleaved).
        const uint8_t* p = raw.data();
        for (int i = 0; i < FRAME_SAMPLES; ++i) {
            for (int c = 0; c < cfg_.channels; ++c) {
                int32_t val = 0;
                float fval = 0.0f;
                if (bytes_per_sample == 2) {
                    val = (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
                    fval = val / 32768.0f;
                } else if (bytes_per_sample == 3) {
                    uint32_t u = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
                    if (u & 0x00800000u) u |= 0xFF000000u;  // sign-extend 24-bit
                    val = (int32_t)u;
                    fval = val / 8388608.0f;
                } else if (bytes_per_sample == 4) {
                    val = (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                                   ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
                    fval = val / 2147483648.0f;
                }
                channels[c][i] = val;
                
                // Keep scaled float for SoX (Interleaved)
                soxr_in_buf_[i * cfg_.channels + c] = fval;
                p += bytes_per_sample;
            }
        }

        frame_buf_.clear();
        FLAC__stream_encoder_process(encoder_, ch_ptrs.data(), FRAME_SAMPLES);

        // --- AI denoise capture: resample to 48 kHz and buffer the whole clip ---
        // The DeepFilterNet inference is offline (the exported models need the full
        // sequence), so here we only accumulate; finalize_denoise() runs the model.
        {
            std::lock_guard<std::mutex> lock(ai_mutex_);
            if (soxr_ && !df_buf_.empty()) {
                size_t idone, odone;
                soxr_process(soxr_, soxr_in_buf_.data(), FRAME_SAMPLES, &idone,
                             soxr_out_buf_.data(), soxr_out_buf_.size() / cfg_.channels, &odone);
                for (int c = 0; c < cfg_.channels; ++c)
                    for (size_t i = 0; i < odone; ++i)
                        df_buf_[c].push_back(soxr_out_buf_[i * cfg_.channels + c]);
            }
        }

        if (callback_ && !frame_buf_.empty()) {
            callback_(frame_buf_.data(), (int)frame_buf_.size(), frame_ts_);
        }

        raw_filled = 0;
    }
}

// Called by libFLAC each time it produces encoded output for a frame.
FLAC__StreamEncoderWriteStatus AudioCapture::on_flac_write(
    const FLAC__StreamEncoder*,
    const FLAC__byte* buffer,
    size_t bytes,
    uint32_t samples,
    uint32_t /*frame_number*/,
    void* ctx)
{
    auto* self = reinterpret_cast<AudioCapture*>(ctx);

    // samples == 0 is the stream header ("fLaC" + STREAMINFO/metadata), written
    // during init_stream. Keep it as the Matroska CodecPrivate — NOT as an audio
    // frame (prepending it to the first frame would corrupt the first block).
    if (samples == 0) {
        self->header_buf_.insert(self->header_buf_.end(), buffer, buffer + bytes);
    } else {
        self->frame_buf_.insert(self->frame_buf_.end(), buffer, buffer + bytes);
    }

    // Audio frames are delivered once per block in capture_loop after process().
    return FLAC__STREAM_ENCODER_WRITE_STATUS_OK;
}

} // namespace aud
