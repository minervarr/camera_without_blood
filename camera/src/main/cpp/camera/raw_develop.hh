#pragma once

#include <cstdint>
#include <vector>

#include "dng_writer.hh"

namespace cam {

// Output transfer/primaries for a developed still.
enum class OutputSpace {
    kSrgb,       // sRGB primaries + sRGB curve, clamped at diffuse white (SDR)
    kBt2020Pq,   // BT.2020 primaries + SMPTE ST 2084, absolute (HDR)
};

// Linear scene radiance 1.0 (the reference exposure's clip point) maps to
// kPqScale * 10000 nits = 1000 nits. Shared convention with the video ISP's
// RawVideoPipeline::kPqScale, which is what makes a still and a clip of the same
// scene match. Changing this invalidates the absolute levels of every file
// already shot, so it lives here in the open rather than as a literal.
constexpr float kPqScale = 0.10f;

// Develops a RAW16 Bayer frame into 16-bit RGB, for saving a viewable
// still alongside the DNG.
//
// The DNG stays the archival original — this is the "already developed" copy.
// kBt2020Pq is the one the RAW still path uses: real HDR, matching the video ISP.
// kSrgb remains for SDR consumers. Colour follows the same derivation
// the video ISP uses (DNG ForwardMatrix -> XYZ, Bradford to D65, then to the
// output primaries, rows normalised so camera neutral maps to white), so a still
// and a clip shot back to back agree.
//
// Demosaic is Malvar-He-Cutler, the same 5x5 gradient-corrected filter the video
// path defaults to.
//
// `bayer` is the raw plane with `stride_bytes` per row (may exceed width*2).
// `neutral` is the as-shot camera neutral; pass {1,1,1} if unknown. Output is
// width*height*3 unsigned shorts, host-endian, row-contiguous.
bool develop_raw_to_rgb16(const uint16_t* bayer, int width, int height, int stride_bytes,
                          const dng::DngMeta& meta, const float neutral[3],
                          OutputSpace space, std::vector<uint16_t>& out_rgb16);

// The core of the above, for callers that already hold linear data — notably the
// HDR bracket merge, whose radiance is float and deliberately exceeds 1.0.
//
// `lin` is a black-subtracted Bayer plane, stride == width, normalised so 1.0 is
// the reference exposure's clip point. Values ABOVE 1.0 are recovered highlights
// and are carried through to the PQ curve rather than clamped; under kBt2020Pq a
// 4x bracket boost lands at 4000 nits, inside PQ's 10000-nit ceiling.
//
// `clip_level` is the value at which the SOURCE saturated (1.0 for a single
// shot; a merged bracket's ceiling is its max_boost). Channels at or near it
// carry no information beyond "at least this bright", so they are rebuilt —
// see the highlight reconstruction in the implementation. Pass <= 0 to disable.
//
// `out_peak_linear`, when non-null, receives the largest LINEAR channel value
// the develop produced, before the transfer curve. Callers need it to label the
// file's real peak luminance: reading the encoded buffer back cannot tell you,
// because PQ saturates at 1.0 (10000 nits), so a handful of samples over the
// ceiling would report 10000 for an image that never approaches it.
bool develop_linear_bayer(const float* lin, int width, int height,
                          const dng::DngMeta& meta, const float neutral[3],
                          OutputSpace space, std::vector<uint16_t>& out_rgb16,
                          float clip_level = 1.0f, float* out_peak_linear = nullptr);

} // namespace cam
