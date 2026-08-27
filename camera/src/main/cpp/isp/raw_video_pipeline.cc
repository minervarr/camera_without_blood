#include "raw_video_pipeline.hh"

#include <media/NdkMediaFormat.h>

#include <dlfcn.h>
#include <time.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <thread>

#include "../cpu_affinity.hh"
#include "../logger.hh"

#undef LOG_TAG
#define LOG_TAG "RawVideo"

namespace isp {

namespace {

// MediaFormat integer values (stable public API constants, set via string keys
// so we don't depend on NDK header availability at minSdk).
constexpr int32_t kColorFormatYUVP010 = 54;   // COLOR_FormatYUVP010
constexpr int32_t kHevcProfileMain10  = 2;    // HEVCProfileMain10
constexpr int32_t kColorStandardBt2020 = 6;   // COLOR_STANDARD_BT2020
constexpr int32_t kColorTransferSt2084 = 6;   // COLOR_TRANSFER_ST2084
constexpr int32_t kColorRangeFull      = 1;   // COLOR_RANGE_FULL
constexpr int32_t kBitrateModeVbr      = 1;   // BITRATE_MODE_VBR
// Bitrate from a perceptual bpp target on the real geometry. Storage is a
// non-issue here and the encoder sits at ~0.1 ms enc-wait even at high rates
// (Phase-1 pipelining unblocked throughput), so we push for maximum quality:
// ~0.8 bpp ≈ 300 Mbps at 4080x3060p30. That exceeds the HEVC L6.0 High-tier
// ceiling (240 Mbps) so the encoder negotiates L6.1 (480 Mbps ceiling), which
// Adreno 740 + modern players handle fine. The cap stays under L6.1 High tier.
constexpr double  kTargetBpp           = 0.80;
// Binned (half-resolution) path. At 0.80 bpp a 2040x1530p30 stream would be only
// ~75 Mbps — a quarter of what the full-res path already sustains, against an
// encoder ceiling it was nowhere near. Spending those bits back is the cheapest
// quality available here: ~2.6 bpp is close to visually lossless for HEVC Main10,
// and it lands at roughly the same ~240 Mbps and the same file size per minute as
// a full-res clip, so nothing about storage planning changes. It also means the
// encoder is no longer asked to spend a large share of its bits describing
// sensor noise that nlm_rgb has already removed.
// 2.60 bpp -> ~243 Mbps requested. Raising this does NOTHING: device-measured,
// asking for 449 Mbps delivered 253, exactly what asking for 243 delivered. The
// encoder has its own ~254 Mbps hardware wall and ignores the request above it.
// Do not "fix" an encoder-quality problem by raising this number.
constexpr double  kTargetBppBinned     = 2.60;
constexpr int32_t kMaxBitrate          = 460'000'000;  // safe ceiling under L6.1 High tier
constexpr int32_t kGopSeconds          = 5;   // keyframe interval; 1s pulsed a ~1Hz hitch

// Must match the PushConstants block in debayer_isp.slang (112 bytes).
struct PushConstants {
    uint32_t out_w, out_h, stride_pixels, uv_word_offset;
    uint32_t raw_w, raw_h, cfa;
    float    white;
    float    black[4];
    float    wb[4];          // .w = pqScale
    float    ccm0[4], ccm1[4], ccm2[4];
};
static_assert(sizeof(PushConstants) == 112, "push constant layout drift");

// Must match the PushConstants block in nlm_bayer.slang (48 bytes). Each 16-byte
// group maps to a Slang vec4 slot (std140-safe — no bare vec3). The trailing vec4
// carries the NLM controls (the legacy bilateral ignored y/z/w), so the slot can
// run either shader without a layout change.
struct DenoisePush {
    uint32_t raw_w, raw_h;   // rawDim
    float    noise_k;
    float    noise_floor;
    float    black[4];       // black level per 2x2 CFA position
    float    white;          // .x of the trailing vec4 (nlm)
    float    search_radius;  // NLM search half-window, in same-colour steps
    float    patch_radius;   // NLM patch half-window, in same-colour steps
    float    h_scale;        // NLM filter strength (× shot-noise sigma)
};
static_assert(sizeof(DenoisePush) == 48, "denoise push constant layout drift");

// Must match the PushConstants block in green_isp.slang (32 bytes). float4 black
// aligns to a 16-byte boundary (std140), so the leading scalars pad to 16.
struct GreenPush {
    uint32_t raw_w, raw_h;   // rawDim
    uint32_t cfa;            // pattern only (low 2 bits)
    float    white;
    float    black[4];
};
static_assert(sizeof(GreenPush) == 32, "green push constant layout drift");

// Must match the PushConstants block in chroma_denoise.slang (32 bytes). uint2
// outDim aligns to 8; the remaining members are bare scalars (no vec3/vec4), so
// the layout is tight with no padding.
struct ChromaPush {
    uint32_t out_w, out_h;        // outDim
    uint32_t stride_pixels;       // Y stride in pixels
    uint32_t uv_word_offset;      // CbCr plane start (uint32 words)
    uint32_t chroma_stride;       // chroma scratch stride in words (= out_w/2)
    int32_t  radius;              // half window in chroma sites
    float    sigma_s;             // spatial gaussian sigma (sites)
    float    sigma_l;             // luma range sigma (10-bit units)
};
static_assert(sizeof(ChromaPush) == 32, "chroma push constant layout drift");

// Must match the PushConstants block in bin_isp.slang (48 bytes). float4 black
// aligns to a 16-byte boundary (std140), which is what the explicit padding
// after `white` is for — the shader declares the same two-float hole.
struct BinPush {
    uint32_t out_w, out_h;   // outDim
    uint32_t raw_w, raw_h;   // rawDim
    uint32_t cfa;            // pattern only (low 2 bits)
    float    white;
    float    pad[2];
    float    black[4];
};
static_assert(sizeof(BinPush) == 48, "bin push constant layout drift");

// Must match the PushConstants block in nlm_rgb.slang (112 bytes). The ten
// leading scalars come to 40 bytes; the explicit pad carries them to 48 so the
// four float4 rows land 16-byte aligned, which is what the shader's std140
// layout does implicitly. Drop the pad and every colour matrix shifts by 8 bytes.
struct NlmRgbPush {
    uint32_t out_w, out_h;        // outDim — the ENCODED, CTU-padded size
    uint32_t src_w, src_h;        // srcDim — the real size rgb_buf holds
    uint32_t stride_pixels;       // Y stride in pixels (padded domain)
    uint32_t uv_word_offset;      // CbCr plane start (uint32 words, padded)
    float    noise_k;             // already scaled by kBinNoiseScale
    float    noise_floor;         // likewise
    float    h_scale;             // NLM filter strength
    uint32_t flags;               // bit 0 = denoise on
    uint32_t pad[2];              // align the float4s — see above
    float    wb[4];               // .w = pqScale
    float    ccm0[4], ccm1[4], ccm2[4];
};
static_assert(sizeof(NlmRgbPush) == 112, "nlm_rgb push constant layout drift");

// Must match the PushConstants block in chroma_median.slang (32 bytes). src and
// dst strides are separate on purpose — see the shader's comment.
struct ChromaMedianPush {
    uint32_t out_w, out_h;
    uint32_t src_stride;
    uint32_t dst_stride;
    uint32_t dst_offset;      // 0 for a scratch target, uv_word_offset for P010
    uint32_t dilation;        // tap SPACING (1 or 2) — never a tap count
    uint32_t pad[2];
};
static_assert(sizeof(ChromaMedianPush) == 32, "chroma median push constant layout drift");

int64_t clock_ns(clockid_t clk) {
    struct timespec ts {};
    clock_gettime(clk, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1'000'000'000ll + ts.tv_nsec;
}

// ── 3x3 helpers (row-major) ──────────────────────────────────────────────────

void mat_mul(const double a[9], const double b[9], double out[9]) {
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            out[r * 3 + c] = a[r * 3 + 0] * b[0 + c] +
                             a[r * 3 + 1] * b[3 + c] +
                             a[r * 3 + 2] * b[6 + c];
}

bool mat_inv(const double m[9], double out[9]) {
    double det = m[0] * (m[4] * m[8] - m[5] * m[7]) -
                 m[1] * (m[3] * m[8] - m[5] * m[6]) +
                 m[2] * (m[3] * m[7] - m[4] * m[6]);
    if (std::fabs(det) < 1e-12) return false;
    double inv = 1.0 / det;
    out[0] =  (m[4] * m[8] - m[5] * m[7]) * inv;
    out[1] = -(m[1] * m[8] - m[2] * m[7]) * inv;
    out[2] =  (m[1] * m[5] - m[2] * m[4]) * inv;
    out[3] = -(m[3] * m[8] - m[5] * m[6]) * inv;
    out[4] =  (m[0] * m[8] - m[2] * m[6]) * inv;
    out[5] = -(m[0] * m[5] - m[2] * m[3]) * inv;
    out[6] =  (m[3] * m[7] - m[4] * m[6]) * inv;
    out[7] = -(m[0] * m[7] - m[1] * m[6]) * inv;
    out[8] =  (m[0] * m[4] - m[1] * m[3]) * inv;
    return true;
}

// XYZ(D50) -> XYZ(D65), Bradford-adapted.
const double kBradfordD50ToD65[9] = {
     0.9555766, -0.0230393,  0.0631636,
    -0.0282895,  1.0099416,  0.0210077,
     0.0122982, -0.0204830,  1.3299098,
};

// XYZ(D65) -> linear BT.2020 RGB.
const double kXyzD65ToBt2020[9] = {
     1.7166512, -0.3556708, -0.2533663,
    -0.6666844,  1.6164812,  0.0157685,
     0.0176399, -0.0427706,  0.9421031,
};

constexpr int32_t kIlluminantD65 = 21;  // EXIF LightSource code

} // namespace

// ── Color setup ──────────────────────────────────────────────────────────────

// DNG semantics: ForwardMatrix maps white-balanced camera RGB to XYZ(D50);
// ColorMatrix maps XYZ to camera RGB (so its inverse goes the other way).
// Prefer the D65 forward matrix (video is normally daylight-balanced), then
// any forward matrix, then inverse(ColorMatrix1). Finally row-normalize so
// camera neutral (1,1,1) maps exactly to BT.2020 white.
void RawVideoPipeline::derive_color_matrix() {
    double cam2xyz[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    if (meta_.has_fm2 && meta_.illuminant2 == kIlluminantD65) {
        std::memcpy(cam2xyz, meta_.forward_matrix2, sizeof(cam2xyz));
    } else if (meta_.has_fm1) {
        std::memcpy(cam2xyz, meta_.forward_matrix1, sizeof(cam2xyz));
    } else if (meta_.has_cm1) {
        double inv[9];
        if (mat_inv(meta_.color_matrix1, inv)) std::memcpy(cam2xyz, inv, sizeof(cam2xyz));
        else LOGE("color_matrix1 not invertible — using identity CCM");
    } else {
        LOGE("no color matrices in metadata — using identity CCM");
    }

    double adapt[9], full[9];
    mat_mul(kXyzD65ToBt2020, kBradfordD50ToD65, adapt);
    mat_mul(adapt, cam2xyz, full);

    // Neutral preservation: scale each row so (1,1,1) -> (1,1,1).
    for (int r = 0; r < 3; ++r) {
        double e = full[r * 3] + full[r * 3 + 1] + full[r * 3 + 2];
        if (std::fabs(e) < 1e-9) e = 1.0;
        for (int c = 0; c < 3; ++c) ccm_[r * 3 + c] = static_cast<float>(full[r * 3 + c] / e);
    }
    LOGI("CCM sensor->BT2020: [%.4f %.4f %.4f | %.4f %.4f %.4f | %.4f %.4f %.4f]",
         ccm_[0], ccm_[1], ccm_[2], ccm_[3], ccm_[4], ccm_[5], ccm_[6], ccm_[7], ccm_[8]);
}

void RawVideoPipeline::set_neutral(const float neutral[3]) {
    float g[3];
    for (int i = 0; i < 3; ++i) {
        g[i] = (neutral[i] > 1e-6f) ? 1.0f / neutral[i] : 1.0f;
        wb_[i].store(g[i], std::memory_order_relaxed);
    }
    // release-ordered: pairs with the acquire load in pipeline_loop, so a reader
    // that sees wb_valid_ also sees the gains above.
    if (!wb_valid_.exchange(true, std::memory_order_release)) {
        LOGI("WB gains %.3f %.3f %.3f (valid)", g[0], g[1], g[2]);
        wb_cv_.notify_all();
    }
}

// The sensor's own noise model for the frame being captured, straight from
// ACAMERA_SENSOR_NOISE_PROFILE: variance at normalised signal x is S*x + O, per
// CFA channel. That is exactly the form nlm_rgb.slang's noiseK/noiseFloor take,
// so this replaces the hardcoded kNoiseK/kNoiseFloor guesses with a measurement
// that tracks the ISO actually in use — which is what makes the denoise strength
// automatically right in a dark room and gentle in a bright one.
//
// The four CFA channels are averaged into one (S, O). nlm_rgb denoises a
// demosaic-free RGB plane whose patch distance is a luma mix of all three
// channels, so a per-channel model has nowhere to be applied; the mean is the
// honest summary. kBinNoiseScale is applied at use, not here.
//
// Written on the camera thread once per frame, read on the pipeline thread while
// recording the command buffer — same lock-free pattern as set_neutral above.
void RawVideoPipeline::set_noise_profile(const double np[8], int count) {
    if (count < 2) return;
    const int pairs = count / 2;
    double s = 0.0, o = 0.0;
    for (int i = 0; i < pairs; ++i) { s += np[i * 2]; o += np[i * 2 + 1]; }
    s /= pairs;
    o /= pairs;
    // A HAL that reports a degenerate model would otherwise switch the denoise
    // off entirely; keep the compiled-in fallback in that case.
    if (!(s > 0.0) || !(o >= 0.0)) return;
    noise_s_.store(static_cast<float>(s), std::memory_order_relaxed);
    noise_o_.store(static_cast<float>(o), std::memory_order_relaxed);
    if (!noise_valid_.exchange(true, std::memory_order_release)) {
        // Logged once per clip: this is the number that decides whether the
        // denoiser was previously over- or under-filtering (the hardcoded pair
        // was S=%.4f O=%.5f).
        LOGI("sensor noise profile: S=%.6f O=%.7f (%d pairs) | hardcoded was S=%.4f O=%.5f",
             s, o, pairs, kNoiseK, kNoiseFloor);
    }
}

// On this pipeline the audio is anchored to CLOCK_MONOTONIC, but the camera
// sensor timestamp may be CLOCK_BOOTTIME (differs by accumulated suspend time)
// or some other epoch. Rebase onto MONOTONIC so A/V share one clock; the offset
// is fixed for the whole recording so relative frame timing stays exact.
int64_t RawVideoPipeline::compute_ts_offset(int64_t sensor_ts) {
    const int64_t mono = clock_ns(CLOCK_MONOTONIC);
    const int64_t boot = clock_ns(CLOCK_BOOTTIME);
    constexpr int64_t k5s = 5'000'000'000ll;
    if (llabs(sensor_ts - mono) < k5s) {
        LOGI("ts domain: monotonic (no rebase)");
        return 0;
    }
    if (llabs(sensor_ts - boot) < k5s) {
        LOGI("ts domain: boottime -> monotonic (offset %lld ms)",
             static_cast<long long>((boot - mono) / 1'000'000));
        return boot - mono;
    }
    LOGI("ts domain: unknown epoch -> rebase first frame to now");
    return sensor_ts - mono;
}

// ── Init ─────────────────────────────────────────────────────────────────────

bool RawVideoPipeline::init(AAssetManager* assets, const dng::DngMeta& meta,
                            int raw_w, int raw_h, int fps, bool bin2) {
    if (ready_) return true;
    meta_  = meta;
    raw_w_ = raw_w;
    raw_h_ = raw_h;
    fps_   = fps > 0 ? fps : 30;
    bin2_  = bin2;
    // bin_isp.slang reads a RAW row as packed uint32 pairs (`rawBuf[idx >> 1]`),
    // which only selects the right half of each word when a row starts on a word
    // boundary. An odd width would invert that selection on every other row and
    // produce garbage rather than an error, so refuse the binned path instead.
    if (bin2_ && (raw_w & 1)) {
        LOGE("RAW width %d is odd — binned path needs word-aligned rows; using full-res", raw_w);
        bin2_ = false;
    }
    // On for the binned path: it removes the isolated colour speckle a
    // demosaic-free bin leaves in shadows (R and B are single photosites there,
    // so their noise lands almost entirely in chroma), and — like the NLM — that
    // speckle is noise the encoder would otherwise have to spend bits describing.
    // ~0.8 ms per pass. See docs/DORMANT_METHODS.md.
    if (bin2_) chroma_enabled_.store(true, std::memory_order_relaxed);
    // Binned: one output pixel per 2x2 CFA quad. The extra & ~1 matters — the
    // P010 pack and the 4:2:0 chroma both work on 2x2 output quads, so an odd
    // output dimension would leave a half-written edge row/column.
    out_w_ = bin2_ ? ((raw_w / 2) & ~1) : (raw_w & ~1);
    out_h_ = bin2_ ? ((raw_h / 2) & ~1) : (raw_h & ~1);
    // Round the ENCODED size up to whole coding tree units. HEVC codes in 64x64
    // CTUs anchored at the top-left corner, so a picture that is not a multiple
    // of 64 leaves partial CTUs along the right and bottom edges; the encoder
    // fills them itself and signals a conformance window, and an HEVC
    // conformance window can only crop from the right and the bottom. That
    // asymmetry is the fingerprint of the streaked ~1 mm border seen on every
    // clip this project has ever produced (2040/64 = 31.875, 1530/64 = 23.9 —
    // and the full-res path is no better at 4080/64 = 63.75, 3060/64 = 47.8).
    //
    // We ADD the missing rows and columns instead, filling them by replicating
    // the last real column and row (the standard edge extension — a motion
    // vector pointing into the pad then predicts from real picture content
    // rather than from invented data). NOTHING IS CROPPED: the sensor's whole
    // field of view survives untouched, the picture simply gains a few pixels of
    // duplicated border. Both native geometries stay exactly 4:3 through this
    // (2040x1530 -> 2048x1536, 4080x3060 -> 4096x3072), so the aspect ratio does
    // not move either.
    pad_w_ = (out_w_ + kCtuAlign - 1) / kCtuAlign * kCtuAlign;
    pad_h_ = (out_h_ + kCtuAlign - 1) / kCtuAlign * kCtuAlign;
    // Perceptual-bpp target from the ENCODED geometry (that is what the encoder
    // actually spends bits on), clamped under the HEVC L6.0 High-tier ceiling
    // (see kTargetBpp/kMaxBitrate above).
    int64_t br = llround((bin2_ ? kTargetBppBinned : kTargetBpp) * pad_w_ * pad_h_ * fps_);
    bitrate_ = static_cast<int>(br < kMaxBitrate ? br : kMaxBitrate);

    if (meta_.has_neutral) {
        float n[3] = { static_cast<float>(meta_.as_shot_neutral[0]),
                       static_cast<float>(meta_.as_shot_neutral[1]),
                       static_cast<float>(meta_.as_shot_neutral[2]) };
        set_neutral(n);
    }
    derive_color_matrix();

    if (!probe_encoder()) {
        LOGE("no P010 HEVC Main10 encoder — RAW video unavailable");
        return false;
    }
    if (!build_vulkan(assets)) {
        LOGE("Vulkan ISP setup failed — RAW video unavailable");
        // build_vulkan bails at the first failure, leaving whatever it already
        // created (pipelines, layouts, buffers, the descriptor pool) alive.
        // shutdown() is written to skip null handles, so it is the safe way to
        // release that partial state instead of leaking it.
        shutdown();
        return false;
    }
    ready_ = true;
    LOGI("RAW pipeline ready: %dx%d Bayer -> %dx%d P010 PQ%s @ %d Mbps "
         "(encoded %dx%d, +%d/+%d CTU pad, buffer input)",
         raw_w_, raw_h_, out_w_, out_h_, bin2_ ? " (bin2)" : "", bitrate_ / 1'000'000,
         pad_w_, pad_h_, pad_w_ - out_w_, pad_h_ - out_h_);
    return true;
}

// Builds one compute pipeline (descriptor-set layout + pipeline layout +
// pipeline) from a SPIR-V asset. `bindings`/`binding_count` describe the set;
// `push_size` is the push-constant block size.
// Android's libvulkan.so exports only Vulkan 1.0 symbols, so this 1.1 entry
// point has to come from vkGetDeviceProcAddr. Null means "not available" and the
// helper falls back to a single unbanded dispatch.
static PFN_vkCmdDispatchBase g_cmd_dispatch_base = nullptr;

static void load_dispatch_base(VkDevice dev) {
    g_cmd_dispatch_base = reinterpret_cast<PFN_vkCmdDispatchBase>(
        vkGetDeviceProcAddr(dev, "vkCmdDispatchBase"));
    if (!g_cmd_dispatch_base)
        g_cmd_dispatch_base = reinterpret_cast<PFN_vkCmdDispatchBase>(
            vkGetDeviceProcAddr(dev, "vkCmdDispatchBaseKHR"));
    if (!g_cmd_dispatch_base)
        LOGI("vkCmdDispatchBase unavailable - ISP passes stay unbanded");
}

static void dispatch_banded(VkCommandBuffer cmd, uint32_t gx, uint32_t gy, uint32_t gz) {
    const uint32_t bands = g_cmd_dispatch_base
                         ? std::min<uint32_t>(RawVideoPipeline::kDispatchBands, gy) : 1u;
    if (bands <= 1) { vkCmdDispatch(cmd, gx, gy, gz); return; }
    const uint32_t step = (gy + bands - 1) / bands;
    for (uint32_t base = 0; base < gy; base += step) {
        const uint32_t n = std::min(step, gy - base);
        g_cmd_dispatch_base(cmd, 0, base, 0, gx, n, gz);
    }
}

static bool build_compute(VkCompute& vk, AAssetManager* assets, const char* spv,
                          const VkDescriptorSetLayoutBinding* bindings, uint32_t binding_count,
                          uint32_t push_size, VkDescriptorSetLayout& dsl,
                          VkPipelineLayout& layout, VkPipeline& pipeline) {
    VkDevice dev = vk.device();
    VkDescriptorSetLayoutCreateInfo dslci{};
    dslci.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslci.bindingCount = binding_count;
    dslci.pBindings    = bindings;
    if (vkCreateDescriptorSetLayout(dev, &dslci, nullptr, &dsl) != VK_SUCCESS) return false;

    VkPushConstantRange pcr{ VK_SHADER_STAGE_COMPUTE_BIT, 0, push_size };
    VkPipelineLayoutCreateInfo plci{};
    plci.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount         = 1;
    plci.pSetLayouts            = &dsl;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges    = &pcr;
    if (vkCreatePipelineLayout(dev, &plci, nullptr, &layout) != VK_SUCCESS) return false;

    VkShaderModule shader = vk.load_shader(assets, spv);
    if (shader == VK_NULL_HANDLE) return false;
    VkComputePipelineCreateInfo cpci{};
    cpci.sType        = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cpci.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpci.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
    cpci.stage.module = shader;
    cpci.stage.pName  = "main";
    cpci.layout       = layout;
    // DISPATCH_BASE lets the ISP issue each pass as a series of row bands via
    // vkCmdDispatchBase (see dispatch_banded). Adreno only preempts at dispatch
    // boundaries, so a single full-frame dispatch is an uninterruptible block —
    // which is what made a swapchain present stall behind it (see app.cc).
    cpci.flags        = VK_PIPELINE_CREATE_DISPATCH_BASE_BIT;
    VkResult pr = vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, nullptr, &pipeline);
    vkDestroyShaderModule(dev, shader, nullptr);
    if (pr != VK_SUCCESS) { LOGE("compute pipeline creation failed for %s", spv); return false; }
    return true;
}

// Buffers + descriptors for the binned path. Called from build_vulkan once the
// bin_isp / nlm_rgb pipelines exist. Per frame the chain is
//   staging_[slot] (RAW16) -> f.rgb_buf (half-res linear RGB) -> f.out_buf (P010)
// with both intermediates per-slot, so no cross-slot WAR barrier is ever needed.
bool RawVideoPipeline::build_vulkan_binned(VkDevice dev) {
    // 5 sets per in-flight slot: bin(2 bufs), nlm_rgb(3), and one per median
    // pass (2 each) — the median runs three times, ping-ponging the scratches.
    VkDescriptorPoolSize sizes[1] = {
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 11 * kInFlight },
    };
    VkDescriptorPoolCreateInfo dpci{};
    dpci.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.maxSets       = 5 * kInFlight;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes    = sizes;
    if (vkCreateDescriptorPool(dev, &dpci, nullptr, &dpool_) != VK_SUCCESS) return false;

