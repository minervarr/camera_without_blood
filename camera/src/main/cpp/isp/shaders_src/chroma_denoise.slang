// Chroma-only denoise, run after the ISP pass.
//
// Colour noise is what survives most visibly in low light: the luma plane can
// look clean while CbCr still crawls with coloured blotches. Because human
// vision resolves chroma far more coarsely than luma, chroma can be averaged
// hard with no perceived loss of detail — as long as the averaging does not
// cross an object boundary and drag colour across it.
//
// So this is a JOINT bilateral: the spatial term is a plain gaussian, but the
// range term is driven by the LUMA plane, which is sharp and already final.
// Neighbours on the far side of an edge get a near-zero weight because their
// luma differs, so colour never bleeds across the edge that luma sees.
//
// The luma plane is only READ here. It is never written, so enabling this pass
// leaves the ISP's luma output byte-for-byte identical.
//
// Dispatch: one thread per chroma site, grid = (out_w/2, out_h/2), 8x8 groups.

[[vk::binding(0, 0)]] RWStructuredBuffer<uint> outBuf;     // P010: read Y, write CbCr
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> chromaBuf;  // noisy CbCr from the ISP

struct PushConstants {
    uint  outW, outH;
    uint  stridePixels;
    uint  uvWordOffset;
    uint  chromaStride;   // in words (= out_w / 2)
    int   radius;         // half window, in chroma sites
    float sigmaS;         // spatial gaussian sigma, in sites
    float sigmaL;         // luma range sigma, in 10-bit units
};
[[vk::push_constant]] ConstantBuffer<PushConstants> push;

// Mean luma of the 2x2 quad backing chroma site (cx, cy), in 10-bit units.
// P010 keeps the 10 bits in the HIGH end of each 16-bit word, hence the >> 6.
float site_luma(int cx, int cy) {
    int nx = int(push.outW) / 2 - 1;
    int ny = int(push.outH) / 2 - 1;
    cx = clamp(cx, 0, nx);
    cy = clamp(cy, 0, ny);

    uint halfStride = push.stridePixels >> 1;
    uint w0 = outBuf[uint(cy * 2)     * halfStride + uint(cx)];
    uint w1 = outBuf[uint(cy * 2 + 1) * halfStride + uint(cx)];
    float y00 = float((w0 & 0xFFFFu) >> 6);
    float y10 = float((w0 >> 16)     >> 6);
    float y01 = float((w1 & 0xFFFFu) >> 6);
    float y11 = float((w1 >> 16)     >> 6);
    return 0.25f * (y00 + y10 + y01 + y11);
}

// Noisy (Cb, Cr) at a chroma site, in 10-bit units.
float2 site_chroma(int cx, int cy) {
    int nx = int(push.outW) / 2 - 1;
    int ny = int(push.outH) / 2 - 1;
    cx = clamp(cx, 0, nx);
    cy = clamp(cy, 0, ny);
    uint packed = chromaBuf[uint(cy) * push.chromaStride + uint(cx)];
    return float2(float((packed & 0xFFFFu) >> 6), float((packed >> 16) >> 6));
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    uint cxu = tid.x, cyu = tid.y;
    if (cxu >= push.outW / 2u || cyu >= push.outH / 2u) return;

    int cx = int(cxu), cy = int(cyu);

    float lumaC   = site_luma(cx, cy);
    float invS2   = 1.0f / max(2.0f * push.sigmaS * push.sigmaS, 1e-6f);
    float invL2   = 1.0f / max(2.0f * push.sigmaL * push.sigmaL, 1e-6f);

    float2 acc  = float2(0.0f, 0.0f);
    float  wsum = 0.0f;

    for (int dy = -push.radius; dy <= push.radius; ++dy) {
        for (int dx = -push.radius; dx <= push.radius; ++dx) {
            float r2 = float(dx * dx + dy * dy);
            float dl = site_luma(cx + dx, cy + dy) - lumaC;

            float w = exp(-r2 * invS2 - dl * dl * invL2);
            acc  += w * site_chroma(cx + dx, cy + dy);
            wsum += w;
        }
    }

    float2 c = acc / max(wsum, 1e-8f);

    uint cb = uint(clamp(round(c.x), 0.0f, 1023.0f)) << 6;
    uint cr = uint(clamp(round(c.y), 0.0f, 1023.0f)) << 6;

    uint halfStride = push.stridePixels >> 1;
    outBuf[push.uvWordOffset + uint(cy) * halfStride + uint(cx)] = cb | (cr << 16);
}
