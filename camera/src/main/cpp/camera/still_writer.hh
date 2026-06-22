#pragma once

#include <cstdint>
#include <string>

namespace cam {

// Converts an Android YUV_420_888 frame to RGB8 and writes it as a lossless PNG.
//
// The planes come straight from android.media.Image: each has its own row stride,
// and the chroma planes carry a `uv_pixel_stride` (1 for fully-planar I420, 2 for
// semi-planar NV12/NV21) so both layouts are handled. `orientation_deg`
// (0/90/180/270, the sensor mount orientation, clockwise-to-upright) is baked into
// the pixels because PNG has no reliable orientation tag.
//
// This is the non-RAW still path: on devices without a RAW_SENSOR stream the ISP
// only hands us YUV, so the rawest/highest-quality lossless still we can store is
// the full-resolution YUV converted to RGB. Returns true on success.
bool write_png_yuv420(const std::string& path,
                      const uint8_t* y, const uint8_t* u, const uint8_t* v,
                      int width, int height,
                      int y_stride, int u_stride, int v_stride, int uv_pixel_stride,
                      int orientation_deg);

} // namespace cam
