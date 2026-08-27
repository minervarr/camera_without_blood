// Non-Local Means denoise on the 2x2-binned RGB plane, then develop and pack P010.
//
// The second (and last) GPU pass of the binned RAW video path. bin_isp.slang has
// already turned the Bayer mosaic into half-resolution LINEAR camera RGB, so
// this pass:
//   1. NLM-denoises that plane,
//   2. applies highlight reconstruction, white balance, the sensor->BT.2020 CCM
//      and PQ ST 2084 — the same tail as debayer_isp.slang's develop(),
//   3. converts to BT.2020 non-constant-luminance Y'CbCr and packs P010.
// Folding 2 and 3 in here rather than adding a third dispatch keeps the whole
// binned path at two passes.
//
// WHY THIS IS CHEAPER THAN nlm_bayer.slang, and by how much.
// It runs at a quarter of the pixels (2040x1530 vs 4080x3060). But a colour NLM
// is not simply a mono NLM: comparing patches on all three channels would triple
// the SSD work and give back most of that saving. So the patch distance is
// computed on a LUMA plane derived once during the tile fill, and the resulting
// weights are applied to all three channels. SSD cost per pixel is then the same
// as the Bayer version's, and only the accumulation is 3-wide — roughly a third
// of nlm_bayer.slang's total cost, not three quarters.
//
// The two rules that make nlm_bayer.slang fast are kept verbatim, and matter for
// the same reason: the 3x3 centre patch lives in three registers (invariant
// across every candidate), and S/P are COMPILE-TIME constants so the loops unroll
// and the patch indices stay static. A runtime P makes the indices dynamic and
// spills the patch to scratch memory, which costs more than the tiling saves.
// After changing this file, spirv-dis the result and confirm there is no
// OpVariable %_ptr_Function__arr.
//
// Unlike the Bayer NLM, neighbours here are ADJACENT pixels — there is no mosaic
// to step over, so the same-colour stride of 2 becomes 1 and the tile shrinks.
//
// Dispatch: one thread per 2x2 OUTPUT quad, grid = (out_w/2, out_h/2), 8x8 groups.

#ifndef NLM_HALF
#define NLM_HALF 0
#endif
#if NLM_HALF
typedef half  nlm_t;
typedef half3 nlm_t3;
#else
typedef float  nlm_t;
typedef float3 nlm_t3;
#endif

[[vk::binding(0, 0)]] RWStructuredBuffer<float> rgbBuf;    // binned linear RGB, 3 floats/px
[[vk::binding(1, 0)]] RWStructuredBuffer<uint>  outBuf;    // P010 (Y plane then CbCr)
[[vk::binding(2, 0)]] RWStructuredBuffer<uint>  chromaBuf; // CbCr scratch (median path)

struct PushConstants {
    // ENCODED (CTU-padded) picture size. Every bound, stride and offset below is
    // in this domain — see srcDim for why the two differ.
    uint2  outDim;
    // REAL picture size, i.e. what rgbBuf actually holds. outDim is rounded up to
    // the encoder's 64x64 CTU grid and the surplus columns/rows are filled by
    // load_rgb's clamp, which replicates the last real column and row. That is
    // deliberate: an unaligned picture makes the encoder code partial CTUs and
    // signal a conformance window, and an HEVC conformance window crops only from
    // the RIGHT and the BOTTOM — exactly where the streaked ~1 mm border artifact
    // appeared. Feeding it whole CTUs removes the partial blocks, and replicated
    // edge pixels are the standard extension: a motion vector pointing into the
    // pad then predicts from real picture content rather than invented data.
    // NOTHING IS CROPPED — the pad is added outside the sensor's full field.
    uint2  srcDim;
    uint   stridePixels;    // Y stride in pixels, in the PADDED domain
    uint   uvWordOffset;    // CbCr plane start, in uint32 words (padded domain)
    float  noiseK;          // shot-noise slope, already rescaled for the binned plane
    float  noiseFloor;      // read-noise variance floor, likewise rescaled
    float  hScale;          // filter strength, x the shot-noise sigma
    uint   flags;           // bit 0 = denoise on | bit 1 = divert chroma to scratch
    // Explicit, not incidental. The ten scalars above come to 40 bytes and a
    // float4 must start on a 16-byte boundary, so something has to fill the gap;
    // spelling it out here keeps the host struct (NlmRgbPush) a literal mirror
    // instead of relying on both compilers padding the same way.
    uint   _padA, _padB;
    float4 wb;              // xyz = R,G,B gains; w = pqScale
    float4 ccm0, ccm1, ccm2;// sensor -> BT.2020, row-major (xyz used)
};
[[vk::push_constant]] ConstantBuffer<PushConstants> push;

