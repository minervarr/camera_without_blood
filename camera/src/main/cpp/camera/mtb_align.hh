#pragma once

#include <vector>

namespace mtb {

// A downsampled single-channel image (luma proxy) used for exposure-invariant
// alignment. Callers build it however suits their domain (Bayer 2x2-bin, RGB
// luma, ...); the pyramid search below is shared.
struct Proxy {
    int w = 0, h = 0;
    std::vector<float> v;
};

// Median-threshold-bitmap pyramid alignment (Ward): returns the proxy-space shift
// (dx,dy) that best aligns `mov` to `ref`. Exposure-invariant (median threshold),
// integer pixel. Scale the result to full-image pixels at the call site.
void align_mtb(const Proxy& ref, const Proxy& mov, int& out_dx, int& out_dy);

} // namespace mtb
