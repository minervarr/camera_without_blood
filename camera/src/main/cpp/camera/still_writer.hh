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

// Writes 16-bit RGB as a lossless PNG (colour type 2, bit depth 16).
//
// stb_image_write cannot do 16-bit, and 8 bits would throw away most of what a
// RAW frame carries — which is the whole point of saving one. This uses zlib
// directly, which is also several times faster than stb's deflate (the 8-bit
// YUV path above takes ~9 s for 12 MP almost entirely inside it).
//
// `rgb16` is width*height*3 host-endian samples (PNG stores big-endian; the
// conversion happens here). `orientation_deg` (0/90/180/270, clockwise to
// upright) is baked into the pixels, as PNG has no orientation tag.
bool write_png_rgb16(const std::string& path,
                     const uint16_t* rgb16, int width, int height,
                     int orientation_deg);

} // namespace cam
