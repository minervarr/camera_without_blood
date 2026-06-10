#pragma once

#include <cstdint>
#include <string>

namespace mux {

struct VideoTrackConfig {
    int32_t width;
    int32_t height;
    int32_t fps;
    // "V_MPEGH/ISO/HEVC" or "V_MPEG4/ISO/AVC"
    const char* codec_id;
    // Decoder-specific data (hvcC for HEVC / avcC for AVC). The video track is
    // only written when this is present.
    const uint8_t* private_data;
    int            private_data_size;
    // When true, tag the track as 10-bit BT.2020 / HLG HDR.
    bool           hdr_hlg;
};

struct AudioTrackConfig {
    int sample_rate;
    int channels;
    int bit_depth;
    // "A_FLAC"
    const char* codec_id;
    // FLAC CodecPrivate (the "fLaC" marker + STREAMINFO/metadata blocks). The
    // audio track is only written when this is present.
    const uint8_t* private_data;
    int            private_data_size;
};

// Wraps libmatroska to write a simple MKV file with one video + one audio track.
class Muxer {
public:
    Muxer();
    ~Muxer();

    bool open(const std::string& path,
              const VideoTrackConfig& vcfg,
              const AudioTrackConfig& acfg);
    void close();

    // Both write functions are thread-safe (internally locked).
    void write_video(const uint8_t* data, int size, int64_t timestamp_ns, bool keyframe);
    void write_audio(const uint8_t* data, int size, int64_t timestamp_ns);

    bool is_open() const { return open_; }

private:
    struct Impl;
    Impl* impl_ = nullptr;
    bool  open_ = false;
};

} // namespace mux
