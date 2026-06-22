#include "mtb_align.hh"

#include <algorithm>
#include <climits>
#include <vector>

namespace mtb {
namespace {

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

} // namespace

void align_mtb(const Proxy& ref, const Proxy& mov, int& out_dx, int& out_dy) {
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
    out_dx = dx; out_dy = dy;
}

} // namespace mtb
