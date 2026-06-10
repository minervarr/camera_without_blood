#include "audio_capture.hh"
#include "usb_audio.h"

#include <FLAC/stream_encoder.h>
#include <aaudio/AAudio.h>

#include "logger.hh"
#include <algorithm>
#include <chrono>
#include <ctime>
#include <thread>
#include <cstring>
#undef LOG_TAG
#define LOG_TAG "AudioCapture"

namespace aud {

// Samples per FLAC frame — 4096 is a good balance of latency vs overhead
static constexpr int FRAME_SAMPLES = 4096;

// CLOCK_MONOTONIC nanoseconds (== std::chrono::steady_clock on Android). This is
// the domain the video encoder PTS actually lands in on this pipeline: although the
// camera advertises SENSOR timestamp source REALTIME, anchoring audio to BOOTTIME
// made audio diverge from video by the device's (large) suspend time, overflowing
// the Matroska 16-bit block delta and aborting. MONOTONIC keeps A/V on one clock.
static int64_t monotonic_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

AudioCapture::AudioCapture()  = default;
AudioCapture::~AudioCapture() { close(); }

bool AudioCapture::open(const AudioConfig& cfg) {
    cfg_ = cfg;

    auto devices = UsbAudioDriver::enumerateUsbAudioDevices();
    for (auto& info : devices) {
        auto* drv = new UsbAudioDriver();
        if (drv->open(info.vid, info.pid)) {
            drv->parseDescriptors();
            if (drv->hasCaptureFormats()) {
                drv->configureCapture(cfg_.sample_rate, cfg_.channels, cfg_.bit_depth);
                driver_ = drv;
                LOGI("USB audio opened: %s %dHz %dch %dbit",
                     info.name.c_str(), cfg_.sample_rate, cfg_.channels, cfg_.bit_depth);
                return true;
            }
        }
        delete drv;
    }

    LOGE("No USB audio capture device found");
    return false;
}

bool AudioCapture::open_fd(int fd, const AudioConfig& cfg) {
    cfg_ = cfg;
    if (fd <= 0) { LOGE("open_fd: invalid fd %d", fd); return false; }

    auto* drv = new UsbAudioDriver();
    if (!drv->open(fd) || (drv->parseDescriptors(), !drv->hasCaptureFormats())) {
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

    // ── Configure FLAC encoder ─────────────────────────────────────────────────
    encoder_ = FLAC__stream_encoder_new();
    if (!encoder_) { LOGE("FLAC__stream_encoder_new failed"); return false; }

    FLAC__stream_encoder_set_channels(encoder_, cfg_.channels);
    FLAC__stream_encoder_set_bits_per_sample(encoder_, cfg_.bit_depth);
    FLAC__stream_encoder_set_sample_rate(encoder_, cfg_.sample_rate);
    FLAC__stream_encoder_set_compression_level(encoder_, 5); // 0=fast … 8=best
    FLAC__stream_encoder_set_blocksize(encoder_, FRAME_SAMPLES);
    FLAC__stream_encoder_set_streamable_subset(encoder_, true);

    auto status = FLAC__stream_encoder_init_stream(
        encoder_,
        on_flac_write,
        nullptr,  // seek  — streaming, no seek needed
        nullptr,  // tell
        nullptr,  // metadata
        this
    );
    if (status != FLAC__STREAM_ENCODER_INIT_STATUS_OK) {
        LOGE("FLAC encoder init failed: %d", status);
        FLAC__stream_encoder_delete(encoder_);
        encoder_ = nullptr;
        return false;
    }

    capturing_ = true;
    if (use_internal_ && aaudio_stream_)
        AAudioStream_requestStart(static_cast<AAudioStream*>(aaudio_stream_));
    else if (driver_) driver_->startCapture();
    std::thread([this]{ capture_loop(); }).detach();
    LOGI("Audio capture + FLAC encoding started");
    return true;
}

void AudioCapture::stop() {
    if (!capturing_) return;
    capturing_ = false;
    if (use_internal_ && aaudio_stream_)
        AAudioStream_requestStop(static_cast<AAudioStream*>(aaudio_stream_));
    else if (driver_) driver_->stopCapture();
    if (encoder_) FLAC__stream_encoder_finish(encoder_);
    LOGI("Audio capture stopped");
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
            std::this_thread::sleep_for(std::chrono::microseconds(200));
            continue;
        }

        // Anchor the audio timeline to the first sample's capture instant, in the
        // same clock domain as the video encoder PTS (MONOTONIC), so A/V align.
        if (!anchored_) { audio_base_ns_ = monotonic_ns(); anchored_ = true; }

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
        // (Reading it big-endian byte-reverses every sample → pure noise, the old bug.)
        const uint8_t* p = raw.data();
        for (int s = 0; s < FRAME_SAMPLES; ++s) {
            for (int c = 0; c < cfg_.channels; ++c) {
                int32_t v = 0;
                if (bytes_per_sample == 4) {
                    v = (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                                  ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
                } else if (bytes_per_sample == 3) {
                    uint32_t u = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
                    if (u & 0x00800000u) u |= 0xFF000000u;  // sign-extend 24-bit
                    v = (int32_t)u;
                } else if (bytes_per_sample == 2) {
                    v = (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
                }
                channels[c][s] = v;
                p += bytes_per_sample;
            }
        }

        frame_buf_.clear();
        FLAC__stream_encoder_process(encoder_, ch_ptrs.data(), FRAME_SAMPLES);
        // on_flac_write will fill frame_buf_ and trigger callback

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
