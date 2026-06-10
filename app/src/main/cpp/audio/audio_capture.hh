#pragma once

#include <FLAC/stream_encoder.h>

#include <cstdint>
#include <functional>
#include <vector>

class UsbAudioDriver;

namespace aud {

struct AudioConfig {
    int sample_rate = 48000;
    int channels    = 2;
    int bit_depth   = 32;  // native USB bit depth; FLAC will encode at this depth
};

// Delivers a complete encoded FLAC frame + its PCM timestamp.
using FlacFrameCallback = std::function<void(const uint8_t* frame, int bytes, int64_t timestamp_ns)>;

class AudioCapture {
public:
    AudioCapture();
    ~AudioCapture();

    // Opens first USB audio device with capture support (libusb device
    // discovery — only works with root; kept as a fallback).
    bool open(const AudioConfig& cfg = {});

    // Opens a USB device from a file descriptor obtained via Android's
    // UsbManager (after a runtime USB permission grant). This is the path that
    // works on a normal, non-rooted device.
    bool open_fd(int fd, const AudioConfig& cfg = {});

    // Opens the phone's built-in microphone via AAudio (stereo if the device
    // offers it, otherwise mono). Used when no USB DAC is attached.
    bool open_internal(const AudioConfig& cfg = {});

    void close();

    bool start(FlacFrameCallback cb);
    void stop();

    bool        is_capturing() const { return capturing_; }
    AudioConfig config()       const { return cfg_; }

    // FLAC stream header ("fLaC" + STREAMINFO) for use as the Matroska
    // CodecPrivate. Valid after start().
    const std::vector<uint8_t>& codec_private() const { return header_buf_; }

private:
    void capture_loop();

    // libFLAC stream encoder callbacks
    static FLAC__StreamEncoderWriteStatus on_flac_write(
        const FLAC__StreamEncoder*, const FLAC__byte* buffer,
        size_t bytes, uint32_t samples, uint32_t frame_number, void* ctx);

    UsbAudioDriver*    driver_    = nullptr;
    void*              aaudio_stream_ = nullptr;  // AAudioStream* (internal mic)
    bool               use_internal_  = false;
    FLAC__StreamEncoder* encoder_ = nullptr;
    FlacFrameCallback  callback_;
    AudioConfig        cfg_;
    bool               capturing_ = false;

    // Accumulates the current encoded FLAC frame before delivery
    std::vector<uint8_t> frame_buf_;
    int64_t              frame_ts_ = 0;

    // Sample-accurate timestamping. Audio frame times are derived from a running
    // sample count anchored to CLOCK_MONOTONIC (steady_clock — the domain the video
    // encoder PTS lands in), NOT from wall-clock-at-block-completion — that jitter
    // warbled the music's rhythm. (BOOTTIME was tried and overflowed the muxer.)
    int64_t              audio_base_ns_   = 0;      // MONOTONIC anchor at first sample
    uint64_t             samples_emitted_ = 0;      // cumulative per-channel-frame count
    bool                 anchored_        = false;  // base captured yet?

    // FLAC stream header captured during init_stream (Matroska CodecPrivate).
    std::vector<uint8_t> header_buf_;
};

} // namespace aud