// Compile-time bounds — see the header comment. Must match kNlmSearchRadius /
// kNlmPatchRadius in raw_video_pipeline.hh.
// Guide weights. Mirrored on the host (see the noise-model derivation) — change
// one and you must change the other, or the denoise strength silently drifts.
static const float3 kLumaW = float3(0.25f, 0.65f, 0.10f);

static const int MAX_S = 1;                 // 8 candidates (3x3 search window)
static const int MAX_P = 1;                 // 3x3 patch
static const int REACH = MAX_S + MAX_P;     // 2 px (contiguous, not mosaic steps)

// A group covers 16x16 output pixels (8x8 threads, one 2x2 quad each).
static const int TILE_W = 16 + 2 * REACH;   // 20
static const int TILE_H = 16 + 2 * REACH;   // 20
static const int TILE_N = TILE_W * TILE_H;  // 400

// Luma drives the patch distance; RGB is what actually gets averaged. Both are
// FLAT SCALAR arrays: a groupshared array of a 3-vector invites the compiler to
// pad each element to 4 lanes (or to lay it out in a way the fill loop cannot
// coalesce), and nlm_bayer.slang's proven tile is scalar for the same reason.
groupshared nlm_t tileY[TILE_N];
groupshared nlm_t tileRgb[TILE_N * 3];

nlm_t3 tile_rgb(int i) {
    return nlm_t3(tileRgb[i * 3 + 0], tileRgb[i * 3 + 1], tileRgb[i * 3 + 2]);
}

// Clamped to the REAL picture, not the padded one. Inside the picture this is
// the ordinary edge clamp; past the right/bottom edge it is what replicates the
// border into the CTU pad. rgbBuf is only ever srcDim big, so this clamp is also
// what keeps the padded dispatch in bounds.
float3 load_rgb(int x, int y) {
    x = clamp(x, 0, int(push.srcDim.x) - 1);
    y = clamp(y, 0, int(push.srcDim.y) - 1);
    uint base = (uint(y) * push.srcDim.x + uint(x)) * 3u;
    return float3(rgbBuf[base + 0u], rgbBuf[base + 1u], rgbBuf[base + 2u]);
}

// Patch row from the tile. Contiguous in x — no mosaic step.
nlm_t3 patch_row(int c) {
    return nlm_t3(tileY[c - 1], tileY[c], tileY[c + 1]);
}

