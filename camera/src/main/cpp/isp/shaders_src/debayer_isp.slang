// Main ISP pass: Bayer -> BT.2020 PQ -> P010.
//
// Per 2x2 output quad this reconstructs RGB for four pixels, applies white
// balance and the sensor->BT.2020 colour matrix, encodes ST 2084 (PQ), converts
// to non-constant-luminance Y'CbCr, and packs the result as 10-bit-in-16
// (P010, value left-shifted 6) straight into the encoder's input buffer.
//
// Two demosaic modes, selected by push.cfa bit 8:
//   0 = Malvar-He-Cutler, a single-pass 5x5 gradient-corrected linear filter.
//   1 = HQ directional: green comes from the RCD-style prepass (green_isp.slang)
//       and R/B are interpolated in COLOUR-DIFFERENCE space (R-G, B-G), which is
//       far smoother across edges than interpolating R and B directly.
//
// push.cfa bit 9 diverts the chroma into the scratch buffer for the separate
// chroma-denoise pass; the luma plane is written identically either way, so
// enabling CD leaves luma byte-for-byte unchanged.
//
// Dispatch: one thread per 2x2 quad, grid = (out_w/2, out_h/2), 8x8 groups.

[[vk::binding(0, 0)]] RWStructuredBuffer<uint>  rawBuf;     // RAW16, 2 px per uint32
[[vk::binding(1, 0)]] RWStructuredBuffer<uint>  outBuf;     // P010 (Y plane then CbCr)
[[vk::binding(2, 0)]] RWStructuredBuffer<float> greenBuf;   // full-res green (HQ only)
[[vk::binding(3, 0)]] RWStructuredBuffer<uint>  chromaBuf;  // noisy CbCr scratch (CD only)

struct PushConstants {
    uint   outW, outH;
    uint   stridePixels;    // Y stride in pixels
    uint   uvWordOffset;    // CbCr plane start, in uint32 words
    uint   rawW, rawH;
    uint   cfa;             // low 2 bits pattern | bit 8 = HQ | bit 9 = chroma divert
    float  white;
    float4 black;           // per 2x2 CFA position, row-major
    float4 wb;              // xyz = R,G,B gains; w = pqScale
    float4 ccm0, ccm1, ccm2;// sensor -> BT.2020, row-major (xyz used)
};
[[vk::push_constant]] ConstantBuffer<PushConstants> push;

float black_at(int x, int y) {
    int idx = (y & 1) * 2 + (x & 1);
    return (idx == 0) ? push.black.x : (idx == 1) ? push.black.y
         : (idx == 2) ? push.black.z : push.black.w;
}

// ── Bayer staging tile ───────────────────────────────────────────────────────
// Each thread develops a 2x2 quad, so a group covers 16x16 pixels; Malvar's 5x5
// kernel reaches +/-2 around each, giving a 20x20 footprint. Read straight from
// the buffer that is 64 threads x 52 samples = 3328 global loads per group, over
// an area of only 400 distinct values — neighbouring quads re-read almost
// everything. Staging it once in groupshared cuts that ~8x. Same values, same
// arithmetic; this is a memory-access change only. (The same rewrite took the
// NLM pass from ~350 ms to ~71 ms.)
static const int ISP_HALO   = 2;
static const int ISP_TILE_W = 16 + 2 * ISP_HALO;   // 20
static const int ISP_TILE_H = 16 + 2 * ISP_HALO;   // 20
static const int ISP_TILE_N = ISP_TILE_W * ISP_TILE_H;

groupshared float rawTile[ISP_TILE_N];
groupshared int   rawTileX0;
groupshared int   rawTileY0;

// Global read, normalized exactly as the untiled version did. Fill only.
float load_raw_norm(int x, int y) {
    x = clamp(x, 0, int(push.rawW) - 1);
    y = clamp(y, 0, int(push.rawH) - 1);
    uint idx  = uint(y) * push.rawW + uint(x);
    uint word = rawBuf[idx >> 1];
    uint v    = ((idx & 1u) == 0u) ? (word & 0xFFFFu) : (word >> 16);
    float b   = black_at(x, y);
    return saturate((float(v) - b) / max(push.white - b, 1.0f));
}

// Tile lookup. Every caller stays within the halo by construction; the clamp is
// the same edge behaviour the untiled version had.
float sample_raw(int x, int y) {
    int tx = clamp(x - rawTileX0, 0, ISP_TILE_W - 1);
    int ty = clamp(y - rawTileY0, 0, ISP_TILE_H - 1);
    return rawTile[ty * ISP_TILE_W + tx];
}

float sample_green(int x, int y) {
    x = clamp(x, 0, int(push.rawW) - 1);
    y = clamp(y, 0, int(push.rawH) - 1);
    return greenBuf[uint(y) * push.rawW + uint(x)];
}

// 0 = red, 1 = green, 2 = blue.
int color_at(int x, int y) {
    uint p = push.cfa & 3u;
    int rx = int((p == 0u || p == 2u) ? 0u : 1u);
    int ry = int((p == 0u || p == 1u) ? 0u : 1u);
    int dx = (x & 1) ^ rx;
    int dy = (y & 1) ^ ry;
    if (dx == 0 && dy == 0) return 0;
    if (dx == 1 && dy == 1) return 2;
    return 1;
}

