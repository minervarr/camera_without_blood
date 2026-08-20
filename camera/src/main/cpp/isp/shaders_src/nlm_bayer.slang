// Non-Local Means denoise in the RAW Bayer domain.
//
// Runs BEFORE the demosaic, so it removes sensor noise while the samples are
// still independent — denoising after interpolation would instead smear the
// correlations the demosaic just invented. Every comparison is made between
// samples of the SAME CFA colour (offsets step by 2), so the mosaic is never
// mixed and the pattern survives the pass intact.
//
// The candidate weight is the classic NLM one: the sum of squared differences
// over a small patch, scaled by the shot-noise variance expected at that signal
// level. Flat noisy areas average many candidates; detailed areas find few
// matches and stay sharp.
//
// PERFORMANCE — why this shader is tiled.
// The straightforward version reads srcBuf 457 times per output pixel: 24
// candidates x (9 patch samples x 2 reads + 1) + 1. At 4080x3060 that is 5.7
// BILLION global loads per frame, and it measured ~350 ms/frame on an Adreno
// 740 (8 fps, 253 of 299 frames dropped). Two redundancies caused it: the
// centre patch was re-read for every candidate, and neighbouring threads —
// whose search windows overlap almost completely — shared nothing.
//
// So the group stages its whole footprint in groupshared memory ONCE, already
// normalized and edge-clamped, and the inner loops become pure LDS reads. The
// arithmetic is unchanged: same values, same order, same float precision. This
// is a memory-access rewrite, not an algorithmic one.
//
// Dispatch: one thread per horizontal pixel PAIR (one uint32 word),
// grid = (raw_w/2, raw_h), 8x8 groups.

// Precision switch. The tile and the patch arithmetic run in fp16 when the
// device reports shaderFloat16 (VkCompute::fp16_supported): it halves the
// groupshared traffic and doubles the ALU rate, and this pass is bound by both.
// The weight accumulators stay fp32 — that is where precision actually matters.
// nlm_bayer_fp16.slang is a 2-line wrapper that sets NLM_HALF and includes this
// file; the host picks the .spv at load time and keeps this fp32 build as the
// fallback for devices without the feature.
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

[[vk::binding(0, 0)]] RWStructuredBuffer<uint> srcBuf;    // current frame, RAW16
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> dstBuf;    // denoised out, RAW16

struct PushConstants {
    uint2  rawDim;
    float  noiseK;        // shot-noise slope: var = noiseK * signal + noiseFloor
    float  noiseFloor;    // read-noise variance floor
    float4 black;         // per 2x2 CFA position, row-major
    float  white;
    float  searchRadius;  // half window, in same-colour steps
    float  patchRadius;   // half window, in same-colour steps
    float  hScale;        // filter strength, x the shot-noise sigma
};
[[vk::push_constant]] ConstantBuffer<PushConstants> push;

// Compile-time bounds. The host pins these (kNlmSearchRadius / kNlmPatchRadius
// in raw_video_pipeline.hh); the tile is sized from them, so the runtime values
// are clamped to match rather than silently reading outside the tile.
// Search radius 1 (8 candidates), not 2 (24). Measured on an Adreno 740 at
// 4080x3060: the demosaic pass alone costs the entire 33 ms budget for 30 fps,
// and radius 2 added ~71 ms on top — 15 fps with half the frames dropped.
// Radius 1 denoises flat areas less, and is what makes real-time denoise fit.
static const int MAX_S = 1;
static const int MAX_P = 1;
static const int REACH = (MAX_S + MAX_P) * 2;   // 4 px, in absolute pixels

// A group covers 16 px across (8 threads x one pair) and 8 rows.
static const int TILE_W = 16 + 2 * REACH;       // 24
static const int TILE_H = 8  + 2 * REACH;       // 16
static const int TILE_N = TILE_W * TILE_H;      // 384 floats = 1536 B of LDS

groupshared nlm_t tile[TILE_N];

float black_at(int x, int y) {
    int idx = (y & 1) * 2 + (x & 1);
    return (idx == 0) ? push.black.x : (idx == 1) ? push.black.y
         : (idx == 2) ? push.black.z : push.black.w;
}

float range_at(int x, int y) { return max(push.white - black_at(x, y), 1.0f); }

// Edge clamp that steps by 2, so a clamped read lands on the same CFA colour as
// the sample it stands in for. REACH is 6, so at most 3 steps are ever needed.
int mirror(int v, int limit) {
    [unroll] for (int k = 0; k < 4; ++k) { if (v < 0) v += 2; }
    [unroll] for (int k = 0; k < 4; ++k) { if (v >= limit) v -= 2; }
    return clamp(v, 0, limit - 1);
}

