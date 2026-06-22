#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "dng_writer.hh"   // dng::DngMeta, dng::write_dng

namespace hdr {

// One frame of a RAW exposure bracket: a *contiguous* copy of the 16-bit Bayer
// plane (row stride removed, so stride == width) plus the per-shot metadata
// needed to linearize and normalize it for the merge.
struct BracketFrame {
    std::vector<uint16_t> data;          // width*height, row-contiguous
    int      width  = 0;
    int      height = 0;
    float    black[4] = {0, 0, 0, 0};    // per-CFA-element black level (raster 2x2)
    int      white  = 0;                 // dynamic white level (0 = unknown)
    float    neutral[3] = {1, 1, 1};     // as-shot neutral
    int64_t  exposure_ns = 0;            // 0 = unknown
    int      iso = 0;                    // 0 = unknown
    int      index = 0;                  // position in the burst (0 = darkest/-EV)
};

// Result of an exposure-bracket merge: the per-pixel merged radiance in the
// reference (0 EV) frame's raw counts (black subtracted), kept as float so it can
// be either quantized to a DNG (write_merged_dng) or developed to a scene-referred
// HDR image (raw_develop) without an intermediate precision loss.
struct MergeResult {
    bool                ok = false;
    int                 W = 0, H = 0;
    std::vector<float>  radiance;        // W*H Bayer, ref-exposure counts (black=0)
    double              ref_span = 1.0;  // 0 EV usable span (white-black) -> maps to 1.0
    double              out_white = 65535.0; // 16-bit white for the DNG quantization
    float               neutral[3] = {1, 1, 1};
    dng::DngMeta        meta;            // static tags (CFA, colour matrices) + geometry
};

// Merges an exposure bracket (>= 2 frames, identical geometry/CFA). Frames are
// globally aligned to the middle (0 EV) frame on an exposure-invariant proxy, by
// EVEN pixel offsets so the Bayer phase is preserved, then combined in the linear
// radiance domain (highlight/shadow-aware weighting). Returns `ok=false` if it
// can't merge — the caller keeps the per-shot source DNGs as the fallback.
MergeResult merge_raw_bracket(const std::vector<BracketFrame>& frames,
                              const dng::DngMeta& base_meta);

// Quantizes a MergeResult to a 16-bit Bayer DNG at `out_path` (black=0,
// white=out_white). Stays fully RAW (no demosaic/tone curve).
bool write_merged_dng(const MergeResult& r, const std::string& out_path);

} // namespace hdr
