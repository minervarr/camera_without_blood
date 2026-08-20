#include "ndk_encoder.hh"

#include "../logger.hh"

#include <media/NdkMediaFormat.h>

#include <algorithm>

#undef LOG_TAG
#define LOG_TAG "NdkEnc"

namespace ndkcam {
namespace {

constexpr const char* kMime = "video/hevc";

// HEVC Main. Not Main10: this path exists for devices with no dynamic-range
// profile, and the verified one has no 10-bit encoder either.
constexpr int32_t kProfileMain = 0x01;

// Bitrate for a given pixel rate, capped at what HEVC encoders on this class of
// device actually accept (the verified one tops out at 30 Mbit/s). Generous on
// purpose: this is a "rawest possible" recorder and the legacy path was leaving
// headroom unused.
int32_t bitrate_for(int32_t w, int32_t h, int32_t fps) {
    // Deliberately high, then clamped to the ceiling: this is a "rawest the
    // hardware allows" recorder, so the encoder should be saturated rather than
    // politely under-driven. The Java path used ~0.5 bpp at a much smaller
    // frame, so anything less here would trade resolution for compression
    // artefacts — a worse deal, not a better one.
    const double bpp = 0.40;   // bits per pixel per frame
    double bps = double(w) * h * fps * bpp;
    return static_cast<int32_t>(std::min(bps, 30.0e6));
}

} // namespace

Encoder::~Encoder() { stop(); }

bool Encoder::configure(const Size* candidates, int count, int32_t fps, jni::RecordSink sink) {
    sink_ = std::move(sink);

    for (int i = 0; i < count; ++i) {
        const Size s = candidates[i];
        if (s.w <= 0 || s.h <= 0) continue;

        AMediaCodec* codec = AMediaCodec_createEncoderByType(kMime);
        if (!codec) { LOGE("createEncoderByType(%s) failed", kMime); return false; }

        const int32_t br = bitrate_for(s.w, s.h, fps);

        AMediaFormat* fmt = AMediaFormat_new();
        AMediaFormat_setString(fmt, AMEDIAFORMAT_KEY_MIME, kMime);
        AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_WIDTH,  s.w);
        AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_HEIGHT, s.h);
        AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_FRAME_RATE, fps);
        AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_BIT_RATE, br);
        AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_I_FRAME_INTERVAL, 1);
        // AMEDIAFORMAT_KEY_PROFILE is guarded to API 28 and minSdk here is 26;
        // the constant is just this string, and older runtimes ignore an
        // unknown key rather than failing.
        AMediaFormat_setInt32(fmt, "profile", kProfileMain);
        // COLOR_FormatSurface: the camera writes into the input surface, so
        // there is no readback and no CPU copy anywhere in this path.
        AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_COLOR_FORMAT, 0x7F000789);

        const media_status_t st =
            AMediaCodec_configure(codec, fmt, nullptr, nullptr,
                                  AMEDIACODEC_CONFIGURE_FLAG_ENCODE);
        AMediaFormat_delete(fmt);

        if (st != AMEDIA_OK) {
            // Capability tables lie in both directions; the codec's own answer
            // is the only one worth trusting, so just try the next size down.
            LOGI("encoder rejected %dx%d (status %d), trying smaller", s.w, s.h, st);
            AMediaCodec_delete(codec);
            continue;
        }

        ANativeWindow* win = nullptr;
        if (AMediaCodec_createInputSurface(codec, &win) != AMEDIA_OK || !win) {
            LOGE("createInputSurface failed at %dx%d", s.w, s.h);
            AMediaCodec_delete(codec);
            continue;
        }

        codec_   = codec;
        surface_ = win;
        w_ = s.w; h_ = s.h; bitrate_ = br;
        LOGI("encoder configured %dx%d @%d bps (HEVC Main, surface input)", w_, h_, bitrate_);
        return true;
    }

    LOGE("no candidate size was accepted by the HEVC encoder");
    return false;
}

bool Encoder::start() {
    if (!codec_ || running_.load(std::memory_order_acquire)) return false;
    if (AMediaCodec_start(codec_) != AMEDIA_OK) {
        LOGE("AMediaCodec_start failed");
        return false;
    }
    running_.store(true, std::memory_order_release);
    drain_ = std::thread(&Encoder::drain_loop, this);
    return true;
}

void Encoder::drain_loop() {
    bool sent_format = false;
    while (running_.load(std::memory_order_acquire)) {
        AMediaCodecBufferInfo info{};
        const ssize_t idx = AMediaCodec_dequeueOutputBuffer(codec_, &info, 20'000);
        if (idx >= 0) {
            size_t size = 0;
            uint8_t* buf = AMediaCodec_getOutputBuffer(codec_, static_cast<size_t>(idx), &size);
            if (buf && info.size > 0) {
                if (info.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG) {
                    // Annex B VPS/SPS/PPS, exactly what the muxer expects.
                    if (sink_.on_format && !sent_format) {
                        sink_.on_format(buf + info.offset, info.size, w_, h_);
                        sent_format = true;
                    }
                } else if (sink_.on_packet) {
                    const bool key = (info.flags & AMEDIACODEC_BUFFER_FLAG_KEY_FRAME) != 0;
                    sink_.on_packet(buf + info.offset, info.size, info.presentationTimeUs, key);
                }
            }
            AMediaCodec_releaseOutputBuffer(codec_, static_cast<size_t>(idx), false);
            if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) break;
        } else if (idx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            AMediaFormat* out = AMediaCodec_getOutputFormat(codec_);
            if (out) {
                // Some encoders deliver csd-0 here instead of as a CODEC_CONFIG
                // buffer; take whichever arrives first.
                uint8_t* csd = nullptr; size_t csd_len = 0;
                if (!sent_format && sink_.on_format &&
                    AMediaFormat_getBuffer(out, "csd-0", reinterpret_cast<void**>(&csd), &csd_len) &&
                    csd && csd_len > 0) {
                    sink_.on_format(csd, static_cast<int>(csd_len), w_, h_);
                    sent_format = true;
                }
                AMediaFormat_delete(out);
            }
        }
        // AMEDIACODEC_INFO_TRY_AGAIN_LATER: nothing ready, loop.
    }
}

void Encoder::stop() {
    if (codec_ && running_.load(std::memory_order_acquire)) {
        // Signal EOS so the drain thread sees the tail packets before exiting.
        AMediaCodec_signalEndOfInputStream(codec_);
    }
    running_.store(false, std::memory_order_release);
    if (drain_.joinable()) drain_.join();

    if (codec_) {
        AMediaCodec_stop(codec_);
        AMediaCodec_delete(codec_);
        codec_ = nullptr;
    }
    // The input surface is owned by the codec; releasing it after delete would
    // double-free.
    surface_ = nullptr;
}

} // namespace ndkcam
