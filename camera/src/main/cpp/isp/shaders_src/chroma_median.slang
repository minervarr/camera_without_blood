// 9-tap median on the chroma plane, at a selectable sample SPACING. Removes
// isolated colour speckle, and — at spacing 2 — clustered speckle too.
//
// WHY A MEDIAN AND NOT A BLUR (this is the whole point of the pass).
// A weighted average — a bilateral, a gaussian, an NLM — computes a NEW value by
// mixing neighbours together. Near a high-contrast edge that mixture contains
// colour from both sides, which is exactly how a denoise produces a coloured halo
// or "aura" hugging an edge. A median computes nothing: it SELECTS one of the
// nine values that is already there. It therefore cannot invent a colour that was
// not present locally, and it cannot bleed one side of an edge into the other.
// That makes it artifact-free in the way that matters here, and it happens to be
// the classic optimal filter for exactly the symptom this targets — isolated
// outliers (salt-and-pepper), i.e. random single-site colour dots in the shadows.
//
// What it deliberately does NOT do:
//   * It never reads another frame. There is no temporal term anywhere in this
//     pass, so it physically cannot leave a trail or ghost behind motion.
//   * It never grows the number of TAPS. Always exactly 9 samples and the same
//     19 compare-exchanges; `dilation` moves where those 9 sit, it does not add
//     any. The historic frame-rate collapses in this pipeline all came from
//     adding candidates (NLM radius 2: 8 candidates -> 24, 8 fps, 253/299 frames
//     dropped). Nothing here does that.
//   * It never touches luma. The Y plane of outBuf is not written here, so it is
//     byte-for-byte identical to the no-median path and no detail, sharpness or
//     texture can be affected.
//
// Run THREE times, ping-ponging the two scratch buffers:
//
//   1. chroma_buf  -> chroma_buf2   dilation 1   isolated single-site dots
//   2. chroma_buf2 -> chroma_buf    dilation 2   CLUSTERS (see below)
//   3. chroma_buf  -> P010 CbCr     dilation 1   cleanup
//
// Repeated median is a standard idiom: each pass drives the signal closer to its
// "root" while remaining a median throughout — so the no-halo guarantee holds no
// matter how often it runs, unlike widening a blur.
//
// WHY THE MIDDLE PASS IS DILATED. What survives two adjacent-neighbour passes is
// CLUSTERED speckle: a 2x2 or 3-site colour blob is the MAJORITY inside a 3x3
// window centred on it, so the median dutifully keeps it — repeating the same
// window cannot help, because the blob is a root of that filter. Sampling at
// spacing 2 puts the whole ring outside a blob that size, so the blob is
// outvoted by real neighbours and loses. It costs nothing extra in compares;
// only the staged tile grows (12x12 instead of 10x10).
//
// Pass 3 exists because a dilated pass selects from two sites away, which can
// leave a value that is a mild outlier against its immediate neighbours; a final
// spacing-1 median resolves that. Every pass is still a selection from values
// already present, so no pass can invent a colour or bleed across an edge.
//
// Three 9-tap passes remain far cheaper than one 5x5 (3x19 compare-exchanges
// versus ~99) and reject clustered speckle better than any of them.
//
// Cost: one 9-tap median per channel on a 4:2:0 chroma plane — at 2040x1530
// output that is 1020x765 sites, a sixteenth of the sensor's pixel count. The
// median network below is 19 compare-exchanges, branch-free.
//
// Cb and Cr are medianed independently. In principle that can pair a Cb from one
// neighbour with a Cr from another and yield a combination present in neither;
// with near-neutral shadow chroma the difference from a true vector median is far
// below a quantisation step, and a vector median costs several times as much.
//
// Dispatch: one thread per chroma site, grid = (out_w/2, out_h/2), 8x8 groups.

// Generic src -> dst so the SAME pipeline serves all three passes. Both the
// scratch buffers and the P010 CbCr plane have stride outW/2, so the
// destinations differ only by a base offset (0 for a scratch, the CbCr plane
// start for the final write) — hence dstOffset rather than separate shaders.
[[vk::binding(0, 0)]] RWStructuredBuffer<uint> dstBuf;   // scratch, or the P010 surface
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> srcBuf;   // CbCr to filter

struct PushConstants {
    uint outW, outH;
    // SEPARATE strides. They happen to be equal today (the ISP writes a tight
    // P010 surface, so its chroma stride is outW/2, the same as a scratch), but
    // submit_to_encoder already handles an encoder stride != outW*2. One shared
    // stride would silently write the CbCr plane at the wrong pitch the moment
    // that changes, shredding chroma on pass 2 while pass 1 stayed correct.
    uint srcStride;
    uint dstStride;
    uint dstOffset;       // 0 for a scratch target, uvWordOffset for the P010 plane
    // Sample SPACING, not radius: still 9 taps, just further apart. 1 or 2 only
    // (the staged tile carries a halo of exactly CM_HALO = 2).
    uint dilation;
    uint _pad0, _pad1;
};
[[vk::push_constant]] ConstantBuffer<PushConstants> push;

