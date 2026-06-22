#include "hdr_merge.hh"

#include "../logger.hh"

#include <algorithm>
#include <climits>
#include <cmath>
#include <vector>

#undef LOG_TAG
#define LOG_TAG "HdrMerge"

namespace hdr {
namespace {

// 2x2-binned luma proxy: average each CFA cell into one value. Proxy dimensions
// are (w/2)x(h/2), so a 1px shift in the proxy is a 2px (EVEN) shift in the full
// image — which keeps the Bayer phase (R stays on R) after alignment.
struct Proxy {
    int w = 0, h = 0;
    std::vector<float> v;
};

Proxy make_proxy(const BracketFrame& f) {
    Proxy p;
    p.w = f.width / 2;
    p.h = f.height / 2;
    p.v.resize(static_cast<size_t>(p.w) * p.h);
    for (int y = 0; y < p.h; ++y) {
        const uint16_t* r0 = &f.data[static_cast<size_t>(2 * y)     * f.width];
        const uint16_t* r1 = &f.data[static_cast<size_t>(2 * y + 1) * f.width];
        for (int x = 0; x < p.w; ++x) {
            const int x2 = 2 * x;
            p.v[static_cast<size_t>(y) * p.w + x] =
                0.25f * (static_cast<float>(r0[x2]) + r0[x2 + 1] + r1[x2] + r1[x2 + 1]);
        }
    }
    return p;
}

Proxy downsample(const Proxy& s) {
    Proxy p;
    p.w = s.w / 2;
    p.h = s.h / 2;
    p.v.resize(static_cast<size_t>(p.w) * p.h);
    for (int y = 0; y < p.h; ++y) {
        const float* a = &s.v[static_cast<size_t>(2 * y)     * s.w];
        const float* b = &s.v[static_cast<size_t>(2 * y + 1) * s.w];
        for (int x = 0; x < p.w; ++x) {
            const int x2 = 2 * x;
            p.v[static_cast<size_t>(y) * p.w + x] = 0.25f * (a[x2] + a[x2 + 1] + b[x2] + b[x2 + 1]);
        }
    }
    return p;
}

float median_of(const std::vector<float>& v) {
    if (v.empty()) return 0.0f;
    std::vector<float> t = v;
    const size_t n = t.size() / 2;
    std::nth_element(t.begin(), t.begin() + n, t.end());
    return t[n];
}

// Median-threshold-bitmap mismatch fraction when `mov` is shifted by (dx,dy).
long mtb_error(const Proxy& ref, float refMed, const Proxy& mov, float movMed, int dx, int dy) {
    long err = 0, cnt = 0;
    for (int y = 0; y < ref.h; ++y) {
        const int my = y + dy;
        if (my < 0 || my >= mov.h) continue;
        for (int x = 0; x < ref.w; ++x) {
            const int mx = x + dx;
            if (mx < 0 || mx >= mov.w) continue;
            const bool rb = ref.v[static_cast<size_t>(y) * ref.w + x] > refMed;
            const bool mb = mov.v[static_cast<size_t>(my) * mov.w + mx] > movMed;
            if (rb != mb) ++err;
            ++cnt;
        }
    }
    if (cnt == 0) return LONG_MAX;
    return static_cast<long>(static_cast<double>(err) / cnt * 1e6);
}

// Pyramid MTB alignment: proxy-space shift (dx,dy) that best aligns mov to ref.
void align_mtb(const Proxy& ref, const Proxy& mov, int& outDx, int& outDy) {
    std::vector<Proxy> rp{ref}, mp{mov};
    while (rp.back().w > 16 && rp.back().h > 16) {
        rp.push_back(downsample(rp.back()));
        mp.push_back(downsample(mp.back()));
    }
    int dx = 0, dy = 0;
    for (int l = static_cast<int>(rp.size()) - 1; l >= 0; --l) {
        dx *= 2; dy *= 2;
        const float rm = median_of(rp[l].v), mm = median_of(mp[l].v);
        long best = LONG_MAX; int bdx = dx, bdy = dy;
        for (int oy = -1; oy <= 1; ++oy)
            for (int ox = -1; ox <= 1; ++ox) {
                const long e = mtb_error(rp[l], rm, mp[l], mm, dx + ox, dy + oy);
                if (e < best) { best = e; bdx = dx + ox; bdy = dy + oy; }
            }
        dx = bdx; dy = bdy;
    }
    outDx = dx; outDy = dy;
}

} // namespace

MergeResult merge_raw_bracket(const std::vector<BracketFrame>& frames,
                              const dng::DngMeta& base_meta) {
    MergeResult R;
    if (frames.size() < 2) {
        LOGE("merge: need >= 2 frames, have %zu", frames.size());
        return R;
    }
    const int W = frames[0].width, H = frames[0].height;
    for (const auto& f : frames) {
        if (f.width != W || f.height != H || f.data.size() < static_cast<size_t>(W) * H) {
            LOGE("merge: geometry mismatch");
            return R;
        }
    }

    // Reference = the frame whose burst index is the middle (the 0 EV shot).
    const int target = static_cast<int>(frames.size()) / 2;
    int ref = 0, bestDist = INT_MAX;
    for (size_t i = 0; i < frames.size(); ++i) {
        const int d = std::abs(frames[i].index - target);
        if (d < bestDist) { bestDist = d; ref = static_cast<int>(i); }
    }

    // Relative exposure (light gathered ∝ exposure_time × ISO), normalized to ref.
    // Fall back to nominal 2-EV steps from the burst index if metadata is missing.
    bool haveExp = true;
    for (const auto& f : frames) if (f.exposure_ns <= 0 || f.iso <= 0) haveExp = false;
    const double eRef = haveExp
        ? static_cast<double>(frames[ref].exposure_ns) * frames[ref].iso : 1.0;
    std::vector<double> eRel(frames.size());
    for (size_t i = 0; i < frames.size(); ++i) {
        eRel[i] = haveExp
            ? (static_cast<double>(frames[i].exposure_ns) * frames[i].iso) / eRef
            : std::pow(2.0, 2.0 * (frames[i].index - frames[ref].index));  // 2 EV/step
        if (eRel[i] <= 0) eRel[i] = 1.0;
    }

    // Align every non-ref frame to the reference (even full-pixel shifts).
    const Proxy rpx = make_proxy(frames[ref]);
    std::vector<int> shx(frames.size(), 0), shy(frames.size(), 0);
    for (size_t i = 0; i < frames.size(); ++i) {
        if (static_cast<int>(i) == ref) continue;
        int dx = 0, dy = 0;
        const Proxy mpx = make_proxy(frames[i]);
        align_mtb(rpx, mpx, dx, dy);
        shx[i] = 2 * dx; shy[i] = 2 * dy;
        LOGI("frame idx %d aligned by (%d,%d) px, eRel=%.3f", frames[i].index, shx[i], shy[i], eRel[i]);
    }

    auto black_at = [](const BracketFrame& f, int x, int y) -> float {
        return f.black[(y & 1) * 2 + (x & 1)];
    };

    // Output range: keep mid-tones at the reference (0 EV) brightness with black at
    // 0, recovered highlights filling the headroom up to 1/min(eRel)x.
    const float refBlackMean =
        0.25f * (frames[ref].black[0] + frames[ref].black[1] + frames[ref].black[2] + frames[ref].black[3]);
    const double refWhite = frames[ref].white > 0 ? frames[ref].white
                          : (base_meta.white_level > 0 ? base_meta.white_level : 65535);
    const double refSpan = std::max(1.0, refWhite - refBlackMean);
    double minRel = 1.0;
    for (double e : eRel) minRel = std::min(minRel, e);
    const double maxBoost = minRel > 0 ? 1.0 / minRel : 1.0;
    const double outWhite = std::min(65535.0, refSpan * maxBoost);

    std::vector<float> radiance(static_cast<size_t>(W) * H);
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            double num = 0.0, den = 0.0;
            for (size_t i = 0; i < frames.size(); ++i) {
                const int sx = x - shx[i], sy = y - shy[i];
                if (sx < 0 || sx >= W || sy < 0 || sy >= H) continue;  // border-invalid
                const BracketFrame& f = frames[i];
                const float raw   = static_cast<float>(f.data[static_cast<size_t>(sy) * W + sx]);
                const float bl    = black_at(f, sx, sy);
                const float white = f.white > 0 ? static_cast<float>(f.white) : static_cast<float>(refWhite);
                if (raw >= 0.98f * white) continue;                    // clipped -> drop
                float lin = raw - bl; if (lin < 0) lin = 0;
                const float v = (white > bl) ? (raw - bl) / (white - bl) : 0.0f;  // exposedness 0..1
                const double w = std::exp(-((v - 0.5) * (v - 0.5)) / (2.0 * 0.2 * 0.2)) + 1e-3;
                num += w * (static_cast<double>(lin) / eRel[i]);       // radiance in ref counts
                den += w;
            }
            double merged;
            if (den > 0.0) {
                merged = num / den;
            } else {  // everything clipped/invalid here -> fall back to the ref pixel
                const BracketFrame& f = frames[ref];
                float lin = static_cast<float>(f.data[static_cast<size_t>(y) * W + x]) - black_at(f, x, y);
                if (lin < 0) lin = 0;
                merged = lin;
            }
            radiance[static_cast<size_t>(y) * W + x] = static_cast<float>(merged);
        }
    }

    R.ok = true;
    R.W = W; R.H = H;
    R.radiance = std::move(radiance);
    R.ref_span = refSpan;
    R.out_white = outWhite;
    for (int i = 0; i < 3; ++i) R.neutral[i] = frames[ref].neutral[i];
    R.meta = base_meta;            // static tags (CFA, colour matrices)
    R.meta.width = W; R.meta.height = H; R.meta.stride_bytes = W * 2;
    R.meta.cfa = base_meta.cfa;
    LOGI("merge ok: %dx%d, %zu frames, refSpan=%.0f outWhite=%.0f", W, H, frames.size(), refSpan, outWhite);
    return R;
}

bool write_merged_dng(const MergeResult& r, const std::string& out_path) {
    if (!r.ok) return false;
    std::vector<uint16_t> out(static_cast<size_t>(r.W) * r.H);
    const long cap = static_cast<long>(r.out_white);
    for (size_t i = 0; i < out.size(); ++i) {
        long o = std::lround(r.radiance[i]);
        if (o < 0) o = 0;
        if (o > cap) o = cap;
        out[i] = static_cast<uint16_t>(o);
    }
    dng::DngMeta m = r.meta;
    for (int i = 0; i < 4; ++i) m.black_level[i] = 0.0f;       // black already subtracted
    m.white_level = static_cast<int>(std::lround(r.out_white));
    for (int i = 0; i < 3; ++i) m.as_shot_neutral[i] = r.neutral[i];
    m.has_neutral = true;
    const bool ok = dng::write_dng(out_path, out.data(), m);
    LOGI("merged DNG %s -> %s", ok ? "ok" : "FAILED", out_path.c_str());
    return ok;
}

} // namespace hdr
