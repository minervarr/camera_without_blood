// Green prepass for the HQ (directional) demosaic.
//
// Builds a full-resolution green plane from the Bayer mosaic. Green carries most
// of the luminance detail, so reconstructing it well first — and letting the ISP
// pass derive R/B from colour DIFFERENCES against it — is what separates the HQ
// path from the single-pass Malvar path.
//
// The interpolation is Hamilton-Adams / RCD style: at a red or blue site, green
// is estimated separately along the horizontal and the vertical axis, each
// estimate corrected by the second derivative of the KNOWN channel at that site
// (which removes most of the zipper artefact a plain average leaves), then the
// two are blended by inverse gradient weight so edges pick the direction that
// runs ALONG them rather than across them.
//
// Dispatch: one thread per pixel, grid = (raw_w, raw_h), 8x8 groups.

[[vk::binding(0, 0)]] RWStructuredBuffer<uint>  rawBuf;    // RAW16, 2 px per uint32
[[vk::binding(1, 0)]] RWStructuredBuffer<float> greenBuf;  // full-res green, 1 float per px

struct PushConstants {
    uint2  rawDim;      // raw_w, raw_h
    uint   cfa;         // 0=RGGB 1=GRBG 2=GBRG 3=BGGR (low 2 bits)
    float  white;       // sensor white level (raw counts)
    float4 black;       // black level per 2x2 CFA position, row-major
};
[[vk::push_constant]] ConstantBuffer<PushConstants> push;

// Black level for the CFA position of (x, y).
float black_at(int x, int y) {
    int idx = (y & 1) * 2 + (x & 1);
    return (idx == 0) ? push.black.x : (idx == 1) ? push.black.y
         : (idx == 2) ? push.black.z : push.black.w;
}

// Normalized [0,1] sensor value with the per-position black subtracted. Reads
// are edge-clamped, so the border reconstructs from mirrored-in data instead of
// reading out of bounds.
float sample_raw(int x, int y) {
    x = clamp(x, 0, int(push.rawDim.x) - 1);
    y = clamp(y, 0, int(push.rawDim.y) - 1);
    uint idx  = uint(y) * push.rawDim.x + uint(x);
    uint word = rawBuf[idx >> 1];
    uint v    = ((idx & 1u) == 0u) ? (word & 0xFFFFu) : (word >> 16);
    float b   = black_at(x, y);
    return saturate((float(v) - b) / max(push.white - b, 1.0f));
}

// Colour of the CFA site (x, y): 0 = red, 1 = green, 2 = blue.
// The CFA code names the 2x2 pattern top-left-first, so it is exactly a parity
// lookup once the pattern's own phase is folded in.
int color_at(int x, int y) {
    uint p  = push.cfa & 3u;
    // Phase of the pattern: which (x,y) parity holds red.
    int rx = int((p == 0u || p == 2u) ? 0u : 1u);
    int ry = int((p == 0u || p == 1u) ? 0u : 1u);
    int dx = (x & 1) ^ rx;
    int dy = (y & 1) ^ ry;
    if (dx == 0 && dy == 0) return 0;   // red
    if (dx == 1 && dy == 1) return 2;   // blue
    return 1;                            // green
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    int x = int(tid.x);
    int y = int(tid.y);
    if (uint(x) >= push.rawDim.x || uint(y) >= push.rawDim.y) return;

    float g;
    if (color_at(x, y) == 1) {
        g = sample_raw(x, y);            // green site: nothing to interpolate
    } else {
        // Known channel (R or B) at this site and two steps out, used for the
        // Laplacian correction term on each axis.
        float c0 = sample_raw(x, y);
        float cL = sample_raw(x - 2, y);
        float cR = sample_raw(x + 2, y);
        float cU = sample_raw(x, y - 2);
        float cD = sample_raw(x, y + 2);

        float gL = sample_raw(x - 1, y);
        float gR = sample_raw(x + 1, y);
        float gU = sample_raw(x, y - 1);
        float gD = sample_raw(x, y + 1);

        // Directional estimates: neighbour average + 1/4 of the known channel's
        // second derivative along that axis.
        float gH = 0.5f * (gL + gR) + 0.25f * (2.0f * c0 - cL - cR);
        float gV = 0.5f * (gU + gD) + 0.25f * (2.0f * c0 - cU - cD);

        // Gradients: how much the signal changes ACROSS each axis. A large
        // horizontal gradient means the horizontal estimate crosses an edge, so
        // it must be trusted less.
        float dH = abs(gL - gR) + abs(2.0f * c0 - cL - cR);
        float dV = abs(gU - gD) + abs(2.0f * c0 - cU - cD);

        // Inverse-square weighting: smooth areas (dH ~ dV) blend evenly, strong
        // edges collapse onto the direction along the edge.
        float wH = 1.0f / (1.0f + dH * dH * 256.0f);
        float wV = 1.0f / (1.0f + dV * dV * 256.0f);
        g = (wH * gH + wV * gV) / (wH + wV);
    }

    greenBuf[uint(y) * push.rawDim.x + uint(x)] = saturate(g);
}
