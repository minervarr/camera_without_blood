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

// XYZ(D65) -> linear sRGB. (The video path targets BT.2020 instead; a PNG has
// no colour-space signalling that viewers reliably honour, so sRGB it is.)
const double kXyzD65ToSrgb[9] = {
     3.2404542, -1.5371385, -0.4985314,
    -0.9692660,  1.8760108,  0.0415560,
     0.0556434, -0.2040259,  1.0572252,
};

constexpr int32_t kIlluminantD65 = 21;   // EXIF LightSource code

void derive_ccm(const dng::DngMeta& m, float ccm[9]) {
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
    mat_mul(kXyzD65ToSrgb, kBradfordD50ToD65, adapt);
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
                          std::vector<uint16_t>& out_rgb16) {
    if (!bayer || width <= 1 || height <= 1) return false;
    const int stride_px = stride_bytes > 0 ? stride_bytes / 2 : width;

    // Normalised, black-subtracted plane. Doing this once up front keeps the
    // demosaic itself branch-free on black levels.
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

    // White balance from the as-shot neutral, green-normalised.
    float g[3] = {1.0f, 1.0f, 1.0f};
    if (neutral && neutral[0] > 1e-6f && neutral[1] > 1e-6f && neutral[2] > 1e-6f) {
        g[0] = neutral[1] / neutral[0];
        g[1] = 1.0f;
        g[2] = neutral[1] / neutral[2];
    }

    float ccm[9];
    derive_ccm(meta, ccm);

    out_rgb16.assign(static_cast<size_t>(width) * height * 3, 0);

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

            // White balance, then camera -> sRGB, then the sRGB curve.
            const float r = rgb[0] * g[0], gg = rgb[1] * g[1], b = rgb[2] * g[2];
            float o[3];
            for (int k = 0; k < 3; ++k)
                o[k] = ccm[k * 3] * r + ccm[k * 3 + 1] * gg + ccm[k * 3 + 2] * b;

            uint16_t* dst = &out_rgb16[(static_cast<size_t>(y) * width + x) * 3];
            for (int k = 0; k < 3; ++k)
                dst[k] = static_cast<uint16_t>(std::lround(srgb_encode(o[k]) * 65535.0f));
        }
    }
    return true;
}

} // namespace cam