// Packed (Cb,Cr) at a chroma site, edge-clamped. Values stay in the P010 domain
// (10 bits left-shifted 6); the median only ever compares and copies them, so
// there is no need to normalise and no rounding anywhere in this pass.
uint site(int cx, int cy) {
    int w = int(push.outW >> 1), h = int(push.outH >> 1);
    cx = clamp(cx, 0, w - 1);
    cy = clamp(cy, 0, h - 1);
    return srcBuf[uint(cy) * push.srcStride + uint(cx)];
}

// Branch-free median of 9, the standard 19 compare-exchange network. Operates on
// both channels at once: Cb in the low half of each uint, Cr in the high half, so
// a uint2 carries the pair and each swap is two scalar min/max.
void cswap(inout uint2 a, inout uint2 b) {
    uint2 t = min(a, b);
    b = max(a, b);
    a = t;
}

uint median9(uint2 p0, uint2 p1, uint2 p2,
             uint2 p3, uint2 p4, uint2 p5,
             uint2 p6, uint2 p7, uint2 p8) {
    cswap(p1, p2); cswap(p4, p5); cswap(p7, p8);
    cswap(p0, p1); cswap(p3, p4); cswap(p6, p7);
    cswap(p1, p2); cswap(p4, p5); cswap(p7, p8);
    cswap(p0, p3); cswap(p5, p8); cswap(p4, p7);
    cswap(p3, p6); cswap(p1, p4); cswap(p2, p5);
    cswap(p4, p7); cswap(p4, p2); cswap(p6, p4);
    cswap(p4, p2);
    // p4.x is the median Cb, p4.y the median Cr — both already P010-packed.
    return (p4.x & 0xFFFFu) | (p4.y << 16);
}

uint2 split(uint packed) { return uint2(packed & 0xFFFFu, packed >> 16); }

// Groupshared staging, for the same reason nlm_bayer.slang and debayer_isp.slang
// stage: an 8x8 group's windows span only a small footprint, but reading them
// per-thread costs 64 x 9 = 576 global loads — and this pass now runs THREE times
// per frame. Staging once makes the median network pure LDS reads. Same values,
// same result; a memory-access change only.
//
// The halo is 2, not 1, so ONE tile serves both dilations and one shader serves
// all three passes: 144 staged samples across 64 threads instead of 100. Still
// four times fewer than the 576 global loads the untiled version cost, and the
// spacing-1 passes read a strict subset of the same tile — bit-identical to what
// a 10x10 tile would have given them.
static const int CM_HALO   = 2;
static const int CM_TILE_W = 8 + 2 * CM_HALO;   // 12
static const int CM_TILE_H = 8 + 2 * CM_HALO;   // 12
static const int CM_TILE_N = CM_TILE_W * CM_TILE_H;

groupshared uint2 tile[CM_TILE_N];

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID,
          uint3 gid : SV_GroupID,
          uint3 ltid : SV_GroupThreadID,
          uint  lin : SV_GroupIndex) {
    const int tx0 = int(gid.x) * 8 - CM_HALO;
    const int ty0 = int(gid.y) * 8 - CM_HALO;

    // 144 samples across 64 threads = 3 strided iterations. site() already
    // edge-clamps, so the halo is filled exactly as the untiled version read it.
    for (int i = int(lin); i < CM_TILE_N; i += 64) {
        int ty = i / CM_TILE_W;
        int tx = i - ty * CM_TILE_W;
        tile[i] = split(site(tx0 + tx, ty0 + ty));
    }
    GroupMemoryBarrierWithGroupSync();

    uint cx = tid.x, cy = tid.y;
    if (cx >= (push.outW >> 1) || cy >= (push.outH >> 1)) return;

    // Centre of this thread's window within the tile. Uses SV_GroupThreadID rather
    // than tid % 8: dispatch_banded issues these passes via vkCmdDispatchBase, so
    // gid/tid carry a band offset and only the group-local id is band-invariant.
    const int b = (int(ltid.y) + CM_HALO) * CM_TILE_W + (int(ltid.x) + CM_HALO);
    const int R = CM_TILE_W;

    // Step between taps, in tile elements. dilation is 1 or 2 and the halo is 2,
    // so b +/- dr and b +/- d are always inside the tile for every thread.
    const int d  = int(clamp(push.dilation, 1u, uint(CM_HALO)));
    const int dr = R * d;

    uint m = median9(tile[b - dr - d], tile[b - dr], tile[b - dr + d],
                     tile[b      - d], tile[b     ], tile[b      + d],
                     tile[b + dr - d], tile[b + dr], tile[b + dr + d]);

    dstBuf[push.dstOffset + cy * push.dstStride + cx] = m;
}
