#include "raw_develop.hh"

#include "../logger.hh"

#include <vector>

// stb implementation lives in still_writer.cc; here we only need the prototypes.
#include "../third_party/stb_image_write.h"

#undef LOG_TAG
#define LOG_TAG "RawDevelop"

namespace cam {
namespace {

// 0=R, 1=G, 2=B for pixel (x,y). cfa: 0=RGGB 1=GRBG 2=GBRG 3=BGGR
// (Camera2 SENSOR_INFO_COLOR_FILTER_ARRANGEMENT).
inline int cfa_color(int cfa, int x, int y) {
    static const int pat[4][4] = {
        {0, 1, 1, 2},  // RGGB
        {1, 0, 2, 1},  // GRBG
        {1, 2, 0, 1},  // GBRG
        {2, 1, 1, 0},  // BGGR
    };
    return pat[cfa & 3][(y & 1) * 2 + (x & 1)];
}

} // namespace

bool develop_to_hdr(const hdr::MergeResult& r, const std::string& out_path) {
    if (!r.ok || r.radiance.empty()) { LOGE("develop: empty merge result"); return false; }
    const int W = r.W, H = r.H;
    const int cfa = r.meta.cfa;
    const float* rad = r.radiance.data();

    // White balance: divide camera RGB by the as-shot neutral, normalized to green
    // so overall brightness is unchanged.
    const float ng = r.neutral[1] != 0.0f ? r.neutral[1] : 1.0f;
    const float wbR = ng / (r.neutral[0] != 0.0f ? r.neutral[0] : 1.0f);
    const float wbB = ng / (r.neutral[2] != 0.0f ? r.neutral[2] : 1.0f);

    // camera -> XYZ(D50): DNG ForwardMatrix (row-major). Prefer FM1, else FM2.
    const bool haveFM = r.meta.has_fm1 || r.meta.has_fm2;
    const double* FM = r.meta.has_fm1 ? r.meta.forward_matrix1 : r.meta.forward_matrix2;
    if (!haveFM) LOGE("develop: no ForwardMatrix — colour will be approximate");

    // XYZ(D50) -> linear sRGB (D65), Bradford-adapted (standard constants).
    static const double X2S[9] = {
         3.1338561, -1.6168667, -0.4906146,
        -0.9787684,  1.9161415,  0.0334540,
         0.0719453, -0.2289914,  1.4052427,
    };

    // Normalize so the 0 EV usable span maps to ~1.0 (recovered highlights > 1.0).
    const float inv = r.ref_span > 0.0 ? static_cast<float>(1.0 / r.ref_span) : 1.0f;

    std::vector<float> rgb(static_cast<size_t>(W) * H * 3);

    // Estimate one non-native channel at (x,y) by a 3x3 normalized average of the
    // CFA sites that carry it (bilinear-equivalent, generic over the CFA phase).
    auto gather = [&](int oc, int x, int y) -> float {
        float s = 0.0f; int n = 0;
        for (int dy = -1; dy <= 1; ++dy) {
            const int yy = y + dy; if (yy < 0 || yy >= H) continue;
            for (int dx = -1; dx <= 1; ++dx) {
                const int xx = x + dx; if (xx < 0 || xx >= W) continue;
                if (cfa_color(cfa, xx, yy) == oc) { s += rad[static_cast<size_t>(yy) * W + xx]; ++n; }
            }
        }
        return n ? s / n : 0.0f;
    };

    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            const int c = cfa_color(cfa, x, y);
            float cam[3];
            cam[c] = rad[static_cast<size_t>(y) * W + x];          // native channel: exact
            for (int oc = 0; oc < 3; ++oc) if (oc != c) cam[oc] = gather(oc, x, y);

            // scene-linear + white balance
            float cr = cam[0] * inv * wbR;
            float cg = cam[1] * inv;
            float cb = cam[2] * inv * wbB;

            float lr, lg, lb;
            if (haveFM) {
                const double Xc = FM[0] * cr + FM[1] * cg + FM[2] * cb;
                const double Yc = FM[3] * cr + FM[4] * cg + FM[5] * cb;
                const double Zc = FM[6] * cr + FM[7] * cg + FM[8] * cb;
                lr = static_cast<float>(X2S[0] * Xc + X2S[1] * Yc + X2S[2] * Zc);
                lg = static_cast<float>(X2S[3] * Xc + X2S[4] * Yc + X2S[5] * Zc);
                lb = static_cast<float>(X2S[6] * Xc + X2S[7] * Yc + X2S[8] * Zc);
            } else {
                lr = cr; lg = cg; lb = cb;  // no colour matrix — pass through
            }

            float* o = &rgb[(static_cast<size_t>(y) * W + x) * 3];
            o[0] = lr > 0.0f ? lr : 0.0f;
            o[1] = lg > 0.0f ? lg : 0.0f;
            o[2] = lb > 0.0f ? lb : 0.0f;
        }
    }

    const bool ok = stbi_write_hdr(out_path.c_str(), W, H, 3, rgb.data()) != 0;
    LOGI("HDR develop %s: %dx%d -> %s", ok ? "ok" : "FAILED", W, H, out_path.c_str());
    return ok;
}

} // namespace cam