// Denoise the pixel staged at `base`. Weights come from the luma patch SSD and
// are shared by all three channels, which is what keeps this affordable.
float3 denoise_at(int base) {
    const int S = MAX_S;
    const int R = TILE_W;               // one row step in the tile
    const float npatch = 9.0f;          // (2*P+1)^2 with P == 1

    float centreY = float(tileY[base]);
    float sigma2  = max(push.noiseK * max(centreY, 0.0f) + push.noiseFloor, 1e-8f);
    float h2      = max(push.hScale * push.hScale * sigma2, 1e-8f);

    // Invariant across every candidate — held in registers, never re-read.
    nlm_t3 c0 = patch_row(base - R);
    nlm_t3 c1 = patch_row(base);
    nlm_t3 c2 = patch_row(base + R);

    float3 acc = float3(0.0f, 0.0f, 0.0f);
    float  wsum = 0.0f, wmax = 0.0f;
    [unroll] for (int dy = -S; dy <= S; ++dy) {
        [unroll] for (int dx = -S; dx <= S; ++dx) {
            if (dx == 0 && dy == 0) continue;   // centre handled after the loop

            int cbase = base + dy * R + dx;

            nlm_t3 d0 = c0 - patch_row(cbase - R);
            nlm_t3 d1 = c1 - patch_row(cbase);
            nlm_t3 d2 = c2 - patch_row(cbase + R);
            // Back to fp32 for the weight: exp() and the running sums are where
            // half's 11-bit mantissa would actually show.
            float ssd = float(dot(d0, d0) + dot(d1, d1) + dot(d2, d2)) / npatch;

            float w = exp(-ssd / h2);
            acc  += w * float3(tile_rgb(cbase));
            wsum += w;
            wmax  = max(wmax, w);
        }
    }

    // Give the centre sample the largest weight any candidate earned — weighting
    // it 1.0 would under-filter flat areas, dropping it would let an isolated
    // pixel be replaced outright. Same rule as nlm_bayer.slang.
    if (wmax <= 0.0f) wmax = 1.0f;
    acc  += wmax * float3(tile_rgb(base));
    wsum += wmax;

    return acc / max(wsum, 1e-8f);
}

// ── Develop tail: identical maths to debayer_isp.slang's develop(), minus the
// demosaic (there is none to do — every channel here is a real measurement). ──

// SMPTE ST 2084 (PQ) OETF. Input is display luminance normalized to 10 000 nits.
float3 pq_encode(float3 L) {
    const float m1 = 0.1593017578125f;   // 2610 / 16384
    const float m2 = 78.84375f;          // 2523 / 4096 * 128
    const float c1 = 0.8359375f;         //  107 / 128
    const float c2 = 18.8515625f;        // 2413 / 4096 * 32
    const float c3 = 18.6875f;           // 2392 / 4096 * 32
    float3 Lm = pow(saturate(L), m1.xxx);
    return pow((c1 + c2 * Lm) / (1.0f + c3 * Lm), m2.xxx);
}

float3 develop_rgb(float3 cam) {
    // Highlight reconstruction BEFORE white balance is allowed to tint what a
    // saturated photosite could not measure — see the long comment on
    // develop() in debayer_isp.slang; this is the same code and must stay in
    // lockstep with it and with cam::develop_linear_bayer.
    const float3 pre = cam;
    cam *= push.wb.xyz;
    {
        const float knee = 0.95f;
        const float peak = max(max(cam.r, cam.g), cam.b);
        const float3 t   = smoothstep(knee.xxx, 1.0f.xxx, pre);
        cam += (peak.xxx - cam) * t;
    }

    float3 lin;
    lin.r = dot(push.ccm0.xyz, cam);
    lin.g = dot(push.ccm1.xyz, cam);
    lin.b = dot(push.ccm2.xyz, cam);
    lin = max(lin, 0.0f.xxx);

    // pqScale maps linear 1.0 to the intended peak (0.10 -> 1000 nits).
    return pq_encode(lin * push.wb.w);
}

// BT.2020 non-constant luminance, applied to the PQ-encoded primaries.
float3 rgb_to_ycbcr(float3 c) {
    float Y  = 0.2627f * c.r + 0.6780f * c.g + 0.0593f * c.b;
    float Cb = (c.b - Y) / 1.8814f;
    float Cr = (c.r - Y) / 1.4746f;
    return float3(Y, Cb + 0.5f, Cr + 0.5f);
}

