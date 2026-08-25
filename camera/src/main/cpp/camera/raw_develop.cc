#include "raw_develop.hh"

#include "../logger.hh"

#include <algorithm>
#include <cmath>
#include <cstring>

#undef LOG_TAG
#define LOG_TAG "RawDev"

namespace cam {
namespace {

void mat_mul(const double a[9], const double b[9], double out[9]) {
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            out[r * 3 + c] = a[r * 3] * b[c] + a[r * 3 + 1] * b[3 + c] + a[r * 3 + 2] * b[6 + c];
}

bool mat_inv(const double m[9], double out[9]) {
    const double det = m[0] * (m[4] * m[8] - m[5] * m[7])
                     - m[1] * (m[3] * m[8] - m[5] * m[6])
                     + m[2] * (m[3] * m[7] - m[4] * m[6]);
    if (std::fabs(det) < 1e-12) return false;
    const double d = 1.0 / det;
    out[0] =  (m[4] * m[8] - m[5] * m[7]) * d;
    out[1] = -(m[1] * m[8] - m[2] * m[7]) * d;
    out[2] =  (m[1] * m[5] - m[2] * m[4]) * d;
    out[3] = -(m[3] * m[8] - m[5] * m[6]) * d;
    out[4] =  (m[0] * m[8] - m[2] * m[6]) * d;
    out[5] = -(m[0] * m[5] - m[2] * m[3]) * d;
    out[6] =  (m[3] * m[7] - m[4] * m[6]) * d;
    out[7] = -(m[0] * m[7] - m[1] * m[6]) * d;
    out[8] =  (m[0] * m[4] - m[1] * m[3]) * d;
    return true;
}

// XYZ(D50) -> XYZ(D65), Bradford-adapted. Same constants as the video ISP.
const double kBradfordD50ToD65[9] = {
     0.9555766, -0.0230393,  0.0631636,
    -0.0282895,  1.0099416,  0.0210077,
     0.0122982, -0.0204830,  1.3299098,
};

// XYZ(D65) -> linear sRGB. Kept for the legacy SDR path.
const double kXyzD65ToSrgb[9] = {
     3.2404542, -1.5371385, -0.4985314,
    -0.9692660,  1.8760108,  0.0415560,
     0.0556434, -0.2040259,  1.0572252,
};

// XYZ(D65) -> linear BT.2020. Byte-identical to the video ISP's copy in
// isp/raw_video_pipeline.cc, so a still and a clip agree on colour.
// TODO: this whole derivation (matrices, mat_mul/mat_inv, derive_ccm) is
// duplicated in isp/raw_video_pipeline.cc. Lift it into a shared header the next
// time either side is touched.
const double kXyzD65ToBt2020[9] = {
     1.7166512, -0.3556708, -0.2533663,
    -0.6666844,  1.6164812,  0.0157685,
     0.0176399, -0.0427706,  0.9421031,
};

constexpr int32_t kIlluminantD65 = 21;   // EXIF LightSource code

void derive_ccm(const dng::DngMeta& m, const double xyz_to_target[9], float ccm[9]) {
    double cam2xyz[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    if (m.has_fm2 && m.illuminant2 == kIlluminantD65) {
        std::memcpy(cam2xyz, m.forward_matrix2, sizeof(cam2xyz));
    } else if (m.has_fm1) {
        std::memcpy(cam2xyz, m.forward_matrix1, sizeof(cam2xyz));
    } else if (m.has_cm1) {
        double inv[9];
        if (mat_inv(m.color_matrix1, inv)) std::memcpy(cam2xyz, inv, sizeof(cam2xyz));
        else LOGE("color_matrix1 not invertible - identity CCM");
    } else {
        LOGE("no colour matrices - identity CCM");
    }

    double adapt[9], full[9];
    mat_mul(xyz_to_target, kBradfordD50ToD65, adapt);
    mat_mul(adapt, cam2xyz, full);

    // Row-normalise so camera neutral (1,1,1) lands exactly on white.
    for (int r = 0; r < 3; ++r) {
        double e = full[r * 3] + full[r * 3 + 1] + full[r * 3 + 2];
        if (std::fabs(e) < 1e-9) e = 1.0;
        for (int c = 0; c < 3; ++c) ccm[r * 3 + c] = static_cast<float>(full[r * 3 + c] / e);
    }
}

inline float srgb_encode(float v) {
    v = std::min(std::max(v, 0.0f), 1.0f);
    return v <= 0.0031308f ? v * 12.92f
                           : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
}

// SMPTE ST 2084 (PQ). Ported verbatim from isp/shaders_src/debayer_isp.slang's
// pq_encode so a still and a video frame of the same scene land on the same code
// values - if you change one, change both. Input convention: 1.0 == 10000 cd/m2,
// so callers pre-multiply linear scene radiance by kPqScale.
inline float pq_encode(float L) {
    constexpr float m1 = 0.1593017578125f;   // 2610/16384
    constexpr float m2 = 78.84375f;          // 2523/4096*128
    constexpr float c1 = 0.8359375f;         // 3424/4096
    constexpr float c2 = 18.8515625f;        // 2413/4096*32
    constexpr float c3 = 18.6875f;           // 2392/4096*32
    L = std::min(std::max(L, 0.0f), 1.0f);
    const float Lm = std::pow(L, m1);
    return std::pow((c1 + c2 * Lm) / (1.0f + c3 * Lm), m2);
}

// CFA layout: 0=RGGB 1=GRBG 2=GBRG 3=BGGR. Returns 0=R, 1=G, 2=B for (x,y).
inline int cfa_colour(int cfa, int x, int y) {
    const int p = (y & 1) * 2 + (x & 1);
    switch (cfa) {
        case 0: return (p == 0) ? 0 : (p == 3) ? 2 : 1;   // RGGB
        case 1: return (p == 1) ? 0 : (p == 2) ? 2 : 1;   // GRBG
        case 2: return (p == 2) ? 0 : (p == 1) ? 2 : 1;   // GBRG
        default: return (p == 3) ? 0 : (p == 0) ? 2 : 1;  // BGGR
    }
}

} // namespace

bool develop_raw_to_rgb16(const uint16_t* bayer, int width, int height, int stride_bytes,
                          const dng::DngMeta& meta, const float neutral[3],
                          OutputSpace space, std::vector<uint16_t>& out_rgb16) {
    if (!bayer || width <= 1 || height <= 1) return false;
    const int stride_px = stride_bytes > 0 ? stride_bytes / 2 : width;

    // Normalised, black-subtracted plane. Doing this once up front keeps the
    // demosaic itself branch-free on black levels. A single shot really is clipped
    // at white by physics, so clamping at 1.0 here is correct - unlike the merged
    // path, which feeds develop_linear_bayer directly with values above 1.0.
    std::vector<float> lin(static_cast<size_t>(width) * height);
    for (int y = 0; y < height; ++y) {
        const uint16_t* row = bayer + static_cast<size_t>(y) * stride_px;
        for (int x = 0; x < width; ++x) {
            const int p = (y & 1) * 2 + (x & 1);
            const float b = meta.black_level[p];
            const float range = std::max(static_cast<float>(meta.white_level) - b, 1.0f);
            lin[static_cast<size_t>(y) * width + x] =
                std::min(std::max((static_cast<float>(row[x]) - b) / range, 0.0f), 1.0f);
        }
    }
    return develop_linear_bayer(lin.data(), width, height, meta, neutral, space, out_rgb16);
}

// Where highlight reconstruction starts, as a fraction of the clip level. The
// demosaic averages neighbours, so a photosite that saturated can land slightly
// under the clip point; starting a little below it catches those and gives the
// correction a ramp instead of a seam.
static constexpr float kHighlightKnee = 0.95f;

bool develop_linear_bayer(const float* lin, int width, int height,
                          const dng::DngMeta& meta, const float neutral[3],
                          OutputSpace space, std::vector<uint16_t>& out_rgb16,
                          float clip_level, float* out_peak_linear) {
    if (!lin || width <= 1 || height <= 1) return false;

    // White balance from the as-shot neutral. Reciprocal gains, matching the video
    // ISP (isp/raw_video_pipeline.cc set_neutral) so a still and a clip of the same
    // scene land on the same absolute brightness - which PQ, being an absolute
    // transfer function, makes visible. This used to be green-normalised here
    // (g[1] == 1), differing from the video by a factor of neutral[1].
    float g[3] = {1.0f, 1.0f, 1.0f};
    if (neutral && neutral[0] > 1e-6f && neutral[1] > 1e-6f && neutral[2] > 1e-6f) {
        g[0] = 1.0f / neutral[0];
        g[1] = 1.0f / neutral[1];
        g[2] = 1.0f / neutral[2];
    }

    const bool pq = (space == OutputSpace::kBt2020Pq);

    float ccm[9];
    derive_ccm(meta, pq ? kXyzD65ToBt2020 : kXyzD65ToSrgb, ccm);

    LOGI("develop %dx%d space=%s pq_scale=%.3f wb=%.3f/%.3f/%.3f",
         width, height, pq ? "bt2020pq" : "srgb", pq ? kPqScale : 0.0f, g[0], g[1], g[2]);

    out_rgb16.assign(static_cast<size_t>(width) * height * 3, 0);

    // Tracked in the LINEAR domain deliberately: pq_encode() clamps at 1.0, so
    // the encoded buffer cannot distinguish "peaks at 10000 nits" from "a few
    // samples went over the ceiling". Reading it back reported 10000 for files
    // whose real peak was ~3400.
    float peak_linear = 0.0f;

    auto at = [&](int x, int y) -> float {
        x = std::min(std::max(x, 0), width - 1);
        y = std::min(std::max(y, 0), height - 1);
        return lin[static_cast<size_t>(y) * width + x];
    };

    const int cfa = meta.cfa;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const float c  = at(x, y);
            const float n1 = at(x, y - 1), s1 = at(x, y + 1);
            const float w1 = at(x - 1, y), e1 = at(x + 1, y);
            const float n2 = at(x, y - 2), s2 = at(x, y + 2);
            const float w2 = at(x - 2, y), e2 = at(x + 2, y);
            const float nw = at(x - 1, y - 1), ne = at(x + 1, y - 1);
            const float sw = at(x - 1, y + 1), se = at(x + 1, y + 1);

            // Malvar-He-Cutler, the same kernels the video demosaic uses. Each
            // set of weights sums to 8.
            const float g_at_rb = (4.0f * c + 2.0f * (n1 + s1 + w1 + e1)
                                   - (n2 + s2 + w2 + e2)) / 8.0f;
            const float rb_h = (5.0f * c + 4.0f * (w1 + e1) - (nw + ne + sw + se)
                                - (w2 + e2) + 0.5f * (n2 + s2)) / 8.0f;
            const float rb_v = (5.0f * c + 4.0f * (n1 + s1) - (nw + ne + sw + se)
                                - (n2 + s2) + 0.5f * (w2 + e2)) / 8.0f;
            const float rb_d = (6.0f * c + 2.0f * (nw + ne + sw + se)
                                - 1.5f * (n2 + s2 + w2 + e2)) / 8.0f;

            float rgb[3];
            const int col = cfa_colour(cfa, x, y);
            if (col == 1) {
                // On a green site the two chroma channels come from the
                // horizontal and vertical neighbours; which is which depends on
                // whether the row above/below carries red or blue.
                const int row_col = cfa_colour(cfa, x, y - 1);
                rgb[1] = c;
                if (row_col == 0) { rgb[0] = rb_v; rgb[2] = rb_h; }
                else              { rgb[0] = rb_h; rgb[2] = rb_v; }
            } else if (col == 0) {
                rgb[0] = c; rgb[1] = g_at_rb; rgb[2] = rb_d;
            } else {
                rgb[2] = c; rgb[1] = g_at_rb; rgb[0] = rb_d;
            }

            // White balance, then camera -> target primaries, then the transfer curve.
            float r = rgb[0] * g[0], gg = rgb[1] * g[1], b = rgb[2] * g[2];

            // Highlight reconstruction. A saturated photosite carries no
            // information beyond "at least this bright", but white balance
            // multiplies it by that channel's gain anyway — so a blown highlight
            // comes out tinted by whichever channel clipped first. On this sensor
            // green saturates earliest (it is the most sensitive: 1.8% of green
            // sites clipped in a test frame against 0.5% blue and 0.3% red) while
            // red and blue carry ~1.9x gains, which is why strong light went green
            // at the fringes and magenta in the core.
            //
            // A clipped channel is raised to the brightest white-balanced channel,
            // ramped in over the last stretch before the clip. Channels that did
            // NOT clip are left exactly alone, so a genuinely saturated colour — a
            // red lamp whose red pins while green and blue sit far below — keeps
            // its hue instead of being bleached to white. Channels only ever rise,
            // so nothing darkens and the HDR headroom above the clip is untouched.
            if (clip_level > 0.0f) {
                const float knee = clip_level * kHighlightKnee;
                const float inv  = 1.0f / std::max(clip_level - knee, 1e-6f);
                const float peak = std::max(std::max(r, gg), b);
                auto ramp = [&](float pre) {
                    const float t = std::min(std::max((pre - knee) * inv, 0.0f), 1.0f);
                    return t * t * (3.0f - 2.0f * t);       // smoothstep
                };
                r  += (peak - r ) * ramp(rgb[0]);
                gg += (peak - gg) * ramp(rgb[1]);
                b  += (peak - b ) * ramp(rgb[2]);
            }

            float o[3];
            for (int k = 0; k < 3; ++k)
                o[k] = ccm[k * 3] * r + ccm[k * 3 + 1] * gg + ccm[k * 3 + 2] * b;

            uint16_t* dst = &out_rgb16[(static_cast<size_t>(y) * width + x) * 3];
            // Clamp at 0 only. Chroma can undershoot near clipped edges, but the
            // top must stay open: values above 1.0 are the bracket's recovered
            // highlights, and clamping them here is the bug this path exists to fix.
            for (int k = 0; k < 3; ++k) {
                const float v = std::max(o[k], 0.0f);
                if (v > peak_linear) peak_linear = v;
                const float e = pq ? pq_encode(v * kPqScale) : srgb_encode(v);
                dst[k] = static_cast<uint16_t>(std::lround(e * 65535.0f));
            }
        }
    }
    if (out_peak_linear) *out_peak_linear = peak_linear;
    return true;
}

} // namespace cam
