#pragma once

#include "dng_writer.hh"

namespace dng {

// Fills the static DNG fields (CFA, white level, black pattern, colour matrices,
// illuminants, orientation) by reading the back camera's characteristics via
// ACameraManager — no device open required. Also reports the RAW16 stream size.
// Returns false if no back camera / RAW support is found.
bool load_static_meta(DngMeta& out, int& raw_width, int& raw_height);

} // namespace dng