// ── Malvar-He-Cutler (ICASSP 2004) ──────────────────────────────────────────
// Gradient-corrected bilinear interpolation: the missing channel is the bilinear
// estimate plus a fraction of the Laplacian of the channel that IS sampled here.
// All kernels are the published 5x5 ones, scaled by 1/8.
float3 demosaic_malvar(int x, int y) {
    float c = sample_raw(x, y);

    // Neighbourhood taps shared by the kernels.
    float n1 = sample_raw(x,     y - 1), s1 = sample_raw(x,     y + 1);
    float w1 = sample_raw(x - 1, y    ), e1 = sample_raw(x + 1, y    );
    float n2 = sample_raw(x,     y - 2), s2 = sample_raw(x,     y + 2);
    float w2 = sample_raw(x - 2, y    ), e2 = sample_raw(x + 2, y    );
    float nw = sample_raw(x - 1, y - 1), ne = sample_raw(x + 1, y - 1);
    float sw = sample_raw(x - 1, y + 1), se = sample_raw(x + 1, y + 1);

    int col = color_at(x, y);

    // Green at a red or blue site.
    float g_at_rb = (4.0f * c + 2.0f * (n1 + s1 + w1 + e1) - (n2 + s2 + w2 + e2)) / 8.0f;

    // Red/blue at a green site, for the two green orientations.
    // "h" = the missing channel sits to the left/right; "v" = above/below.
    // Kernel weights sum to 8 in every case, so the /8 keeps unity gain.
    float rb_h = (5.0f * c + 4.0f * (w1 + e1) - (nw + ne + sw + se)
                  - (w2 + e2) + 0.5f * (n2 + s2)) / 8.0f;
    float rb_v = (5.0f * c + 4.0f * (n1 + s1) - (nw + ne + sw + se)
                  - (n2 + s2) + 0.5f * (w2 + e2)) / 8.0f;

    // Red at a blue site (and blue at a red site): diagonal neighbours.
    float rb_diag = (6.0f * c + 2.0f * (nw + ne + sw + se)
                     - 1.5f * (n2 + s2 + w2 + e2)) / 8.0f;

    float3 rgb;
    if (col == 1) {
        // Green site. Which of R/B is horizontal depends on the row's colour.
        rgb.g = c;
        // The horizontal neighbour of this green pixel: sample its colour.
        int horiz_col = color_at(x + 1, y);
        if (horiz_col == 0) { rgb.r = rb_h; rgb.b = rb_v; }
        else                { rgb.r = rb_v; rgb.b = rb_h; }
    } else if (col == 0) {
        rgb.r = c;
        rgb.g = g_at_rb;
        rgb.b = rb_diag;
    } else {
        rgb.b = c;
        rgb.g = g_at_rb;
        rgb.r = rb_diag;
    }
    return max(rgb, 0.0f.xxx);
}

// ── HQ directional ──────────────────────────────────────────────────────────
// Green is already reconstructed everywhere by green_isp.slang. R and B are then
// interpolated as differences against green, which is nearly flat inside an
// object even where luminance is not — so averaging it does not smear edges.
float3 demosaic_hq(int x, int y) {
    float g = sample_green(x, y);
    int col = color_at(x, y);

    float3 rgb;
    rgb.g = g;

    if (col == 0 || col == 2) {
        // Known channel here; the OTHER chroma channel sits on the 4 diagonals.
        float known = sample_raw(x, y);
        float d = 0.25f * ((sample_raw(x - 1, y - 1) - sample_green(x - 1, y - 1))
                         + (sample_raw(x + 1, y - 1) - sample_green(x + 1, y - 1))
                         + (sample_raw(x - 1, y + 1) - sample_green(x - 1, y + 1))
                         + (sample_raw(x + 1, y + 1) - sample_green(x + 1, y + 1)));
        float other = g + d;
        if (col == 0) { rgb.r = known; rgb.b = other; }
        else          { rgb.b = known; rgb.r = other; }
    } else {
        // Green site: one chroma channel lies horizontally, the other vertically.
        float dh = 0.5f * ((sample_raw(x - 1, y) - sample_green(x - 1, y))
                         + (sample_raw(x + 1, y) - sample_green(x + 1, y)));
        float dv = 0.5f * ((sample_raw(x, y - 1) - sample_green(x, y - 1))
                         + (sample_raw(x, y + 1) - sample_green(x, y + 1)));
        if (color_at(x + 1, y) == 0) { rgb.r = g + dh; rgb.b = g + dv; }
        else                          { rgb.r = g + dv; rgb.b = g + dh; }
    }
    return max(rgb, 0.0f.xxx);
}

