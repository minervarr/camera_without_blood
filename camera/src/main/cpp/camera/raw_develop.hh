#pragma once

#include <cstdint>
#include <vector>

#include "dng_writer.hh"

namespace cam {

// Develops a RAW16 Bayer frame into 16-bit sRGB, for saving a viewable lossless
// still alongside the DNG.
//
// The DNG stays the archival original — this is the "already developed" copy, so
// it commits to sRGB primaries and the sRGB transfer curve, which is what every
// viewer and editor will assume from a PNG. Colour follows the same derivation
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
                          std::vector<uint16_t>& out_rgb16);

} // namespace cam
