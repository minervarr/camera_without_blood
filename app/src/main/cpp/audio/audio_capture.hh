#pragma once

#include <FLAC/stream_encoder.h>
#include <soxr.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>
#include <thread>
#include <mutex>

#include "df_net.hh"   // full type: AudioCapture holds a std::unique_ptr<DfNet>,
                        // and its defaulted ctor/dtor get instantiated in TUs
                        // (e.g. recorder.cc) that don't include df_net.hh directly.

struct AAssetManager;
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
    AudioCapture() = default;
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

    // Sets up the offline DeepFilterNet denoise: loads the model, the 48 kHz
    // resampler, the side-car FLAC encoder, and the capture buffer. Cheap; the
    // actual inference happens later in finalize_denoise().
    bool start_ai_recording(const std::string& path, ::AAssetManager* assets);
    // Runs DeepFilterNet over the whole buffered clip and writes the denoised
    // 48 kHz <clip>_ai.flac side-car. Heavy + slower-than-realtime — call from the
    // recorder's background FINALIZING thread, not the capture/UI thread.
    void finalize_denoise();
    // Releases the denoise resources without running inference (abort path).
    void stop_ai_recording();

    bool        is_capturing() const { return capturing_; }
    AudioConfig config()       const { return cfg_; }

    // FLAC stream header ("fLaC" + STREAMINFO) for use as the Matroska
    // CodecPrivate. Valid after start().
    const std::vector<uint8_t>& codec_private() const { return header_buf_; }

private:
    void capture_loop();
    void stop_ai_recording_locked();   // denoise cleanup; caller holds ai_mutex_
    // Encodes per-channel 48 kHz float audio to a standalone 24-bit FLAC file.
    void write_flac_file(const std::string& path,
                         const std::vector<std::vector<float>>& den, size_t n);

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
    std::thread        capture_thread_;

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

    // Offline DeepFilterNet denoise -> side-car <clip>_ai.flac (48 kHz, 24-bit).
    // During capture the audio is just resampled to 48 kHz and accumulated into
    // df_buf_ (per channel); the model runs once over the whole clip at finalize.
    std::unique_ptr<DfNet> df_;
    FILE*                  ai_file_    = nullptr;   // current output file (transient)
    soxr_t                 soxr_       = nullptr;
    std::string            ai_path_;                // <clip>_ai.flac (DeepFilterNet + post-filter)
    std::vector<float>     soxr_in_buf_;
    std::vector<float>     soxr_out_buf_;
    std::vector<std::vector<float>> df_buf_;   // captured 48 kHz audio, per channel
    std::mutex             ai_mutex_;          // guards the denoise resources
};

} // namespace aud