    // RAW16 input ring — unchanged by binning: the camera still delivers full
    // sensor resolution and bin_isp is what reduces it on the GPU.
    VkDeviceSize raw_bytes = static_cast<VkDeviceSize>(raw_w_) * raw_h_ * 2;
    bool raw_devlocal = true;
    for (auto& st : staging_) {
        if (!vk_.create_buffer(raw_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, st)) {
            raw_devlocal = false;
            if (!vk_.create_buffer(raw_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, st)) {
                LOGE("RAW input buffer creation failed");
                return false;
            }
        }
    }

    // Half-res linear camera RGB, 3 floats per pixel.
    VkDeviceSize rgb_bytes  = static_cast<VkDeviceSize>(out_w_) * out_h_ * 3 * 4;
    VkDeviceSize p010_bytes = static_cast<VkDeviceSize>(pad_w_) * pad_h_ * 3;
    bool out_cached = true;
    VkDescriptorBufferInfo raw0_info{ staging_[0].buf, 0, VK_WHOLE_SIZE };

    for (int k = 0; k < kInFlight; ++k) {
        InFlight& f = inflight_[k];

        if (!vk_.create_buffer(rgb_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, f.rgb_buf)) {
            LOGE("binned RGB buffer creation failed (slot %d)", k);
            return false;
        }
        // Per-slot CbCr scratch for the chroma median: one packed (Cb,Cr) word per
        // 4:2:0 site = (out_w/2)*(out_h/2)*4 bytes = out_w*out_h. Per-slot, not
        // shared, so no cross-slot WAR barrier is needed (a shared intermediate is
        // what serialised the in-flight ring before denoised_buf became per-slot).
        VkDeviceSize chroma_bytes = static_cast<VkDeviceSize>(pad_w_) * pad_h_;
        if (!vk_.create_buffer(chroma_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, f.chroma_buf)) {
            LOGE("chroma scratch creation failed (slot %d)", k);
            return false;
        }
        if (!vk_.create_buffer(chroma_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, f.chroma_buf2)) {
            LOGE("chroma scratch 2 creation failed (slot %d)", k);
            return false;
        }
        // Per-slot P010 output. Prefer cached host memory for the CPU readback.
        if (!vk_.create_buffer(p010_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                               VK_MEMORY_PROPERTY_HOST_CACHED_BIT, f.out_buf)) {
            out_cached = false;
            if (!vk_.create_buffer(p010_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, f.out_buf)) {
                LOGE("P010 output buffer creation failed (slot %d)", k);
                return false;
            }
        }

        VkDescriptorSetAllocateInfo dsai{};
        dsai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool     = dpool_;
        dsai.descriptorSetCount = 1;
        dsai.pSetLayouts        = &bin_dsl_;
        if (vkAllocateDescriptorSets(dev, &dsai, &f.bin_dset) != VK_SUCCESS) return false;
        dsai.pSetLayouts        = &nrgb_dsl_;
        if (vkAllocateDescriptorSets(dev, &dsai, &f.nrgb_dset) != VK_SUCCESS) return false;
        dsai.pSetLayouts        = &cmed_dsl_;
        if (vkAllocateDescriptorSets(dev, &dsai, &f.cmed_dset1) != VK_SUCCESS) return false;
        dsai.pSetLayouts        = &cmed_dsl_;
        if (vkAllocateDescriptorSets(dev, &dsai, &f.cmed_dset2) != VK_SUCCESS) return false;
        dsai.pSetLayouts        = &cmed_dsl_;
        if (vkAllocateDescriptorSets(dev, &dsai, &f.cd_dset) != VK_SUCCESS) return false;

        VkDescriptorBufferInfo rgb_info{ f.rgb_buf.buf, 0, VK_WHOLE_SIZE };
        VkDescriptorBufferInfo out_info{ f.out_buf.buf, 0, VK_WHOLE_SIZE };
        VkDescriptorBufferInfo chroma_info { f.chroma_buf.buf,  0, VK_WHOLE_SIZE };
        VkDescriptorBufferInfo chroma2_info{ f.chroma_buf2.buf, 0, VK_WHOLE_SIZE };

        auto wbuf = [](VkDescriptorSet set, uint32_t binding, const VkDescriptorBufferInfo* bi) {
            VkWriteDescriptorSet w{};
            w.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w.dstSet          = set;
            w.dstBinding      = binding;
            w.descriptorCount = 1;
            w.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            w.pBufferInfo     = bi;
            return w;
        };
        // bin_dset binding 0 is a placeholder repointed at the live staging slot
        // each frame, exactly as the full-res raw-reading sets are.
        const VkWriteDescriptorSet writes[] = {
            wbuf(f.bin_dset,  0, &raw0_info),   // raw (placeholder)
            wbuf(f.bin_dset,  1, &rgb_info),    // linear RGB out
            wbuf(f.nrgb_dset, 0, &rgb_info),    // linear RGB in
            wbuf(f.nrgb_dset, 1, &out_info),    // P010 out
            wbuf(f.nrgb_dset, 2, &chroma_info), // CbCr scratch (median path)
            // The three median passes ping-pong the two scratches:
            //   1 chroma_buf -> chroma_buf2, 2 chroma_buf2 -> chroma_buf,
            //   3 chroma_buf -> the P010 CbCr plane.
            wbuf(f.cmed_dset1, 0, &chroma2_info), // dst = scratch 2
            wbuf(f.cmed_dset1, 1, &chroma_info),  // src = noisy CbCr
            wbuf(f.cmed_dset2, 0, &chroma_info),  // dst = scratch 1 (reused)
            wbuf(f.cmed_dset2, 1, &chroma2_info), // src = once-medianed CbCr
            wbuf(f.cd_dset,    0, &out_info),     // dst = P010 (CbCr written)
            wbuf(f.cd_dset,    1, &chroma_info),  // src = twice-medianed CbCr
        };
        vkUpdateDescriptorSets(dev, static_cast<uint32_t>(std::size(writes)), writes, 0, nullptr);

        f.cmd   = vk_.alloc_cmd();
        f.fence = vk_.create_fence(false);
        if (f.cmd == VK_NULL_HANDLE || f.fence == VK_NULL_HANDLE) return false;

        // Timestamp pool for real per-pass timing. Optional by design: a device
        // whose compute family cannot timestamp still records normally, it just
        // reports no per-pass numbers. Never a failure path.
        if (vk_.timestamp_period_ns() > 0.0f) {
            VkQueryPoolCreateInfo qpci{};
            qpci.sType      = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
            qpci.queryType  = VK_QUERY_TYPE_TIMESTAMP;
            qpci.queryCount = kTimestampSlots;
            if (vkCreateQueryPool(dev, &qpci, nullptr, &f.qpool) != VK_SUCCESS)
                f.qpool = VK_NULL_HANDLE;
        }
    }
    gpu_timing_ = vk_.timestamp_period_ns() > 0.0f;
    for (const auto& f : inflight_) if (f.qpool == VK_NULL_HANDLE) gpu_timing_ = false;
    LOGI("GPU timing: %s (%.2f ns/tick)", gpu_timing_ ? "on" : "unavailable",
         vk_.timestamp_period_ns());
    LOGI("RAW input: %s | P010 readback: %s (%d-deep ring, bin2)",
         raw_devlocal ? "DEVICE_LOCAL|HOST_VISIBLE" : "host-visible (uncached GPU reads!)",
         out_cached ? "HOST_CACHED" : "uncached(slow!)", kInFlight);
    return true;
}

bool RawVideoPipeline::build_vulkan(AAssetManager* assets) {
    if (!vk_.init()) return false;
    VkDevice dev = vk_.device();
    load_dispatch_base(dev);

    // All bindings are storage buffers now (raw + denoised + P010 are all
    // uint16/uint32-packed buffers — no images, no upload).
    // ── Denoise pass (spatio-temporal NLM): binding 0 = current frame, 1 =
    // denoised out. The pass is spatial-only: it used to also bind the two most
    // recent past frames, which nlm_bayer.slang never read (every temporal
    // variant tried here smeared handheld motion), so they cost a descriptor
    // write per frame for nothing.
    // nlm_bayer.slang searches patches across all of them; downstream is unchanged.
    // The binned path needs neither the Bayer NLM, the green prepass, the
    // demosaic, nor the chroma denoise: bin_isp collapses each 2x2 quad to one
    // pixel (so there is nothing to interpolate) and nlm_rgb denoises all three
    // channels of that plane and develops it in the same pass. Building the
    // full-res chain anyway would cost four pipelines and ~100 MB of buffers
    // that no dispatch would ever reference.
    if (bin2_) {
        // ── Bin pass: binding 0 = raw buf, 1 = per-slot linear RGB. ──
        VkDescriptorSetLayoutBinding bin_bind[2]{};
        for (uint32_t i = 0; i < 2; ++i)
            bin_bind[i] = { i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
        if (!build_compute(vk_, assets, "shaders/bin_isp.spv", bin_bind, 2,
                           sizeof(BinPush), bin_dsl_, bin_layout_, bin_pipeline_))
            return false;

        // ── NLM+develop pass: binding 0 = linear RGB, 1 = P010 out. ──
        // Same fp16 story as the Bayer NLM: this pass is bound by groupshared
        // traffic and ALU rate, and half improves both.
        const char* nrgb_spv = vk_.fp16_supported() ? "shaders/nlm_rgb_fp16.spv"
                                                    : "shaders/nlm_rgb.spv";
        LOGI("binned NLM kernel: %s", vk_.fp16_supported() ? "fp16" : "fp32");
        // Binding 2 is the CbCr scratch the chroma median reads; it is bound
        // whether or not the median runs (an unused binding costs nothing, and a
        // conditional layout would mean two pipelines).
        VkDescriptorSetLayoutBinding nr_bind[3]{};
        for (uint32_t i = 0; i < 3; ++i)
            nr_bind[i] = { i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
        if (!build_compute(vk_, assets, nrgb_spv, nr_bind, 3,
                           sizeof(NlmRgbPush), nrgb_dsl_, nrgb_layout_, nrgb_pipeline_))
            return false;

        // ── Chroma median: binding 0 = P010 out (CbCr written), 1 = scratch. ──
        VkDescriptorSetLayoutBinding cm_bind[2]{};
        for (uint32_t i = 0; i < 2; ++i)
            cm_bind[i] = { i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
        if (!build_compute(vk_, assets, "shaders/chroma_median.spv", cm_bind, 2,
                           sizeof(ChromaMedianPush), cmed_dsl_, cmed_layout_, cmed_pipeline_))
            return false;

        return build_vulkan_binned(dev);
    }

    VkDescriptorSetLayoutBinding dn_bind[2]{};
    for (uint32_t i = 0; i < 2; ++i)
        dn_bind[i] = { i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    // fp16 halves this pass's groupshared traffic and doubles its ALU rate, and
    // it is bound by both. Devices without shaderFloat16 get the fp32 build.
    const char* nlm_spv = vk_.fp16_supported() ? "shaders/nlm_bayer_fp16.spv"
                                               : "shaders/nlm_bayer.spv";
    LOGI("NLM kernel: %s", vk_.fp16_supported() ? "fp16" : "fp32");
    if (!build_compute(vk_, assets, nlm_spv, dn_bind, 2,
                       sizeof(DenoisePush), denoise_dsl_, denoise_layout_, denoise_pipeline_))
        return false;

    // ── Green prepass: binding 0 = raw/denoised buffer, binding 1 = green buf. ──
    VkDescriptorSetLayoutBinding gn_bind[2]{};
    gn_bind[0] = { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    gn_bind[1] = { 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    if (!build_compute(vk_, assets, "shaders/green_isp.spv", gn_bind, 2,
                       sizeof(GreenPush), green_dsl_, green_layout_, green_pipeline_))
        return false;

    // ── ISP pass: binding 0 = raw/denoised, 1 = P010, 2 = green, 3 = chroma. ──
    VkDescriptorSetLayoutBinding isp_bind[4]{};
    isp_bind[0] = { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    isp_bind[1] = { 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    isp_bind[2] = { 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    isp_bind[3] = { 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    if (!build_compute(vk_, assets, "shaders/debayer_isp.spv", isp_bind, 4,
                       sizeof(PushConstants), dsl_, layout_, pipeline_))
        return false;

    // ── Chroma denoise pass: binding 0 = P010 out (RW), 1 = chroma scratch. ──
    VkDescriptorSetLayoutBinding cd_bind[2]{};
    cd_bind[0] = { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    cd_bind[1] = { 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    if (!build_compute(vk_, assets, "shaders/chroma_denoise.spv", cd_bind, 2,
                       sizeof(ChromaPush), chroma_dsl_, chroma_layout_, chroma_pipeline_))
        return false;

    // 6 sets per in-flight slot: denoise(2 bufs), green-from-raw(2),
    // green-from-denoised(2), ISP-from-denoised(4), ISP-from-raw(4),
    // chroma-denoise(2).
    VkDescriptorPoolSize sizes[1] = {
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 16 * kInFlight },
    };
    VkDescriptorPoolCreateInfo dpci{};
    dpci.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.maxSets       = 6 * kInFlight;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes    = sizes;
    if (vkCreateDescriptorPool(dev, &dpci, nullptr, &dpool_) != VK_SUCCESS) return false;

    // RAW16 buffers are uint16-packed (2 px per uint32): raw_w*raw_h*2 bytes.
    VkDeviceSize raw_bytes = static_cast<VkDeviceSize>(raw_w_) * raw_h_ * 2;

    // Shared green plane (device-local; one float per pixel; HQ demosaic only).
    VkDeviceSize green_bytes = static_cast<VkDeviceSize>(raw_w_) * raw_h_ * 4;
    if (!vk_.create_buffer(green_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, green_buf_)) {
        LOGE("green buffer creation failed");
        return false;
    }

    // RAW16 input ring: the camera writes these and the ISP reads them directly
    // (no upload). Prefer DEVICE_LOCAL | HOST_VISIBLE so the GPU's scattered
    // demosaic reads stay cached on UMA; fall back to plain host-visible.
    bool raw_devlocal = true;
    for (auto& s : staging_) {
        if (!vk_.create_buffer(raw_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, s)) {
            raw_devlocal = false;
            if (!vk_.create_buffer(raw_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, s)) {
                LOGE("RAW input buffer creation failed");
                return false;
            }
        }
    }

    VkDeviceSize p010_bytes = static_cast<VkDeviceSize>(pad_w_) * pad_h_ * 3;  // Y + UV/2, 2B each
    bool out_cached = true;
    // Fixed buffer infos referenced by every slot's writes (binding 0 of the raw
    // sets is a placeholder repointed per frame in record_and_submit).
    VkDescriptorBufferInfo green_info{ green_buf_.buf,    0, VK_WHOLE_SIZE };
    VkDescriptorBufferInfo raw0_info { staging_[0].buf,   0, VK_WHOLE_SIZE };
    for (int k = 0; k < kInFlight; ++k) {
        InFlight& f = inflight_[k];

        // Per-slot NLM output (device-local). Per-slot rather than shared so the
        // denoise write of this frame needs no WAR barrier against the other
        // slot's ISP read — that barrier serialized the whole in-flight ring.
        if (!vk_.create_buffer(raw_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, f.denoised_buf)) {
            LOGE("denoised buffer creation failed (slot %d)", k);
            return false;
        }
        VkDescriptorBufferInfo dn_info{ f.denoised_buf.buf, 0, VK_WHOLE_SIZE };

        // Per-slot P010 output. Prefer cached host memory for the CPU readback.
        if (!vk_.create_buffer(p010_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                               VK_MEMORY_PROPERTY_HOST_CACHED_BIT, f.out_buf)) {
            out_cached = false;
            if (!vk_.create_buffer(p010_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, f.out_buf)) {
                LOGE("P010 output buffer creation failed (slot %d)", k);
                return false;
            }
        }

        // Per-slot chroma scratch (device-local; one packed (Cb,Cr) word per
        // 4:2:0 site = out_w*out_h bytes). Only used on the CD path.
        VkDeviceSize chroma_bytes = static_cast<VkDeviceSize>(pad_w_) * pad_h_;
        if (!vk_.create_buffer(chroma_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, f.chroma_buf)) {
            LOGE("chroma scratch buffer creation failed (slot %d)", k);
            return false;
        }

        VkDescriptorSetAllocateInfo dsai{};
        dsai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool     = dpool_;
        dsai.descriptorSetCount = 1;
        dsai.pSetLayouts        = &denoise_dsl_;
        if (vkAllocateDescriptorSets(dev, &dsai, &f.denoise_dset) != VK_SUCCESS) return false;
        dsai.pSetLayouts        = &green_dsl_;
        if (vkAllocateDescriptorSets(dev, &dsai, &f.green_dset_raw) != VK_SUCCESS) return false;
        dsai.pSetLayouts        = &green_dsl_;
        if (vkAllocateDescriptorSets(dev, &dsai, &f.green_dset_dn) != VK_SUCCESS) return false;
        dsai.pSetLayouts        = &dsl_;
        if (vkAllocateDescriptorSets(dev, &dsai, &f.dset) != VK_SUCCESS) return false;
        dsai.pSetLayouts        = &dsl_;
        if (vkAllocateDescriptorSets(dev, &dsai, &f.dset_raw) != VK_SUCCESS) return false;
        dsai.pSetLayouts        = &chroma_dsl_;
        if (vkAllocateDescriptorSets(dev, &dsai, &f.cd_dset) != VK_SUCCESS) return false;

        VkDescriptorBufferInfo out_info{ f.out_buf.buf,    0, VK_WHOLE_SIZE };
        VkDescriptorBufferInfo chroma_info { f.chroma_buf.buf,  0, VK_WHOLE_SIZE };
        VkDescriptorBufferInfo chroma2_info{ f.chroma_buf2.buf, 0, VK_WHOLE_SIZE };

        auto wbuf = [](VkDescriptorSet set, uint32_t binding, const VkDescriptorBufferInfo* bi) {
            VkWriteDescriptorSet w{};
            w.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w.dstSet          = set;
            w.dstBinding      = binding;
            w.descriptorCount = 1;
            w.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            w.pBufferInfo     = bi;
            return w;
        };
        // Binding 0 of the raw-reading sets (denoise_dset, green_dset_raw,
        // dset_raw) is a placeholder repointed at the live staging slot each
        // frame. The denoised-reading sets (green_dset_dn, dset) bind it fixed.
        // The green plane is shared and bound fixed on both ISP sets.
        const VkWriteDescriptorSet writes[] = {
            wbuf(f.denoise_dset,   0, &raw0_info),    // current frame (placeholder)
            wbuf(f.denoise_dset,   1, &dn_info),      // denoised out
            wbuf(f.green_dset_raw, 0, &raw0_info),    // raw (placeholder)
            wbuf(f.green_dset_raw, 1, &green_info),   // green out
            wbuf(f.green_dset_dn,  0, &dn_info),      // denoised in
            wbuf(f.green_dset_dn,  1, &green_info),   // green out
            wbuf(f.dset,           0, &dn_info),      // denoised in
            wbuf(f.dset,           1, &out_info),     // P010 out
            wbuf(f.dset,           2, &green_info),   // green in
            wbuf(f.dset,           3, &chroma_info),  // chroma scratch (CD)
            wbuf(f.dset_raw,       0, &raw0_info),    // raw (placeholder)
            wbuf(f.dset_raw,       1, &out_info),     // P010 out
            wbuf(f.dset_raw,       2, &green_info),   // green in
            wbuf(f.dset_raw,       3, &chroma_info),  // chroma scratch (CD)
        };
        vkUpdateDescriptorSets(dev, static_cast<uint32_t>(std::size(writes)),
                               writes, 0, nullptr);

        // Chroma denoise set: reads out_buf (luma guidance) + this slot's chroma
        // scratch; writes the cleaned CbCr back into out_buf.
        const VkWriteDescriptorSet cd_writes[] = {
            wbuf(f.cd_dset, 0, &out_info),     // P010 out (RW: read Y, write CbCr)
            wbuf(f.cd_dset, 1, &chroma_info),  // noisy chroma scratch (read)
        };
        vkUpdateDescriptorSets(dev, static_cast<uint32_t>(std::size(cd_writes)),
                               cd_writes, 0, nullptr);

        f.cmd   = vk_.alloc_cmd();
        f.fence = vk_.create_fence(false);
        if (f.cmd == VK_NULL_HANDLE || f.fence == VK_NULL_HANDLE) return false;
    }
    LOGI("RAW input: %s | P010 readback: %s (%d-deep ring)",
         raw_devlocal ? "DEVICE_LOCAL|HOST_VISIBLE" : "host-visible (uncached GPU reads!)",
         out_cached ? "HOST_CACHED" : "uncached(slow!)", kInFlight);
    return true;
}

// ── Encoder ──────────────────────────────────────────────────────────────────

AMediaCodec* RawVideoPipeline::make_encoder() {
    AMediaCodec* codec = AMediaCodec_createEncoderByType("video/hevc");
    if (!codec) { LOGE("no video/hevc encoder"); return nullptr; }

    AMediaFormat* fmt = AMediaFormat_new();
    AMediaFormat_setString(fmt, AMEDIAFORMAT_KEY_MIME, "video/hevc");
    AMediaFormat_setInt32(fmt, "width",  pad_w_);
    AMediaFormat_setInt32(fmt, "height", pad_h_);
    AMediaFormat_setInt32(fmt, "color-format", kColorFormatYUVP010);
    AMediaFormat_setInt32(fmt, "profile", kHevcProfileMain10);
    AMediaFormat_setInt32(fmt, "frame-rate", fps_);
    // 5s GOP, not 1s: a 1-second keyframe interval put a big I-frame (and the
    // muxer cluster flush it triggers) on a once-per-second beat — the ~1Hz hitch.
    AMediaFormat_setInt32(fmt, "i-frame-interval", kGopSeconds);
    AMediaFormat_setInt32(fmt, "bitrate", bitrate_);
    AMediaFormat_setInt32(fmt, "bitrate-mode", kBitrateModeVbr);
    // Run the encoder realtime at full clocks instead of best-effort/power-save.
    // Without these the HW encoder under-clocks and can't drain 30fps at 4K10
    // (frames pile up at dequeueInputBuffer). priority 0 = realtime;
    // operating-rate requests max performance. Zero effect on output quality.
    AMediaFormat_setInt32(fmt, "priority", 0);
    AMediaFormat_setFloat(fmt, "operating-rate", 120.0f);
    // Bitstream VUI must agree with the shader output and the container tags:
    // BT.2020 primaries, PQ transfer, FULL range.
    AMediaFormat_setInt32(fmt, "color-standard", kColorStandardBt2020);
    AMediaFormat_setInt32(fmt, "color-transfer", kColorTransferSt2084);
    AMediaFormat_setInt32(fmt, "color-range",    kColorRangeFull);

    media_status_t st = AMediaCodec_configure(
        codec, fmt, nullptr, nullptr, AMEDIACODEC_CONFIGURE_FLAG_ENCODE);
    AMediaFormat_delete(fmt);
    if (st != AMEDIA_OK) {
        LOGE("encoder configure failed (%d) — P010/Main10 unsupported?", st);
        AMediaCodec_delete(codec);
        return nullptr;
    }
    return codec;
}

bool RawVideoPipeline::probe_encoder() {
    AMediaCodec* codec = make_encoder();
    if (!codec) return false;
    AMediaCodec_delete(codec);
    return true;
}

// ── Recording control ────────────────────────────────────────────────────────

bool RawVideoPipeline::start() {
    if (!ready_ || running_) return false;

    codec_ = make_encoder();
    if (!codec_) return false;
    if (AMediaCodec_start(codec_) != AMEDIA_OK) {
        LOGE("encoder start failed");
        AMediaCodec_delete(codec_);
        codec_ = nullptr;
        return false;
    }

    // Input layout. KEY_STRIDE is bytes-per-row on some encoders and pixels on
    // others; for P010 a value below width*2 can only be pixels.
    // AMediaCodec_getInputFormat is API 28; minSdk is 26, so resolve it at
    // runtime (any P010-capable device is API 33+ and has it).
    enc_stride_bytes_ = pad_w_ * 2;
    enc_slice_height_ = pad_h_;
    using GetInputFormatFn = AMediaFormat* (*)(AMediaCodec*);
    auto get_input_format = reinterpret_cast<GetInputFormatFn>(
        dlsym(RTLD_DEFAULT, "AMediaCodec_getInputFormat"));
    AMediaFormat* in_fmt = get_input_format ? get_input_format(codec_) : nullptr;
    if (in_fmt) {
        int32_t v = 0;
        if (AMediaFormat_getInt32(in_fmt, "stride", &v) && v > 0)
            enc_stride_bytes_ = (v < pad_w_ * 2) ? v * 2 : v;
        if (AMediaFormat_getInt32(in_fmt, "slice-height", &v) && v >= pad_h_)
            enc_slice_height_ = v;
        AMediaFormat_delete(in_fmt);
    }
    LOGI("encoder started: stride %d B, slice height %d", enc_stride_bytes_, enc_slice_height_);

    for (bool& b : staging_busy_) b = false;
    sent_format_   = false;
    frames_in_.store(0); frames_dropped_.store(0); frames_encoded_ = 0;
    ts_anchored_.store(false);   // re-pin the clock domain for this recording
    prof_count_ = 0; prof_gpu_ns_ = prof_copy_ns_ = prof_encwait_ns_ = prof_wall_ns_ = prof_last_ns_ = 0;
    prof_gpu_max_ns_ = prof_encwait_max_ns_ = 0;
    prof_gap_max_ns_.store(0); prof_gap_prev_ns_.store(0);
    prof_drops_base_ = 0;
    stopping_ = false;
    running_  = true;
    // A previous clip may have halted on an unreclaimable submission; the fences
    // and command buffers are rebuilt-or-idle by then, so the next clip starts
    // clean. (shutdown()/start() bracket the ring's lifetime.)
    gpu_lost_.store(false, std::memory_order_relaxed);
    // Bounded RAM backlog between the camera and the offline NLM pipeline.
    store_.init(raw_w_, raw_h_);
    if (bin2_)
        LOGI("recording start [bin2]: %dx%d, nlm=%s h=%.2f S=%s (rgb), chroma-median=%s",
             pad_w_, pad_h_,
             denoise_enabled_.load(std::memory_order_relaxed) ? "on" : "off",
             kNlmH, kUseSensorNoiseProfile ? "sensor" : "hardcoded",
             chroma_enabled_.load(std::memory_order_relaxed) ? "on" : "off");
    else
        LOGI("recording start [nlm-tiled-lds]: nlm=%s  demosaic=%s  chroma=%s",
             denoise_enabled_.load(std::memory_order_relaxed) ? "on" : "off",
             demosaic_hq_.load(std::memory_order_relaxed) ? "HQ" : "Malvar",
             chroma_enabled_.load(std::memory_order_relaxed) ? "on" : "off");
    pipeline_thread_ = std::thread(&RawVideoPipeline::pipeline_loop, this);
    drain_thread_    = std::thread(&RawVideoPipeline::drain_loop, this);
    return true;
}

void RawVideoPipeline::stop() {
    if (!running_) return;
    // Seal the store: no more input. pipeline_loop keeps popping until the whole
    // backlog is processed, then queues the encoder EOS —
    // this is the finalize drain, so the join below can take a while (by design,
    // the caller runs stop() off a background finalize thread).
    stopping_.store(true, std::memory_order_release);
    wb_cv_.notify_all();   // release pipeline_loop if it is still parked on WB
    store_.seal();
    if (pipeline_thread_.joinable()) pipeline_thread_.join();  // drains backlog, queues EOS on exit
    if (drain_thread_.joinable())    drain_thread_.join();     // exits on EOS
    running_ = false;
    store_.shutdown();

    if (codec_) {
        AMediaCodec_stop(codec_);
        AMediaCodec_delete(codec_);
        codec_ = nullptr;
    }
    LOGI("stopped: %lld frames in, %lld encoded, %lld dropped",
         static_cast<long long>(frames_in_.load()), static_cast<long long>(frames_encoded_),
         static_cast<long long>(frames_dropped_.load()));
}

void RawVideoPipeline::shutdown() {
    stop();
    if (!vk_.ok()) { ready_ = false; return; }
    VkDevice dev = vk_.device();
    vkDeviceWaitIdle(dev);
    for (auto& f : inflight_) {
        if (f.fence) { vkDestroyFence(dev, f.fence, nullptr); f.fence = VK_NULL_HANDLE; }
        vk_.destroy_buffer(f.denoised_buf);
        vk_.destroy_buffer(f.out_buf);
        vk_.destroy_buffer(f.chroma_buf);
        vk_.destroy_buffer(f.rgb_buf);
        vk_.destroy_buffer(f.chroma_buf2);
        if (f.qpool) { vkDestroyQueryPool(dev, f.qpool, nullptr); f.qpool = VK_NULL_HANDLE; }
    }
    for (auto& s : staging_) vk_.destroy_buffer(s);
    vk_.destroy_buffer(green_buf_);
    if (dpool_)    { vkDestroyDescriptorPool(dev, dpool_, nullptr); dpool_ = VK_NULL_HANDLE; }
    if (chroma_pipeline_) { vkDestroyPipeline(dev, chroma_pipeline_, nullptr); chroma_pipeline_ = VK_NULL_HANDLE; }
    if (chroma_layout_)   { vkDestroyPipelineLayout(dev, chroma_layout_, nullptr); chroma_layout_ = VK_NULL_HANDLE; }
    if (chroma_dsl_)      { vkDestroyDescriptorSetLayout(dev, chroma_dsl_, nullptr); chroma_dsl_ = VK_NULL_HANDLE; }
    if (pipeline_) { vkDestroyPipeline(dev, pipeline_, nullptr); pipeline_ = VK_NULL_HANDLE; }
    if (layout_)   { vkDestroyPipelineLayout(dev, layout_, nullptr); layout_ = VK_NULL_HANDLE; }
    if (dsl_)      { vkDestroyDescriptorSetLayout(dev, dsl_, nullptr); dsl_ = VK_NULL_HANDLE; }
    if (green_pipeline_) { vkDestroyPipeline(dev, green_pipeline_, nullptr); green_pipeline_ = VK_NULL_HANDLE; }
    if (green_layout_)   { vkDestroyPipelineLayout(dev, green_layout_, nullptr); green_layout_ = VK_NULL_HANDLE; }
    if (green_dsl_)      { vkDestroyDescriptorSetLayout(dev, green_dsl_, nullptr); green_dsl_ = VK_NULL_HANDLE; }
    if (denoise_pipeline_) { vkDestroyPipeline(dev, denoise_pipeline_, nullptr); denoise_pipeline_ = VK_NULL_HANDLE; }
    if (denoise_layout_)   { vkDestroyPipelineLayout(dev, denoise_layout_, nullptr); denoise_layout_ = VK_NULL_HANDLE; }
    if (denoise_dsl_)      { vkDestroyDescriptorSetLayout(dev, denoise_dsl_, nullptr); denoise_dsl_ = VK_NULL_HANDLE; }
    // Binned path (null on the full-res path, and vice versa — every handle is
    // nulled right after its vkDestroy so a second teardown pass skips it).
    if (nrgb_pipeline_) { vkDestroyPipeline(dev, nrgb_pipeline_, nullptr); nrgb_pipeline_ = VK_NULL_HANDLE; }
    if (nrgb_layout_)   { vkDestroyPipelineLayout(dev, nrgb_layout_, nullptr); nrgb_layout_ = VK_NULL_HANDLE; }
    if (nrgb_dsl_)      { vkDestroyDescriptorSetLayout(dev, nrgb_dsl_, nullptr); nrgb_dsl_ = VK_NULL_HANDLE; }
    if (bin_pipeline_)  { vkDestroyPipeline(dev, bin_pipeline_, nullptr); bin_pipeline_ = VK_NULL_HANDLE; }
    if (bin_layout_)    { vkDestroyPipelineLayout(dev, bin_layout_, nullptr); bin_layout_ = VK_NULL_HANDLE; }
    if (cmed_pipeline_) { vkDestroyPipeline(dev, cmed_pipeline_, nullptr); cmed_pipeline_ = VK_NULL_HANDLE; }
    if (cmed_layout_)   { vkDestroyPipelineLayout(dev, cmed_layout_, nullptr); cmed_layout_ = VK_NULL_HANDLE; }
    if (cmed_dsl_)      { vkDestroyDescriptorSetLayout(dev, cmed_dsl_, nullptr); cmed_dsl_ = VK_NULL_HANDLE; }
    if (bin_dsl_)       { vkDestroyDescriptorSetLayout(dev, bin_dsl_, nullptr); bin_dsl_ = VK_NULL_HANDLE; }
    vk_.destroy();
    ready_ = false;
}

void RawVideoPipeline::set_callbacks(FormatCb on_format, PacketCb on_packet) {
    on_format_ = std::move(on_format);
    on_packet_ = std::move(on_packet);
}

// ── Frame ingest (camera thread) ─────────────────────────────────────────────

void RawVideoPipeline::on_frame(const uint8_t* data, int w, int h,
                                int stride_bytes, int64_t ts_ns) {
    if (!running_ || stopping_) return;
    if (w != raw_w_ || h != raw_h_) {
        LOGE("unexpected RAW geometry %dx%d (want %dx%d)", w, h, raw_w_, raw_h_);
        return;
    }

    // First frame: pin the sensor clock to the audio (MONOTONIC) domain.
    if (!ts_anchored_.load(std::memory_order_relaxed)) {
        ts_offset_.store(compute_ts_offset(ts_ns), std::memory_order_relaxed);
        ts_anchored_.store(true, std::memory_order_relaxed);
    }
    ts_ns -= ts_offset_.load(std::memory_order_relaxed);

    // Worst gap between successive RAW frames arriving from the camera — a ~1Hz
    // spike here means the stall is upstream (camera/handler), not our pipeline.
    const int64_t arr = clock_ns(CLOCK_MONOTONIC);
    const int64_t gap_prev = prof_gap_prev_ns_.load(std::memory_order_relaxed);
    if (gap_prev) {
        const int64_t g = arr - gap_prev;
        if (g > prof_gap_max_ns_.load(std::memory_order_relaxed))
            prof_gap_max_ns_.store(g, std::memory_order_relaxed);
    }
    prof_gap_prev_ns_.store(arr, std::memory_order_relaxed);

    frames_in_.fetch_add(1, std::memory_order_relaxed);

    // Hand the frame to the bounded RAM store (drops the newest past the cap).
    // The store de-strides into a pooled buffer and wakes the pipeline thread.
    if (!store_.push(data, stride_bytes, ts_ns))
        frames_dropped_.fetch_add(1, std::memory_order_relaxed);
}

// ── GPU dispatch + encode (pipeline thread) ──────────────────────────────────

// Issue one logical dispatch as kDispatchBands horizontal bands, so the GPU can
// preempt between them. Same total work and same thread coordinates: with
// VK_PIPELINE_CREATE_DISPATCH_BASE_BIT, gl_WorkGroupID (and therefore
// SV_DispatchThreadID) starts at the base, so no shader is aware of the split.
// The cost is the extra launches: ~20us each, under 1% of a frame.
void RawVideoPipeline::release_staging(int slot) {
    if (slot >= 0 && slot < kSlots) staging_busy_[slot] = false;
}

// Record + submit one frame on the binned path: bin_isp (RAW16 -> half-res
// linear camera RGB) then nlm_rgb (denoise + develop + P010 pack). Two dispatches
// where the full-res path needs up to four, each over a quarter of the pixels.
// Kept separate from record_and_submit rather than branched into it: that
// function is the tuned full-res hot path and there is nothing shared between
// the two beyond the staging barrier and the submit.
bool RawVideoPipeline::record_and_submit_binned(int inflight_idx, int staging_slot) {
    InFlight& f = inflight_[inflight_idx];
    const bool dn_on = denoise_enabled_.load(std::memory_order_relaxed);
    const bool cm_on = chroma_enabled_.load(std::memory_order_relaxed);

    // Repoint the bin pass at this frame's camera-written staging buffer. Only
    // binding 0 moves; binding 1 (this slot's RGB buffer) is fixed at build.
    {
        VkDescriptorBufferInfo raw_info{ staging_[staging_slot].buf, 0, VK_WHOLE_SIZE };
        VkWriteDescriptorSet w{};
        w.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet          = f.bin_dset;
        w.dstBinding      = 0;
        w.descriptorCount = 1;
        w.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w.pBufferInfo     = &raw_info;
        vkUpdateDescriptorSets(vk_.device(), 1, &w, 0, nullptr);
    }

    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkResetCommandBuffer(f.cmd, 0);
    vkBeginCommandBuffer(f.cmd, &bi);

    // Per-pass GPU timing. The reset must be recorded before any write, and the
    // stamps use BOTTOM_OF_PIPE so each one lands after the preceding dispatch
    // has fully completed rather than when it was merely issued.
    int ts = 0;
    if (f.qpool) {
        vkCmdResetQueryPool(f.cmd, f.qpool, 0, kTimestampSlots);
        vkCmdWriteTimestamp(f.cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, f.qpool, ts++);
    }

    // Camera's host write -> shader read.
    {
        VkBufferMemoryBarrier in_b{};
        in_b.sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        in_b.srcAccessMask       = VK_ACCESS_HOST_WRITE_BIT;
        in_b.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
        in_b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        in_b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        in_b.buffer              = staging_[staging_slot].buf;
        in_b.size                = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(f.cmd, VK_PIPELINE_STAGE_HOST_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
                             1, &in_b, 0, nullptr);
    }

    // ── Bin pass: raw -> this slot's half-res linear RGB. ──
    {
        BinPush bp{};
        bp.out_w = static_cast<uint32_t>(out_w_);
        bp.out_h = static_cast<uint32_t>(out_h_);
        bp.raw_w = static_cast<uint32_t>(raw_w_);
        bp.raw_h = static_cast<uint32_t>(raw_h_);
        bp.cfa   = static_cast<uint32_t>(meta_.cfa);
        bp.white = static_cast<float>(meta_.white_level);
        for (int i = 0; i < 4; ++i) bp.black[i] = meta_.black_level[i];

        vkCmdBindPipeline(f.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, bin_pipeline_);
        vkCmdBindDescriptorSets(f.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, bin_layout_,
                                0, 1, &f.bin_dset, 0, nullptr);
        vkCmdPushConstants(f.cmd, bin_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(bp), &bp);
        // One thread per OUTPUT pixel.
        dispatch_banded(f.cmd, (static_cast<uint32_t>(out_w_) + 7) / 8,
                        (static_cast<uint32_t>(out_h_) + 7) / 8, 1);
        if (f.qpool) vkCmdWriteTimestamp(f.cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, f.qpool, ts++);

        // rgb_buf is per-slot, so this is an intra-frame dependency only — no
        // cross-slot WAR barrier is needed (that is the whole point of per-slot).
        VkBufferMemoryBarrier rgb_rd{};
        rgb_rd.sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        rgb_rd.srcAccessMask       = VK_ACCESS_SHADER_WRITE_BIT;
        rgb_rd.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
        rgb_rd.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        rgb_rd.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        rgb_rd.buffer              = f.rgb_buf.buf;
        rgb_rd.size                = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(f.cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
                             1, &rgb_rd, 0, nullptr);
    }

    // ── NLM + develop + P010 pack: linear RGB -> this slot's out_buf. ──
    {
        NlmRgbPush np{};
        // Bounds and strides are in the ENCODED (CTU-padded) domain: this pass
        // is what fills the pad, so it must run over every padded pixel.
        np.out_w          = static_cast<uint32_t>(pad_w_);
        np.out_h          = static_cast<uint32_t>(pad_h_);
        // The source it reads is only the real picture. load_rgb clamps to this,
        // which replicates the last real column/row into the pad — and also keeps
        // the padded dispatch from reading rgb_buf out of bounds.
        np.src_w          = static_cast<uint32_t>(out_w_);
        np.src_h          = static_cast<uint32_t>(out_h_);
        np.stride_pixels  = static_cast<uint32_t>(pad_w_);
        np.uv_word_offset = static_cast<uint32_t>(pad_w_) * pad_h_ / 2;  // Y uint16s / 2
        np.pad[0] = np.pad[1] = 0;
        // The noise model describes a single photosite; the binned plane is
        // quieter (see kBinNoiseScale). Passing the raw figures would over-smooth.
        // Prefer the sensor's MEASURED profile for this frame (set_noise_profile)
        // — it tracks the live ISO, which is the whole reason the denoise is
        // strong in a dark room and gentle in daylight without any control for
        // the user to set. kNoiseK/kNoiseFloor remain the fallback for a HAL that
        // reports no profile.
        const bool have_np = kUseSensorNoiseProfile &&
                             noise_valid_.load(std::memory_order_acquire);
        const float nk = have_np ? noise_s_.load(std::memory_order_relaxed) : kNoiseK;
        const float nf = have_np ? noise_o_.load(std::memory_order_relaxed) : kNoiseFloor;
        // Shot slope and read floor rescale DIFFERENTLY once the guide is white
        // balanced (see bin_noise_scale) — a single factor was only right while
        // the guide was un-gained.
        float shot_scale = kBinNoiseScale, floor_scale = kBinNoiseScale;
        bin_noise_scale(shot_scale, floor_scale);
        np.noise_k        = nk * shot_scale;
        np.noise_floor    = nf * floor_scale;
        np.h_scale        = kNlmH;
        // bit 0 = denoise, bit 1 = divert CbCr to the scratch for the median.
        np.flags          = (dn_on ? 1u : 0u) | (cm_on ? 2u : 0u);
        for (int i = 0; i < 3; ++i) np.wb[i] = wb_[i].load(std::memory_order_relaxed);
        np.wb[3] = kPqScale;
        for (int i = 0; i < 3; ++i) {
            np.ccm0[i] = ccm_[i];
            np.ccm1[i] = ccm_[3 + i];
            np.ccm2[i] = ccm_[6 + i];
        }

        vkCmdBindPipeline(f.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, nrgb_pipeline_);
        vkCmdBindDescriptorSets(f.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, nrgb_layout_,
                                0, 1, &f.nrgb_dset, 0, nullptr);
        vkCmdPushConstants(f.cmd, nrgb_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(np), &np);
        // One thread per 2x2 output quad.
        dispatch_banded(f.cmd, (static_cast<uint32_t>(pad_w_) / 2 + 7) / 8,
                        (static_cast<uint32_t>(pad_h_) / 2 + 7) / 8, 1);
        if (f.qpool) vkCmdWriteTimestamp(f.cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, f.qpool, ts++);
    }

    // ── 3x3 chroma median: scratch CbCr -> out_buf's CbCr. Luma untouched. ──
    if (cm_on) {
        // nlm_rgb wrote this slot's scratch; make it visible to the median read.
        // Both buffers are per-slot, so this is an intra-frame dependency only.
        VkBufferMemoryBarrier cm_pre{};
        cm_pre.sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        cm_pre.srcAccessMask       = VK_ACCESS_SHADER_WRITE_BIT;
        cm_pre.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
        cm_pre.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        cm_pre.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        cm_pre.buffer              = f.chroma_buf.buf;
        cm_pre.size                = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(f.cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
                             1, &cm_pre, 0, nullptr);

        const uint32_t cgx = (static_cast<uint32_t>(pad_w_) / 2 + 7) / 8;
        const uint32_t cgy = (static_cast<uint32_t>(pad_h_) / 2 + 7) / 8;

        ChromaMedianPush cm{};
        cm.out_w      = static_cast<uint32_t>(pad_w_);
        cm.out_h      = static_cast<uint32_t>(pad_h_);
        // Every scratch and the P010 CbCr plane share the same pitch: the Y
        // stride in pixels / 2, which is what nlm_rgb.slang used for its own
        // chroma store (halfStride). They stay separate fields because
        // submit_to_encoder already anticipates an encoder stride != out_w_*2.
        cm.src_stride = static_cast<uint32_t>(pad_w_) / 2;
        cm.dst_stride = static_cast<uint32_t>(pad_w_) / 2;

        vkCmdBindPipeline(f.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cmed_pipeline_);

        // Barrier between consecutive median passes: pass N's writes must be
        // visible to pass N+1's reads. Every buffer here is per-slot, so this is
        // an intra-frame dependency only and never serializes the in-flight ring.
        auto median_barrier = [&](VkBuffer buf) {
            VkBufferMemoryBarrier b{};
            b.sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            b.srcAccessMask       = VK_ACCESS_SHADER_WRITE_BIT;
            b.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
            b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.buffer              = buf;
            b.size                = VK_WHOLE_SIZE;
            vkCmdPipelineBarrier(f.cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
                                 1, &b, 0, nullptr);
        };
        auto median_pass = [&](VkDescriptorSet set, uint32_t dilation, uint32_t dst_offset) {
            cm.dilation   = dilation;
            cm.dst_offset = dst_offset;
            vkCmdBindDescriptorSets(f.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cmed_layout_,
                                    0, 1, &set, 0, nullptr);
            vkCmdPushConstants(f.cmd, cmed_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
                               0, sizeof(cm), &cm);
            dispatch_banded(f.cmd, cgx, cgy, 1);
            if (f.qpool)
                vkCmdWriteTimestamp(f.cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, f.qpool, ts++);
        };

        const uint32_t uv_off = static_cast<uint32_t>(pad_w_) * pad_h_ / 2;

        // Pass 1, spacing 1: chroma_buf -> chroma_buf2. Isolated single-site dots.
        median_pass(f.cmed_dset1, 1, 0);
        median_barrier(f.chroma_buf2.buf);

        // Pass 2, spacing 2: chroma_buf2 -> chroma_buf. This is the one that
        // clears CLUSTERED speckle — a 2x2 blob is the majority of an adjacent
        // 3x3 window and survives any number of spacing-1 passes, but the
        // spacing-2 ring lies outside it, so it is outvoted. Same 9 taps, same
        // 19 compares; only the staged tile is larger.
        median_pass(f.cmed_dset2, 2, 0);
        median_barrier(f.chroma_buf.buf);

        // Pass 3, spacing 1: chroma_buf -> the P010 CbCr plane. Resolves anything
        // pass 2 selected from two sites away against its immediate neighbours.
        median_pass(f.cd_dset, 1, uv_off);
    }
    f.ts_count = ts;

    VkBufferMemoryBarrier to_host{};
    to_host.sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    to_host.srcAccessMask       = VK_ACCESS_SHADER_WRITE_BIT;
    to_host.dstAccessMask       = VK_ACCESS_HOST_READ_BIT;
    to_host.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_host.buffer              = f.out_buf.buf;
    to_host.size                = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(f.cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &to_host,
                         0, nullptr);
    vkEndCommandBuffer(f.cmd);

    VkSubmitInfo si{};
    si.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers    = &f.cmd;
    if (vkQueueSubmit(vk_.queue(), 1, &si, f.fence) != VK_SUCCESS) {
        LOGE("vkQueueSubmit failed");
        return false;
    }
    f.submit_ns = clock_ns(CLOCK_MONOTONIC);
    return true;
}

bool RawVideoPipeline::record_and_submit(int inflight_idx, int staging_slot) {
    if (bin2_) return record_and_submit_binned(inflight_idx, staging_slot);

    InFlight& f = inflight_[inflight_idx];

    const bool dn_on = denoise_enabled_.load(std::memory_order_relaxed);
    const bool hq_on = demosaic_hq_.load(std::memory_order_relaxed);
    const bool cd_on = chroma_enabled_.load(std::memory_order_relaxed);
    // The NLM denoise pass produces denoised_buf_; when it runs, green + ISP read
    // denoised_buf_ instead of raw.
    const bool src_denoised = dn_on;

    // The Bayer input is this frame's camera-written staging buffer. Repoint
    // binding 0 of every set we'll dispatch that reads the raw buffer directly —
    // the retired slot's sets are free to update. The denoise pass reads raw;
    // without it, the green prepass (if HQ) and the ISP read raw. Sets that read
    // denoised_buf_ (green_dset_dn, dset) are fixed at build.
    {
        // Buffer infos must outlive the vkUpdateDescriptorSets call below.
        VkDescriptorBufferInfo raw_info{ staging_[staging_slot].buf, 0, VK_WHOLE_SIZE };
        VkWriteDescriptorSet ws[2]{};
        int nw = 0;
        auto add = [&](VkDescriptorSet set, uint32_t binding, const VkDescriptorBufferInfo* bi) {
            ws[nw].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            ws[nw].dstSet          = set;
            ws[nw].dstBinding      = binding;
            ws[nw].descriptorCount = 1;
            ws[nw].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            ws[nw].pBufferInfo     = bi;
            ++nw;
        };
        if (dn_on) {
            // Only the current frame is repointed; binding 1 (this slot's
            // denoised output) is fixed at build.
            add(f.denoise_dset, 0, &raw_info);
        } else {
            if (hq_on) add(f.green_dset_raw, 0, &raw_info);
            add(f.dset_raw, 0, &raw_info);
        }
        vkUpdateDescriptorSets(vk_.device(), static_cast<uint32_t>(nw), ws, 0, nullptr);
    }

    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkResetCommandBuffer(f.cmd, 0);
    vkBeginCommandBuffer(f.cmd, &bi);

    // Make the camera's host write to this staging buffer visible to the shader
    // read (replaces the staging->image upload + its transfer barriers).
    {
        VkBufferMemoryBarrier in_b{};
        in_b.sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        in_b.srcAccessMask       = VK_ACCESS_HOST_WRITE_BIT;
        in_b.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
        in_b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        in_b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        in_b.buffer              = staging_[staging_slot].buf;
        in_b.size                = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(f.cmd, VK_PIPELINE_STAGE_HOST_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
                             1, &in_b, 0, nullptr);
    }

    // ── Denoise prepass: raw buf -> denoised_buf_ (Non-Local Means, RAW domain).
    if (dn_on) {
        // No WAR barrier here: denoised_buf is per-slot, so this frame's write
        // cannot race the other in-flight slot's ISP read.
        DenoisePush dn{};
        dn.raw_w         = static_cast<uint32_t>(raw_w_);
        dn.raw_h         = static_cast<uint32_t>(raw_h_);
        dn.noise_k       = kNoiseK;
        dn.noise_floor   = kNoiseFloor;
        for (int i = 0; i < 4; ++i) dn.black[i] = meta_.black_level[i];
        dn.white         = static_cast<float>(meta_.white_level);
        dn.search_radius = static_cast<float>(kNlmSearchRadius);
        dn.patch_radius  = static_cast<float>(kNlmPatchRadius);
        dn.h_scale       = kNlmH;

        vkCmdBindPipeline(f.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, denoise_pipeline_);
        vkCmdBindDescriptorSets(f.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, denoise_layout_,
                                0, 1, &f.denoise_dset, 0, nullptr);
        vkCmdPushConstants(f.cmd, denoise_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(dn), &dn);
        // One thread per horizontal pixel PAIR -> grid x is raw_w/2.
        dispatch_banded(f.cmd, (static_cast<uint32_t>(raw_w_) / 2 + 7) / 8,
                      (static_cast<uint32_t>(raw_h_) + 7) / 8, 1);

        // denoised_buf_ written by denoise -> read by the ISP dispatch.
        VkBufferMemoryBarrier dn_rd{};
        dn_rd.sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        dn_rd.srcAccessMask       = VK_ACCESS_SHADER_WRITE_BIT;
        dn_rd.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
        dn_rd.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        dn_rd.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        dn_rd.buffer              = f.denoised_buf.buf;
        dn_rd.size                = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(f.cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
                             1, &dn_rd, 0, nullptr);
    }  // if (dn_on)

    // ── Green prepass (HQ demosaic only): raw/denoised -> shared green plane. ──
    if (hq_on) {
        // green_buf_ is shared across the in-flight ring, so this write must wait
        // for the previous frame's ISP read of it (WAR) — a COMPUTE->COMPUTE
        // execution dependency covers the hazard (mirrors the denoise WAR).
        VkBufferMemoryBarrier gn_war{};
        gn_war.sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        gn_war.srcAccessMask       = VK_ACCESS_SHADER_READ_BIT;
        gn_war.dstAccessMask       = VK_ACCESS_SHADER_WRITE_BIT;
        gn_war.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        gn_war.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        gn_war.buffer              = green_buf_.buf;
        gn_war.size                = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(f.cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
                             1, &gn_war, 0, nullptr);

        GreenPush gp{};
        gp.raw_w = static_cast<uint32_t>(raw_w_);
        gp.raw_h = static_cast<uint32_t>(raw_h_);
        gp.cfa   = static_cast<uint32_t>(meta_.cfa);
        gp.white = static_cast<float>(meta_.white_level);
        for (int i = 0; i < 4; ++i) gp.black[i] = meta_.black_level[i];

        // Green reads the same source the ISP will read (denoised when the NLM
        // or the legacy denoise produced it, else the raw staging buffer).
        VkDescriptorSet gn_set = src_denoised ? f.green_dset_dn : f.green_dset_raw;
        vkCmdBindPipeline(f.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, green_pipeline_);
        vkCmdBindDescriptorSets(f.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, green_layout_,
                                0, 1, &gn_set, 0, nullptr);
        vkCmdPushConstants(f.cmd, green_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(gp), &gp);
        // One thread per pixel (full-res float plane).
        dispatch_banded(f.cmd, (static_cast<uint32_t>(raw_w_) + 7) / 8,
                      (static_cast<uint32_t>(raw_h_) + 7) / 8, 1);

        // green_buf_ written by the green pass -> read by the ISP dispatch.
        VkBufferMemoryBarrier gn_rd{};
        gn_rd.sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        gn_rd.srcAccessMask       = VK_ACCESS_SHADER_WRITE_BIT;
        gn_rd.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
        gn_rd.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        gn_rd.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        gn_rd.buffer              = green_buf_.buf;
        gn_rd.size                = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(f.cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
                             1, &gn_rd, 0, nullptr);
    }  // if (hq_on)

    PushConstants pc{};
    pc.out_w          = static_cast<uint32_t>(pad_w_);
    pc.out_h          = static_cast<uint32_t>(pad_h_);
    pc.stride_pixels  = static_cast<uint32_t>(pad_w_);
    pc.uv_word_offset = static_cast<uint32_t>(pad_w_) * pad_h_ / 2;  // Y uint16s / 2
    pc.raw_w          = static_cast<uint32_t>(raw_w_);
    pc.raw_h          = static_cast<uint32_t>(raw_h_);
    // Low 2 bits: CFA pattern. Bit 8: demosaic mode (1=HQ directional, reads the
    // green plane the prepass just wrote; 0=Malvar, ignores it). Bit 9: chroma
    // denoise (1=divert CbCr into the scratch for the CD pass below).
    pc.cfa            = static_cast<uint32_t>(meta_.cfa) | (hq_on ? (1u << 8) : 0u)
                                                         | (cd_on ? (1u << 9) : 0u);
    pc.white          = static_cast<float>(meta_.white_level);
    for (int i = 0; i < 4; ++i) pc.black[i] = meta_.black_level[i];
    for (int i = 0; i < 3; ++i) pc.wb[i] = wb_[i].load(std::memory_order_relaxed);
    pc.wb[3] = kPqScale;
    for (int i = 0; i < 3; ++i) {
        pc.ccm0[i] = ccm_[i];
        pc.ccm1[i] = ccm_[3 + i];
        pc.ccm2[i] = ccm_[6 + i];
    }

    VkDescriptorSet isp_set = src_denoised ? f.dset : f.dset_raw;
    vkCmdBindPipeline(f.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
    vkCmdBindDescriptorSets(f.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout_,
                            0, 1, &isp_set, 0, nullptr);
    vkCmdPushConstants(f.cmd, layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    dispatch_banded(f.cmd, (static_cast<uint32_t>(pad_w_) / 2 + 7) / 8,
                  (static_cast<uint32_t>(pad_h_) / 2 + 7) / 8, 1);

    // ── Chroma denoise (CD): out_buf luma + noisy chroma scratch -> out_buf CbCr.
    if (cd_on) {
        // The ISP wrote out_buf's luma and diverted the noisy chroma into this
        // slot's scratch. Make both visible to the CD pass, which reads luma (for
        // edge guidance) + scratch chroma and writes the cleaned CbCr into out_buf.
        // Both are per-slot, so this is an intra-frame dependency only (no
        // cross-frame WAR like the shared denoised/green/hist buffers).
        VkBufferMemoryBarrier cd_pre[2]{};
        const VkBuffer cd_bufs[2] = { f.out_buf.buf, f.chroma_buf.buf };
        for (int i = 0; i < 2; ++i) {
            cd_pre[i].sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            cd_pre[i].srcAccessMask       = VK_ACCESS_SHADER_WRITE_BIT;
            cd_pre[i].dstAccessMask       = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            cd_pre[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            cd_pre[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            cd_pre[i].buffer              = cd_bufs[i];
            cd_pre[i].size                = VK_WHOLE_SIZE;
        }
        vkCmdPipelineBarrier(f.cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
                             2, cd_pre, 0, nullptr);

        ChromaPush cp{};
        cp.out_w          = static_cast<uint32_t>(pad_w_);
        cp.out_h          = static_cast<uint32_t>(pad_h_);
        cp.stride_pixels  = static_cast<uint32_t>(pad_w_);
        cp.uv_word_offset = static_cast<uint32_t>(pad_w_) * pad_h_ / 2;
        cp.chroma_stride  = static_cast<uint32_t>(pad_w_) / 2;
        cp.radius         = kChromaRadius;
        cp.sigma_s        = kChromaSigmaS;
        cp.sigma_l        = kChromaSigmaL;

        vkCmdBindPipeline(f.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, chroma_pipeline_);
        vkCmdBindDescriptorSets(f.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, chroma_layout_,
                                0, 1, &f.cd_dset, 0, nullptr);
        vkCmdPushConstants(f.cmd, chroma_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(cp), &cp);
        // One thread per chroma site (one per 2x2 luma quad).
        dispatch_banded(f.cmd, (static_cast<uint32_t>(pad_w_) / 2 + 7) / 8,
                      (static_cast<uint32_t>(pad_h_) / 2 + 7) / 8, 1);
    }

    VkBufferMemoryBarrier to_host{};
    to_host.sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    to_host.srcAccessMask       = VK_ACCESS_SHADER_WRITE_BIT;
    to_host.dstAccessMask       = VK_ACCESS_HOST_READ_BIT;
    to_host.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_host.buffer              = f.out_buf.buf;
    to_host.size                = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(f.cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &to_host,
                         0, nullptr);
    vkEndCommandBuffer(f.cmd);

    VkSubmitInfo si{};
    si.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers    = &f.cmd;
    if (vkQueueSubmit(vk_.queue(), 1, &si, f.fence) != VK_SUCCESS) {
        LOGE("vkQueueSubmit failed");
        return false;
    }
    f.submit_ns = clock_ns(CLOCK_MONOTONIC);
    return true;
}

// Block until the given in-flight slot's GPU work finishes. Profiles total GPU
// latency (submit -> complete) so the per-window MAX still exposes ~1Hz spikes;
// under healthy overlap this wait is near-zero (the GPU finished while the CPU
// was encoding the previous frame).
bool RawVideoPipeline::retire(int inflight_idx) {
    InFlight& f = inflight_[inflight_idx];
    VkResult wr = vkWaitForFences(vk_.device(), 1, &f.fence, VK_TRUE, 2'000'000'000ull);
    const int64_t gpu_dt = clock_ns(CLOCK_MONOTONIC) - f.submit_ns;
    prof_gpu_ns_ += gpu_dt;
    if (gpu_dt > prof_gpu_max_ns_) prof_gpu_max_ns_ = gpu_dt;

    if (wr == VK_TIMEOUT) {
        // 2 s is already far past any plausible frame, but a severely throttled
        // GPU is slow, not broken — so give it one long grace period rather than
        // abandoning the submission. Abandoning is the dangerous option: the
        // command buffer stays PENDING and the fence stays in use, and the ring
        // reuses this slot two frames later, where vkResetCommandBuffer +
        // vkQueueSubmit on still-pending objects is undefined behaviour that
        // faults or wedges the queue. Never return to that state.
        LOGE("ISP dispatch fence timeout after 2s — waiting out the GPU");
        wr = vkWaitForFences(vk_.device(), 1, &f.fence, VK_TRUE, 8'000'000'000ull);
    }

    if (wr != VK_SUCCESS) {
        // Still not done (or the device is lost). The submission can never be
        // safely reclaimed, so this slot — and the ring with it — is finished.
        // Do NOT reset the fence: resetting a pending fence is the same UB.
        // pipeline_loop sees gpu_lost_ and stops developing, so no slot is ever
        // reused; the clip finalizes with whatever was already encoded. Losing
        // the tail of a recording is acceptable; corrupting the queue is not.
        LOGE("ISP GPU unrecoverable (VkResult %d) — halting develop, keeping the clip", wr);
        gpu_lost_.store(true, std::memory_order_release);
        return false;
    }
    vkResetFences(vk_.device(), 1, &f.fence);

    // Real per-pass GPU time. The fence is signalled, so the results are ready
    // and this never blocks. Unlike `gpu` (submit -> fence, which pipeline_loop's
    // blocking pop pins to the camera period), these are the actual cost and are
    // what any future quality increase must be budgeted against.
    if (f.qpool && f.ts_count >= 2) {
        uint64_t stamps[kTimestampSlots]{};
        if (vkGetQueryPoolResults(vk_.device(), f.qpool, 0, static_cast<uint32_t>(f.ts_count),
                                  sizeof(stamps), stamps, sizeof(uint64_t),
                                  VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            const double ns_per_tick = vk_.timestamp_period_ns();
            for (int i = 0; i + 1 < f.ts_count; ++i) {
                // Ticks wrap at timestampValidBits; a negative delta means a wrap
                // straddled this frame, so drop it rather than log a wild number.
                if (stamps[i + 1] < stamps[i]) continue;
                prof_pass_ns_[i] += llround((stamps[i + 1] - stamps[i]) * ns_per_tick);
            }
            prof_pass_used_ = f.ts_count - 1;
        }
    }
    return true;
}

bool RawVideoPipeline::submit_to_encoder(const VkCompute::Buffer& out, int64_t ts_ns) {
    // Time the wait for a free encoder input buffer — this is where frames stall
    // when the encoder can't keep up (the real 30fps bottleneck).
    const int64_t deq_t0 = clock_ns(CLOCK_MONOTONIC);
    ssize_t idx = AMediaCodec_dequeueInputBuffer(codec_, 100'000);  // 100 ms
    const int64_t deq_dt = clock_ns(CLOCK_MONOTONIC) - deq_t0;
    prof_encwait_ns_ += deq_dt;
    if (deq_dt > prof_encwait_max_ns_) prof_encwait_max_ns_ = deq_dt;
    if (idx < 0) { frames_dropped_.fetch_add(1, std::memory_order_relaxed); return false; }

    size_t cap = 0;
    uint8_t* dst = AMediaCodec_getInputBuffer(codec_, static_cast<size_t>(idx), &cap);
    const size_t need = static_cast<size_t>(enc_stride_bytes_) * enc_slice_height_ +
                        static_cast<size_t>(enc_stride_bytes_) * (pad_h_ / 2);
    if (!dst || cap < need) {
        LOGE("input buffer too small (%zu < %zu)", cap, need);
        AMediaCodec_queueInputBuffer(codec_, static_cast<size_t>(idx), 0, 0, 0, 0);
        return false;
    }

    const int64_t copy_t0 = clock_ns(CLOCK_MONOTONIC);
    const auto* src = static_cast<const uint8_t*>(out.mapped);
    const int tight = pad_w_ * 2;
    if (enc_stride_bytes_ == tight && enc_slice_height_ == pad_h_) {
        std::memcpy(dst, src, static_cast<size_t>(tight) * pad_h_ * 3 / 2);
    } else {
        for (int y = 0; y < pad_h_; ++y)
            std::memcpy(dst + static_cast<size_t>(y) * enc_stride_bytes_,
                        src + static_cast<size_t>(y) * tight, tight);
        uint8_t* dst_uv = dst + static_cast<size_t>(enc_stride_bytes_) * enc_slice_height_;
        const uint8_t* src_uv = src + static_cast<size_t>(tight) * pad_h_;
        for (int y = 0; y < pad_h_ / 2; ++y)
            std::memcpy(dst_uv + static_cast<size_t>(y) * enc_stride_bytes_,
                        src_uv + static_cast<size_t>(y) * tight, tight);
    }
    prof_copy_ns_ += clock_ns(CLOCK_MONOTONIC) - copy_t0;

    AMediaCodec_queueInputBuffer(codec_, static_cast<size_t>(idx), 0, need,
                                 ts_ns / 1000, 0);
    ++frames_encoded_;

    // Rolling profile: flush per-stage averages + measured fps every window.
    const int64_t now = clock_ns(CLOCK_MONOTONIC);
    if (prof_last_ns_ != 0) prof_wall_ns_ += now - prof_last_ns_;
    prof_last_ns_ = now;
    if (++prof_count_ >= kProfileWindow) {
        double fps = prof_wall_ns_ > 0 ? 1e9 * prof_count_ / double(prof_wall_ns_) : 0.0;
        long long drops = static_cast<long long>(frames_dropped_.load(std::memory_order_relaxed) - prof_drops_base_);
        LOGI("%.1f fps | gpu %.1f/%.0fms  enc-wait %.1f/%.0fms  cam-gap %.0fms  copy %.1fms  drops %lld | nlm=%s dm=%s cd=%s bin=%s | S=%.5f O=%.6f",
             fps,
             prof_gpu_ns_ / 1e6 / prof_count_, prof_gpu_max_ns_ / 1e6,
             prof_encwait_ns_ / 1e6 / prof_count_, prof_encwait_max_ns_ / 1e6,
             prof_gap_max_ns_.load(std::memory_order_relaxed) / 1e6,
             prof_copy_ns_ / 1e6 / prof_count_, drops,
             denoise_enabled_.load(std::memory_order_relaxed) ? "on" : "off",
             demosaic_hq_.load(std::memory_order_relaxed) ? "HQ" : "Malvar",
             chroma_enabled_.load(std::memory_order_relaxed) ? "on" : "off",
             bin2_ ? "on" : "off",
             // The LIVE noise model, not the one latched at clip start: it tracks
             // ISO, so a scene getting darker mid-take changes the denoise
             // strength. Logging it only on first publish (as it was) hid exactly
             // the variable the over/under-filtering analysis depends on.
             noise_valid_.load(std::memory_order_acquire)
                 ? noise_s_.load(std::memory_order_relaxed) : kNoiseK,
             noise_valid_.load(std::memory_order_acquire)
                 ? noise_o_.load(std::memory_order_relaxed) : kNoiseFloor);
        // Real GPU cost per pass. `gpu` above saturates at the camera period
        // whenever the ISP keeps up, so THIS is the line to read when deciding
        // whether there is room for more work.
        if (gpu_timing_ && prof_pass_used_ > 0) {
            char buf[192]; int n = 0;
            static const char* kPassName[] = { "bin", "nlm", "med1", "med2", "med3", "p6" };
            double total = 0.0;
            for (int i = 0; i < prof_pass_used_ && n < (int)sizeof(buf) - 24; ++i) {
                const double ms = prof_pass_ns_[i] / 1e6 / prof_count_;
                total += ms;
                n += snprintf(buf + n, sizeof(buf) - n, " %s %.2f", kPassName[i], ms);
            }
            LOGI("  gpu-real:%s  = %.2f ms of %.1f ms budget (%.0f%% used)",
                 buf, total, 1000.0 / fps_, 100.0 * total * fps_ / 1000.0);
        }
        for (auto& v : prof_pass_ns_) v = 0;

        prof_count_ = 0; prof_gpu_ns_ = prof_copy_ns_ = prof_encwait_ns_ = prof_wall_ns_ = 0;
        prof_gpu_max_ns_ = prof_encwait_max_ns_ = 0;
        prof_gap_max_ns_.store(0, std::memory_order_relaxed);
        prof_drops_base_ = frames_dropped_.load(std::memory_order_relaxed);
    }
    return true;
}

void RawVideoPipeline::pipeline_loop() {
    // Keep this heavy ~33ms-cadence thread on the SoC's fast cluster so the
    // scheduler can't migrate it onto a little core mid-recording (a ~1Hz hitch).
    cpuaff::pin_current_thread_to_fast_cores("RawVideo");
    cpuaff::raise_current_thread_priority(-10);

    // The NLM is spatial-only, so a staging slot is needed exactly as long as the
    // GPU job reading it is in flight: it is released when that job retires. An
    // earlier revision held each slot for two extra frames to feed a temporal
    // window that the shader never read, which pinned 2 of the 5 slots for
    // nothing. GPU compute for frame N overlaps with CPU readback+encode of frame
    // N-1 via the kInFlight-deep in-flight ring. Runs during AND after capture;
    // exits once the store is sealed and fully drained (the finalize tail).
    const size_t frame_bytes = static_cast<size_t>(raw_w_) * raw_h_ * 2;

    // In-flight ring rotation: submit frame N's GPU work, then retire+encode
    // frame N-1 while the GPU runs. This overlaps GPU compute with CPU
    // readback+encode, nearly doubling throughput vs the serial path.
    int cur = 0;            // current in-flight ring slot
    int prev = -1;          // previous slot with pending GPU work (-1 = none)

    for (;;) {
        // Develop nothing until white balance is valid, else the clip opens on a
        // green (1/1/1 gain) cast. Frames keep buffering in the store while we
        // wait. Bail out if the recording stops first — otherwise a clip that
        // never received a neutral would park this thread forever.
        if (!wb_valid_.load(std::memory_order_acquire)) {
            std::unique_lock<std::mutex> lk(wb_wait_mtx_);
            wb_cv_.wait(lk, [this] {
                return wb_valid_.load(std::memory_order_acquire) ||
                       stopping_.load(std::memory_order_acquire);
            });
            if (!wb_valid_.load(std::memory_order_acquire)) {
                LOGE("stopped before any white balance arrived — no frames developed");
                break;
            }
        }

        int sslot = -1;
        for (int i = 0; i < kSlots; ++i) if (!staging_busy_[i]) { sslot = i; break; }
        if (sslot < 0) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); continue; }  // unreachable

        FrameStore::Frame fr;
        if (!store_.pop(fr)) break;             // sealed + fully drained
        std::memcpy(staging_[sslot].mapped, fr.data.data(), frame_bytes);
        const int64_t ts = fr.ts_ns;
        store_.reclaim(std::move(fr));
        staging_busy_[sslot] = true;

        // Submit this frame's GPU work on the current in-flight slot.
        inflight_[cur].staging_slot = sslot;
        inflight_[cur].ts_ns        = ts;
        bool submitted = record_and_submit(cur, sslot);

        // Retire + encode the PREVIOUS frame while this one's GPU work runs.
        // This is where the overlap happens: the GPU computes frame N while the
        // CPU reads back + encodes frame N-1.
        if (prev >= 0) {
            if (retire(prev))
                submit_to_encoder(inflight_[prev].out_buf, inflight_[prev].ts_ns);
            else
                frames_dropped_.fetch_add(1, std::memory_order_relaxed);
            // The GPU is done with that job's input, so its staging slot is free.
            release_staging(inflight_[prev].staging_slot);
        }

        // A lost GPU means the just-retired slot's submission is unreclaimable.
        // Break BEFORE the ring rotates onto a slot whose fence/command buffer may
        // still be pending — that reuse is the undefined behaviour retire() guards.
        if (gpu_lost_.load(std::memory_order_acquire)) {
            if (submitted) { prev = cur; }   // leave it unretired; teardown waits
            break;
        }

        if (submitted) {
            prev = cur;
            cur  = (cur + 1) % kInFlight;
        } else {
            // Nothing was queued, so nothing will ever read this slot.
            release_staging(sslot);
            prev = -1;
            frames_dropped_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // Drain the last in-flight frame that hasn't been retired yet. Skipped on a
    // lost GPU: that wait already failed once and would only stall the stop path.
    if (prev >= 0 && !gpu_lost_.load(std::memory_order_acquire)) {
        if (retire(prev))
            submit_to_encoder(inflight_[prev].out_buf, inflight_[prev].ts_ns);
        release_staging(inflight_[prev].staging_slot);
    }

    // Flush: signal end-of-stream so the drain thread sees its final packets.
    for (int attempt = 0; attempt < 10; ++attempt) {
        ssize_t idx = AMediaCodec_dequeueInputBuffer(codec_, 100'000);
        if (idx >= 0) {
            AMediaCodec_queueInputBuffer(codec_, static_cast<size_t>(idx), 0, 0, 0,
                                         AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
            return;
        }
    }
    LOGE("could not queue EOS — drain thread will time out");
}

void RawVideoPipeline::drain_loop() {
    // Encoder-output drain shares the fast cluster so packet handoff to the muxer
    // stays responsive and doesn't get parked on a little core.
    cpuaff::pin_current_thread_to_fast_cores("RawVideo");

    AMediaCodecBufferInfo info{};
    // Generous timeout per dequeue; the loop exits on EOS or after the encoder
    // goes silent post-stop.
    int idle = 0;
    for (;;) {
        ssize_t idx = AMediaCodec_dequeueOutputBuffer(codec_, &info, 10'000);
        if (idx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            AMediaFormat* fmt = AMediaCodec_getOutputFormat(codec_);
            void* csd = nullptr; size_t csd_size = 0;
            if (fmt && AMediaFormat_getBuffer(fmt, "csd-0", &csd, &csd_size) &&
                csd && csd_size > 0 && !sent_format_ && on_format_) {
                on_format_(static_cast<const uint8_t*>(csd),
                           static_cast<int>(csd_size), pad_w_, pad_h_);
                sent_format_ = true;
            }
            if (fmt) AMediaFormat_delete(fmt);
        } else if (idx >= 0) {
            size_t cap = 0;
            uint8_t* buf = AMediaCodec_getOutputBuffer(codec_, static_cast<size_t>(idx), &cap);
            if (buf && info.size > 0) {
                const uint8_t* data = buf + info.offset;
                if (info.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG) {
                    if (!sent_format_ && on_format_) {
                        on_format_(data, info.size, pad_w_, pad_h_);
                        sent_format_ = true;
                    }
                } else if (on_packet_) {
                    bool key = (info.flags & 1) != 0;  // BUFFER_FLAG_KEY_FRAME
                    on_packet_(data, info.size, info.presentationTimeUs, key);
                }
            }
            AMediaCodec_releaseOutputBuffer(codec_, static_cast<size_t>(idx), false);
            if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) break;
            idle = 0;
        } else {
            // Timed out. Give up if we're stopping and nothing is coming.
            if (stopping_ && ++idle > 300) { LOGE("drain timeout without EOS"); break; }
        }
    }
}

} // namespace isp