// ── SMPTE ST 2084 (PQ) inverse EOTF ─────────────────────────────────────────
// Input is display luminance normalized so 1.0 == 10000 cd/m2.
float3 pq_encode(float3 L) {
    const float m1 = 0.1593017578125f;   // 2610 / 16384
    const float m2 = 78.84375f;          // 2523 / 4096 * 128
    const float c1 = 0.8359375f;         // 3424 / 4096
    const float c2 = 18.8515625f;        // 2413 / 4096 * 32
    const float c3 = 18.6875f;           // 2392 / 4096 * 32
    float3 Lm = pow(saturate(L), m1.xxx);
    return pow((c1 + c2 * Lm) / (1.0f + c3 * Lm), m2.xxx);
}

// Bayer pixel -> PQ-encoded BT.2020 R'G'B'.
float3 develop(int x, int y, bool hq) {
    float3 cam = hq ? demosaic_hq(x, y) : demosaic_malvar(x, y);

    // Highlight reconstruction, BEFORE white balance is allowed to tint what a
    // saturated photosite could not measure. read_raw() normalises to 1.0 at the
    // sensor's white level, so a channel at ~1.0 is pinned and carries no
    // information beyond "at least this bright". White-balancing it anyway tints
    // every blown highlight by whichever channel clipped first — green saturates
    // earliest on these sensors and red/blue carry ~1.9x gains, which turns
    // strong light green at the fringes and magenta in the core.
    //
    // Each clipped channel is raised to the brightest balanced channel, ramped in
    // over the last 5% before the clip so there is no seam. Unclipped channels are
    // untouched, so a genuinely saturated colour keeps its hue; channels only ever
    // rise, so nothing darkens. Kept in lockstep with cam::develop_linear_bayer in
    // camera/raw_develop.cc so a still and a clip of the same scene still agree.
    const float3 pre = cam;
    cam *= push.wb.xyz;
    {
        const float knee = 0.95f;
        const float peak = max(max(cam.r, cam.g), cam.b);
        const float3 t   = smoothstep(knee.xxx, 1.0f.xxx, pre);
        cam += (peak.xxx - cam) * t;
    }

    // Sensor primaries -> BT.2020 linear.
    float3 lin;
    lin.r = dot(push.ccm0.xyz, cam);
    lin.g = dot(push.ccm1.xyz, cam);
    lin.b = dot(push.ccm2.xyz, cam);
    lin = max(lin, 0.0f.xxx);

    // pqScale maps linear 1.0 to the intended peak (0.10 -> 1000 nits).
    return pq_encode(lin * push.wb.w);
}

// BT.2020 non-constant luminance. The matrix is applied to the PQ-encoded
// (non-linear) primaries, which is what "bt2020nc" means.
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
    if (lin == 0) {
        rawTileX0 = int(gid.x) * 16 - ISP_HALO;
        rawTileY0 = int(gid.y) * 16 - ISP_HALO;
    }
    GroupMemoryBarrierWithGroupSync();

    const int tx0 = rawTileX0, ty0 = rawTileY0;
    for (int i = int(lin); i < ISP_TILE_N; i += 64) {
        int ty = i / ISP_TILE_W;
        int tx = i - ty * ISP_TILE_W;
        rawTile[i] = load_raw_norm(tx0 + tx, ty0 + ty);
    }
    GroupMemoryBarrierWithGroupSync();

    uint qx = tid.x, qy = tid.y;
    if (qx >= push.outW / 2u || qy >= push.outH / 2u) return;

    const bool hq    = (push.cfa & (1u << 8)) != 0u;
    const bool divert = (push.cfa & (1u << 9)) != 0u;

    int x0 = int(qx) * 2, y0 = int(qy) * 2;

    float3 c00 = rgb_to_ycbcr(develop(x0,     y0,     hq));
    float3 c10 = rgb_to_ycbcr(develop(x0 + 1, y0,     hq));
    float3 c01 = rgb_to_ycbcr(develop(x0,     y0 + 1, hq));
    float3 c11 = rgb_to_ycbcr(develop(x0 + 1, y0 + 1, hq));

    // Luma: two pixels per uint32 word, and the quad's x0 is even, so each row
    // of the quad is exactly one aligned store.
    uint halfStride = push.stridePixels >> 1;
    uint row0 = uint(y0)     * halfStride + qx;
    uint row1 = uint(y0 + 1) * halfStride + qx;
    outBuf[row0] = to_p010(c00.x) | (to_p010(c10.x) << 16);
    outBuf[row1] = to_p010(c01.x) | (to_p010(c11.x) << 16);

    // Chroma: 4:2:0, one co-sited pair per quad (box average of the four).
    float cb = 0.25f * (c00.y + c10.y + c01.y + c11.y);
    float cr = 0.25f * (c00.z + c10.z + c01.z + c11.z);
    uint packed = to_p010(cb) | (to_p010(cr) << 16);

    if (divert) {
        // Hand the noisy chroma to chroma_denoise.slang, which writes the final
        // CbCr into outBuf. Luma above is already final and is never touched again.
        chromaBuf[qy * (push.outW >> 1) + qx] = packed;
    } else {
        outBuf[push.uvWordOffset + qy * halfStride + qx] = packed;
    }
}
