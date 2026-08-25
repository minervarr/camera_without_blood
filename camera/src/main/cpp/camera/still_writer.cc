#include "still_writer.hh"

#include "../logger.hh"


#include <vector>
#include <cstdio>


#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../third_party/stb_image_write.h"

#undef LOG_TAG
#define LOG_TAG "StillWriter"

namespace cam {

static inline uint8_t clamp_u8(int v) {
    return v < 0 ? 0 : (v > 255 ? 255 : static_cast<uint8_t>(v));
}

// YUV_420_888 from a camera SDR stream is limited-range BT.601. Integer-approx
// (×1000) of the standard conversion:
//   R = 1.164(Y-16)                + 1.596(V-128)
//   G = 1.164(Y-16) - 0.391(U-128) - 0.813(V-128)
//   B = 1.164(Y-16) + 2.018(U-128)
// The sensor orientation (clockwise to upright) is baked into the pixels —
// PNG and JXL decoders honour orientation tags inconsistently, so every
// writer here does it the same way. Returns row-contiguous RGB8 at ow*oh.
static std::vector<uint8_t> yuv420_to_rgb8(const uint8_t* y, const uint8_t* u,
                                           const uint8_t* v,
                                           int width, int height,
                                           int y_stride, int u_stride, int v_stride,
                                           int uv_pixel_stride, int orientation_deg,
                                           int& ow, int& oh) {
    std::vector<uint8_t> rgb(static_cast<size_t>(width) * height * 3);
    for (int j = 0; j < height; ++j) {
        const uint8_t* yrow = y + static_cast<size_t>(j) * y_stride;
        const uint8_t* urow = u + static_cast<size_t>(j >> 1) * u_stride;
        const uint8_t* vrow = v + static_cast<size_t>(j >> 1) * v_stride;
        uint8_t* out = rgb.data() + static_cast<size_t>(j) * width * 3;
        for (int i = 0; i < width; ++i) {
            const int cidx = (i >> 1) * uv_pixel_stride;
            const int Y = static_cast<int>(yrow[i]) - 16;
            const int U = static_cast<int>(urow[cidx]) - 128;
            const int V = static_cast<int>(vrow[cidx]) - 128;
            const int c = 1164 * Y;
            out[0] = clamp_u8((c + 1596 * V) / 1000);
            out[1] = clamp_u8((c - 391 * U - 813 * V) / 1000);
            out[2] = clamp_u8((c + 2018 * U) / 1000);
            out += 3;
        }
    }

    const int deg = ((orientation_deg % 360) + 360) % 360;
    ow = width; oh = height;
    if (deg == 0) return rgb;

    std::vector<uint8_t> rot;
    if (deg == 90 || deg == 270) { ow = height; oh = width; }
    rot.resize(static_cast<size_t>(ow) * oh * 3);
    for (int j = 0; j < height; ++j) {
        for (int i = 0; i < width; ++i) {
            const uint8_t* sp = rgb.data() + (static_cast<size_t>(j) * width + i) * 3;
            int di, dj;  // destination column, row
            if (deg == 90)       { di = height - 1 - j; dj = i; }                 // 90° CW
            else if (deg == 180) { di = width - 1 - i;  dj = height - 1 - j; }
            else                 { di = j;              dj = width - 1 - i; }      // 270° CW
            uint8_t* dp = rot.data() + (static_cast<size_t>(dj) * ow + di) * 3;
            dp[0] = sp[0]; dp[1] = sp[1]; dp[2] = sp[2];
        }
    }
    return rot;
}

bool write_png_yuv420(const std::string& path,
                      const uint8_t* y, const uint8_t* u, const uint8_t* v,
                      int width, int height,
                      int y_stride, int u_stride, int v_stride, int uv_pixel_stride,
                      int orientation_deg) {
    if (!y || !u || !v || width <= 0 || height <= 0) {
        LOGE("write_png_yuv420: bad args (%dx%d)", width, height);
        return false;
    }

    int ow = 0, oh = 0;
    const std::vector<uint8_t> px = yuv420_to_rgb8(
            y, u, v, width, height, y_stride, u_stride, v_stride, uv_pixel_stride,
            orientation_deg, ow, oh);
    const uint8_t* src = px.data();

    // Trade a little file size for a much faster encode (PNG is lossless at every
    // level — this only changes the DEFLATE search effort, not a single pixel).
    // The default (8) took ~9 s for a 12 MP frame on this CPU, which blocked the
    // capture pipeline; 4 cuts that several-fold.
    stbi_write_png_compression_level = 4;
    if (!stbi_write_png(path.c_str(), ow, oh, 3, src, ow * 3)) {
        LOGE("stbi_write_png failed: %s", path.c_str());
        return false;
    }
    LOGI("wrote PNG %dx%d -> %s", ow, oh, path.c_str());
    return true;
}

} // namespace cam
