#pragma once

#include <cstdint>
#include <memory>
#include <vector>

struct AAssetManager;

namespace aud {

// Real DeepFilterNet3 speech denoiser (48 kHz mono).
//
// The three exported ONNX models (enc / erb_dec / df_dec) run over the WHOLE time
// axis and expose no GRU hidden-state I/O, so per-frame streaming is impossible —
// this processes a whole captured clip offline (at the recorder's FINALIZING
// phase). The ONNX inference is chunked-with-warmup so the (dominant) activation
// memory stays bounded regardless of clip length; the STFT spectrum and PCM
// buffers are still held whole-clip (O(duration) RAM — fine for clips up to many
// minutes; a fully streamed front/back-end is a future step for very long clips).
// Output is the same length as the input and sample-aligned (the model's
// 480-sample STFT delay is compensated).
//
// Every DSP stage (vorbis-window STFT, width-based ERB, erb_norm/unit_norm,
// mask + 5-tap deep filter, ISTFT) is a port of libdf, validated numerically
// against libdf goldens. See memory deepfilternet-audio-impl for the contract.
class DfNet {
public:
    DfNet();
    ~DfNet();
    DfNet(const DfNet&) = delete;
    DfNet& operator=(const DfNet&) = delete;

    // Loads enc/erb_dec/df_dec from assets (models/tmp/export/*.onnx). CPU EP.
    // Returns false if the models are missing or ORT init fails.
    bool init(AAssetManager* assets);
    bool ready() const;

    // Denoises a whole mono 48 kHz clip. Returns same-length, sample-aligned
    // output. Returns an empty vector on failure (caller falls back to original).
    // post_filter: apply DeepFilterNet's optional post-filter to the ERB mask
    // (extra residual-noise suppression in the high-freq masked bands).
    std::vector<float> process(const std::vector<float>& mono48k, bool post_filter = false);

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace aud
