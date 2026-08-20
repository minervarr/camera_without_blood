#include "still_writer.hh"

#include "../logger.hh"

#include <vector>
#include <cstdio>

#include <zlib.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../third_party/stb_image_write.h"

#undef LOG_TAG
#define LOG_TAG "StillWriter"

namespace cam {

static inline uint8_t clamp_u8(int v) {
    return v < 0 ? 0 : (v > 255 ? 255 : static_cast<uint8_t>(v));
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

    // YUV_420_888 from a camera SDR stream is limited-range BT.601. Integer-approx
    // (×1000) of the standard conversion:
    //   R = 1.164(Y-16)                + 1.596(V-128)
    //   G = 1.164(Y-16) - 0.391(U-128) - 0.813(V-128)
    //   B = 1.164(Y-16) + 2.018(U-128)
    std::vector<uint8_t> rgb(static_cast<size_t>(width) * height * 3);
    for (int j = 0; j < height; ++j) {
        const uint8_t* yrow = y + static_cast<size_t>(j) * y_stride;
        const uint8_t* urow = u + static_cast<size_t>(j >> 1) * u_stride;
        const uint8_t* vrow = v + static_cast<size_t>(j >> 1) * v_stride;
        uint8_t* out = rgb.data() + static_cast<size_t>(j) * width * 3;
        for (int i = 0; i < width; ++i) {
            int cidx = (i >> 1) * uv_pixel_stride;
            int Y = static_cast<int>(yrow[i]) - 16;
            int U = static_cast<int>(urow[cidx]) - 128;
            int V = static_cast<int>(vrow[cidx]) - 128;
            int c = 1164 * Y;
            out[0] = clamp_u8((c + 1596 * V) / 1000);
            out[1] = clamp_u8((c - 391 * U - 813 * V) / 1000);
            out[2] = clamp_u8((c + 2018 * U) / 1000);
            out += 3;
        }
    }

    // Bake the sensor orientation into the pixels (clockwise to upright).
    int deg = ((orientation_deg % 360) + 360) % 360;
    int ow = width, oh = height;
    const uint8_t* src = rgb.data();
    std::vector<uint8_t> rot;
    if (deg != 0) {
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
        src = rot.data();
    }

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

// ── 16-bit PNG ───────────────────────────────────────────────────────────────

namespace {

void put_u32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(uint8_t(x >> 24)); v.push_back(uint8_t(x >> 16));
    v.push_back(uint8_t(x >> 8));  v.push_back(uint8_t(x));
}

void put_chunk(std::vector<uint8_t>& out, const char tag[4],
               const uint8_t* data, size_t len) {
    put_u32(out, static_cast<uint32_t>(len));
    const size_t crc_start = out.size();
    out.insert(out.end(), tag, tag + 4);
    if (len) out.insert(out.end(), data, data + len);
    const uLong crc = crc32(crc32(0L, Z_NULL, 0),
                            out.data() + crc_start,
                            static_cast<uInt>(out.size() - crc_start));
    put_u32(out, static_cast<uint32_t>(crc));
}

} // namespace

bool cam::write_png_rgb16(const std::string& path,
                          const uint16_t* rgb16, int width, int height,
                          int orientation_deg) {
    if (!rgb16 || width <= 0 || height <= 0) return false;

    // Bake the sensor orientation into the pixels.
    const bool swap_wh = (orientation_deg == 90 || orientation_deg == 270);
    const int ow = swap_wh ? height : width;
    const int oh = swap_wh ? width  : height;

    // Raw scanlines: one filter byte + ow*3 big-endian shorts per row.
    // Filter 1 (Sub) predicts each byte from the one bpp positions to its left,
    // which on photographic data compresses far better than no filter and costs
    // one subtract. It is exactly reversible, so the image stays lossless.
    const size_t bpp = 6;                       // bytes per pixel, 16-bit RGB
    const size_t row_bytes = static_cast<size_t>(ow) * bpp;
    std::vector<uint8_t> raw(static_cast<size_t>(oh) * (row_bytes + 1));
    std::vector<uint8_t> plain(row_bytes);

    for (int y = 0; y < oh; ++y) {
        uint8_t* line = raw.data() + static_cast<size_t>(y) * (row_bytes + 1);
        *line = 1;                              // filter: Sub

        for (int x = 0; x < ow; ++x) {
            // Source pixel for this output pixel, with the sensor rotation
            // applied. PNG has no orientation tag, so it goes into the pixels.
            int sx, sy;
            switch (orientation_deg) {
                case 90:  sx = y;             sy = height - 1 - x; break;
                case 180: sx = width - 1 - x; sy = height - 1 - y; break;
                case 270: sx = width - 1 - y; sy = x;              break;
                default:  sx = x;             sy = y;              break;
            }
            const uint16_t* src = rgb16 + (static_cast<size_t>(sy) * width + sx) * 3;
            uint8_t* o = plain.data() + static_cast<size_t>(x) * bpp;
            for (int k = 0; k < 3; ++k) {
                o[k * 2]     = uint8_t(src[k] >> 8);   // PNG samples are big-endian
                o[k * 2 + 1] = uint8_t(src[k] & 0xFF);
            }
        }

        // Sub is defined against the *unfiltered* bytes of the same row, so it
        // has to run over the assembled row, not in place while building it.
        uint8_t* dst = line + 1;
        for (size_t i = 0; i < row_bytes; ++i) {
            const uint8_t left = (i >= bpp) ? plain[i - bpp] : 0;
            dst[i] = static_cast<uint8_t>(plain[i] - left);
        }
    }

    uLongf comp_cap = compressBound(static_cast<uLong>(raw.size()));
    std::vector<uint8_t> comp(comp_cap);
    if (compress2(comp.data(), &comp_cap, raw.data(),
                  static_cast<uLong>(raw.size()), Z_BEST_SPEED) != Z_OK) {
        LOGE("PNG deflate failed for %s", path.c_str());
        return false;
    }
    comp.resize(comp_cap);

    std::vector<uint8_t> png;
    const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    png.insert(png.end(), sig, sig + 8);

    uint8_t ihdr[13];
    ihdr[0] = uint8_t(ow >> 24); ihdr[1] = uint8_t(ow >> 16);
    ihdr[2] = uint8_t(ow >> 8);  ihdr[3] = uint8_t(ow);
    ihdr[4] = uint8_t(oh >> 24); ihdr[5] = uint8_t(oh >> 16);
    ihdr[6] = uint8_t(oh >> 8);  ihdr[7] = uint8_t(oh);
    ihdr[8]  = 16;   // bit depth
    ihdr[9]  = 2;    // colour type: truecolour RGB
    ihdr[10] = 0;    // deflate
    ihdr[11] = 0;    // adaptive filtering
    ihdr[12] = 0;    // no interlace
    put_chunk(png, "IHDR", ihdr, sizeof(ihdr));

    // sRGB rendering intent: the develop step encodes sRGB, so say so.
    const uint8_t srgb_intent = 0;   // perceptual
    put_chunk(png, "sRGB", &srgb_intent, 1);

    put_chunk(png, "IDAT", comp.data(), comp.size());
    put_chunk(png, "IEND", nullptr, 0);

    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) { LOGE("cannot open %s", path.c_str()); return false; }
    const size_t wrote = std::fwrite(png.data(), 1, png.size(), f);
    std::fclose(f);
    if (wrote != png.size()) { LOGE("short write for %s", path.c_str()); return false; }

    LOGI("wrote 16-bit PNG %s (%dx%d, %zu bytes)", path.c_str(), ow, oh, png.size());
    return true;
}
