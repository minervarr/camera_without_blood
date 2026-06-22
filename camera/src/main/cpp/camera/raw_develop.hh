#pragma once

#include <string>

#include "hdr_merge.hh"   // hdr::MergeResult

namespace cam {

// Develops a merged RAW bracket (the float radiance Bayer in `r`) into a
// scene-referred LINEAR HDR image and writes it as a Radiance .hdr (RGBE float)
// at `out_path`: bilinear demosaic + white balance (as-shot neutral) +
// camera->linear-sRGB colour (via the DNG ForwardMatrix), normalized so the 0 EV
// white sits near 1.0 and recovered highlights exceed it. No tone curve is
// applied — the file stays scene-referred HDR. Returns false on failure.
bool develop_to_hdr(const hdr::MergeResult& r, const std::string& out_path);

} // namespace cam
