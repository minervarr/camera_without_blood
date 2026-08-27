// 2x2 bin: RAW Bayer -> half-resolution LINEAR camera RGB.
//
// The first pass of the binned RAW video path. Each 2x2 CFA quad becomes exactly
// one output pixel — R from the single red photosite, B from the single blue one,
// G from the mean of the two greens. There is NO demosaic and no interpolation:
// every output channel is a real measurement of that quad, so the pass cannot
// invent the zipper/maze artifacts a demosaic can, and it costs a handful of
// loads per pixel instead of a 5x5 kernel.
//
// What binning does and does NOT do for noise. Averaging the two greens halves
// the green variance (-3 dB); R and B each come from ONE photosite, so their
// noise is unchanged. Binning is therefore not a denoise on its own — it is what
// makes the real denoise (nlm_rgb.slang, the next pass) affordable, by handing it
// a quarter as many pixels.
//
// Output is scene-referred linear camera RGB, black-subtracted and normalized to
// 1.0 at the sensor's white level — deliberately BEFORE white balance, the CCM
// and PQ. The denoise has to see the samples while their noise is still the
// sensor's (a known variance at a known signal level); the colour transforms are
// applied by nlm_rgb.slang on the way out.
//
// Dispatch: one thread per OUTPUT pixel, grid = (out_w, out_h), 8x8 groups.
// No groupshared tile: neighbouring threads read disjoint quads, so there is
// nothing to share (unlike debayer_isp.slang, whose 5x5 reach overlaps heavily).

[[vk::binding(0, 0)]] RWStructuredBuffer<uint>  rawBuf;  // RAW16, 2 px per uint32
[[vk::binding(1, 0)]] RWStructuredBuffer<float> rgbBuf;  // linear camera RGB, 3 floats/px

struct PushConstants {
    uint2  outDim;
    uint2  rawDim;
    uint   cfa;             // low 2 bits: CFA pattern
    float  white;
    float2 _pad;
    float4 black;           // per 2x2 CFA position, row-major
};
[[vk::push_constant]] ConstantBuffer<PushConstants> push;

float black_at(int x, int y) {
    int idx = (y & 1) * 2 + (x & 1);
    return (idx == 0) ? push.black.x : (idx == 1) ? push.black.y
         : (idx == 2) ? push.black.z : push.black.w;
}

// Normalized against this CFA position's own black level.
//
// The top is clamped; the BOTTOM DELIBERATELY IS NOT. Sensor noise straddles the
// declared black level — measured on real S23 Ultra frames, raw minima of 43 and
// 42 against a declared BlackLevel of 64 — so a good share of shadow samples are
// legitimately negative after black subtraction. Clamping them to zero (what
// saturate() did here, and what debayer_isp.slang still does) keeps only the
// positive half of a zero-mean distribution, and a clipped Gaussian has a
// POSITIVE MEAN that varies per pixel and per CFA channel. That turns noise,
// which averaging can remove, into a per-channel DC bias, which it cannot — it
// shows up as fixed coloured speckle in the shadows that no downstream denoise
// can touch.
//
// Letting the negatives through keeps shadow noise zero-mean, so nlm_rgb.slang
// can actually cancel it. Nothing downstream is harmed: the NLM guards its
// signal term with max(centreY, 0), develop_rgb clamps with max(lin, 0) after
// the CCM, pq_encode saturates, and to_p010 clamps again.
float norm_at(uint v, int x, int y) {
    float b = black_at(x, y);
    return min((float(v) - b) / max(push.white - b, 1.0f), 1.0f);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    uint ox = tid.x, oy = tid.y;
    if (ox >= push.outDim.x || oy >= push.outDim.y) return;

    // The quad origin is always even, so it lands on the CFA phase and the quad
    // holds exactly one R, two G and one B whatever the pattern is.
    int x0 = int(ox) * 2, y0 = int(oy) * 2;

    // Two WORD loads, not four sample loads. x0 is always even and rawW is even
    // (init() refuses the binned path otherwise), so each quad row is exactly one
    // aligned uint32 holding both of its pixels. The four-call version went
    // through a per-sample clamp(), which hid that from the compiler and cost
    // four dependent loads instead of two. Bit-exact: verified against the
    // reference color_at() for all four CFA patterns.
    //
    // No clamp is needed: out_w = (raw_w/2) & ~1, so x0+1 <= raw_w-1 by
    // construction, and likewise for y.
    uint rowTop = uint(y0)     * push.rawDim.x + uint(x0);
    uint rowBot = uint(y0 + 1) * push.rawDim.x + uint(x0);
    uint wTop = rawBuf[rowTop >> 1];
    uint wBot = rawBuf[rowBot >> 1];

    float v00 = norm_at(wTop & 0xFFFFu, x0,     y0);
    float v10 = norm_at(wTop >> 16,     x0 + 1, y0);
    float v01 = norm_at(wBot & 0xFFFFu, x0,     y0 + 1);
    float v11 = norm_at(wBot >> 16,     x0 + 1, y0 + 1);

    // The quad origin is even, so its 2x2 colour layout is fixed by the CFA
    // pattern alone — resolve it once instead of calling color_at four times.
    // Pattern (push.cfa & 3): 0=RGGB, 1=GRBG, 2=GBRG, 3=BGGR.
    uint pat = push.cfa & 3u;
    float3 acc;
    if (pat == 0u)      acc = float3(v00,          v10 + v01, v11);        // R G G B
    else if (pat == 1u) acc = float3(v10,          v00 + v11, v01);        // G R B G
    else if (pat == 2u) acc = float3(v01,          v00 + v11, v10);        // G B R G
    else                acc = float3(v11,          v10 + v01, v00);        // B G G R

    acc.g *= 0.5f;   // two greens per quad; R and B are single samples

    // May be slightly negative in the shadows, on purpose — see norm_at.
    // The clamps that matter are at the end of nlm_rgb.slang's develop chain.

    uint base = (oy * push.outDim.x + ox) * 3u;
    rgbBuf[base + 0u] = acc.r;
    rgbBuf[base + 1u] = acc.g;
    rgbBuf[base + 2u] = acc.b;
}