// 10-bit full range, packed into the high bits of a 16-bit word (P010).
uint to_p010(float v) {
    return uint(clamp(round(v * 1023.0f), 0.0f, 1023.0f)) << 6;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID,
          uint3 gid : SV_GroupID,
          uint  lin : SV_GroupIndex) {
    int tileX0 = int(gid.x) * 16 - REACH;
    int tileY0 = int(gid.y) * 16 - REACH;

    // Stage the group's footprint once: 400 samples across 64 threads. Luma is
    // derived here, so the SSD loops never touch the RGB triple.
    for (int i = int(lin); i < TILE_N; i += 64) {
        int ty = i / TILE_W;
        int tx = i - ty * TILE_W;
        float3 c = load_rgb(tileX0 + tx, tileY0 + ty);
        tileRgb[i * 3 + 0] = nlm_t(c.r);
        tileRgb[i * 3 + 1] = nlm_t(c.g);
        tileRgb[i * 3 + 2] = nlm_t(c.b);
        // Guide for the patch distance. The gains are applied HERE, not left to
        // develop_rgb: raw camera R and B sit at roughly half of G before white
        // balance, so an un-gained mix is overwhelmingly green and effectively
        // colour-blind. A boundary where the COLOUR changes but green does not
        // then reads as "same surface", the NLM averages both sides' R and B, and
        // colour bleeds across the edge — the one artifact class this pipeline
        // must never produce. Gaining first makes this a real luminance, so a
        // pure-chroma edge is visible to the weights and gets rejected.
        //
        // NOTE: kLumaW must stay in lockstep with the host's noise-model
        // derivation (raw_video_pipeline.cc fills noise_k/noise_floor from these
        // same weights and the same gains) — the denoiser is only correct while
        // the noise model describes the plane the SSD actually runs on.
        float3 gained = c * push.wb.xyz;
        tileY[i] = nlm_t(kLumaW.x * gained.r + kLumaW.y * gained.g + kLumaW.z * gained.b);
    }
    GroupMemoryBarrierWithGroupSync();

    uint qx = tid.x, qy = tid.y;
    if (qx >= push.outDim.x / 2u || qy >= push.outDim.y / 2u) return;

    const bool dn     = (push.flags & 1u) != 0u;
    // Hand the chroma to chroma_median.slang instead of writing it out here. The
    // luma stores below are identical either way, so enabling the median leaves
    // the Y plane byte-for-byte unchanged.
    const bool divert = (push.flags & 2u) != 0u;

    int x0 = int(qx) * 2, y0 = int(qy) * 2;
    int base = (y0 - tileY0) * TILE_W + (x0 - tileX0);

    // The four pixels of the quad, written out as four named locals rather than
    // a float3[4]. An array assigned through a loop variable is dynamically
    // indexed, and that is exactly what spills to scratch memory — the same trap
    // the centre patch avoids by living in three registers. Each denoise_at call
    // is inlined and its patch registers are dead at the next one, so the four
    // calls reuse the same registers rather than quadrupling them.
    float3 c00 = dn ? denoise_at(base)                  : float3(tile_rgb(base));
    float3 c10 = dn ? denoise_at(base + 1)              : float3(tile_rgb(base + 1));
    float3 c01 = dn ? denoise_at(base + TILE_W)         : float3(tile_rgb(base + TILE_W));
    float3 c11 = dn ? denoise_at(base + TILE_W + 1)     : float3(tile_rgb(base + TILE_W + 1));

    float3 y00 = rgb_to_ycbcr(develop_rgb(c00));
    float3 y10 = rgb_to_ycbcr(develop_rgb(c10));
    float3 y01 = rgb_to_ycbcr(develop_rgb(c01));
    float3 y11 = rgb_to_ycbcr(develop_rgb(c11));

    // Luma: two pixels per uint32 word, and x0 is even, so each row of the quad
    // is exactly one aligned store.
    uint halfStride = push.stridePixels >> 1;
    outBuf[uint(y0)     * halfStride + qx] = to_p010(y00.x) | (to_p010(y10.x) << 16);
    outBuf[uint(y0 + 1) * halfStride + qx] = to_p010(y01.x) | (to_p010(y11.x) << 16);

    // Chroma: 4:2:0, one co-sited pair per quad (box average of the four).
    float cb = 0.25f * (y00.y + y10.y + y01.y + y11.y);
    float cr = 0.25f * (y00.z + y10.z + y01.z + y11.z);
    uint packed = to_p010(cb) | (to_p010(cr) << 16);

    if (divert) chromaBuf[uint(qy) * (push.outDim.x >> 1) + qx] = packed;
    else        outBuf[push.uvWordOffset + uint(qy) * halfStride + qx] = packed;
}