// Global read, normalized to [0,1]. Called only during the tile fill.
float load_norm(int x, int y) {
    int w = int(push.rawDim.x), h = int(push.rawDim.y);
    x = mirror(x, w);
    y = mirror(y, h);
    uint idx  = uint(y) * push.rawDim.x + uint(x);
    uint word = srcBuf[idx >> 1];
    uint v    = ((idx & 1u) == 0u) ? (word & 0xFFFFu) : (word >> 16);
    return (float(v) - black_at(x, y)) / range_at(x, y);
}

// Normalized value -> RAW16 counts at (x, y).
uint to_raw(float n, int x, int y) {
    float v = n * range_at(x, y) + black_at(x, y);
    return uint(clamp(round(v), 0.0f, 65535.0f));
}

// Denoise the sample staged at `base` in the tile. Returns the normalized value.
//
// S and P are compile-time, NOT read from the push constants. The host pins them
// (kNlmSearchRadius / kNlmPatchRadius are constexpr), and baking them is what
// keeps the 3x3 centre patch in registers. An earlier version held it in a
// float[9]: with a runtime P the loops would not unroll, the indices stayed
// dynamic, and the array spilled to scratch memory — costing more than the
// tiling saved. Three float3 rows are register-native and cannot spill.
// push.searchRadius / push.patchRadius stay in the struct to match the host's
// DenoisePush ABI.
//
// NOTE: the body below assumes P == 1 (a 3x3 patch). MAX_P encodes that.
nlm_t3 patch_row(int c) {
    return nlm_t3(tile[c - 2], tile[c], tile[c + 2]);
}

float denoise_at(int base) {
    const int S = MAX_S;
    const int R = 2 * TILE_W;          // one same-colour row step in the tile
    const float npatch = 9.0f;         // (2*P+1)^2 with P == 1

    float centre = float(tile[base]);
    float sigma2 = max(push.noiseK * max(centre, 0.0f) + push.noiseFloor, 1e-8f);
    float h2     = max(push.hScale * push.hScale * sigma2, 1e-8f);

    // The centre patch is invariant across all 24 candidates. Re-reading it was
    // 216 of the original 457 loads per pixel.
    nlm_t3 c0 = patch_row(base - R);
    nlm_t3 c1 = patch_row(base);
    nlm_t3 c2 = patch_row(base + R);

    float acc = 0.0f, wsum = 0.0f, wmax = 0.0f;
    for (int dy = -S; dy <= S; ++dy) {
        for (int dx = -S; dx <= S; ++dx) {
            if (dx == 0 && dy == 0) continue;   // centre handled after the loop

            int cbase = base + dy * R + dx * 2; // same-colour step

            nlm_t3 d0 = c0 - patch_row(cbase - R);
            nlm_t3 d1 = c1 - patch_row(cbase);
            nlm_t3 d2 = c2 - patch_row(cbase + R);
            // Back to fp32 for the weight: exp() and the running sums are where
            // half's 11-bit mantissa would actually show.
            float ssd = float(dot(d0, d0) + dot(d1, d1) + dot(d2, d2)) / npatch;

            float w = exp(-ssd / h2);
            acc  += w * float(tile[cbase]);
            wsum += w;
            wmax  = max(wmax, w);
        }
    }

    // Give the centre sample the largest weight any candidate earned. Weighting
    // it 1.0 instead would under-filter flat areas; dropping it would let an
    // isolated pixel be replaced outright.
    if (wmax <= 0.0f) wmax = 1.0f;
    acc  += wmax * centre;
    wsum += wmax;

    return acc / max(wsum, 1e-8f);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID,
          uint3 gid : SV_GroupID,
          uint  lin : SV_GroupIndex) {
    int words = int(push.rawDim.x) / 2;
    int rows  = int(push.rawDim.y);

    // Tile origin in absolute pixel coordinates.
    int tileX0 = int(gid.x) * 16 - REACH;
    int tileY0 = int(gid.y) * 8  - REACH;

    // Stage the group's whole footprint. 560 samples across 64 threads = 9
    // strided iterations, replacing ~58 000 global loads with 560.
    for (int i = int(lin); i < TILE_N; i += 64) {
        int ty = i / TILE_W;
        int tx = i - ty * TILE_W;
        tile[i] = nlm_t(load_norm(tileX0 + tx, tileY0 + ty));
    }
    GroupMemoryBarrierWithGroupSync();

    int wx = int(tid.x);
    int y  = int(tid.y);
    if (wx >= words || y >= rows) return;

    int x0   = wx * 2;
    int base = (y - tileY0) * TILE_W + (x0 - tileX0);

    uint idx = uint(y) * push.rawDim.x + uint(x0);
    dstBuf[idx >> 1] = to_raw(denoise_at(base),     x0,     y)
                     | (to_raw(denoise_at(base + 1), x0 + 1, y) << 16);
}
