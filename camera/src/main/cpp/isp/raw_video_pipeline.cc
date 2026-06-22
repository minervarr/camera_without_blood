#include "raw_video_pipeline.hh"

#include <media/NdkMediaFormat.h>

#include <dlfcn.h>
#include <time.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
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

// Must match the PushConstants block in temporal_denoise.slang (80 bytes). Each
// 16-byte group maps to a Slang vec4 slot (std140-safe — no bare vec3).
struct TemporalPush {
    uint32_t raw_w, raw_h;   // rawDim
    float    noise_k;
    float    noise_floor;
    float    black[4];       // black level per 2x2 CFA position
    float    params[4];      // x=white, y=staticSigma, z=temporalMax, w=dtFactor
    float    params2[4];     // x=motionSigma, yzw=pad
    uint32_t flags[4];       // x=histValid, yzw=pad
};
static_assert(sizeof(TemporalPush) == 80, "temporal push constant layout drift");

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
    {
        std::lock_guard<std::mutex> lk(wb_mtx_);
        for (int i = 0; i < 3; ++i)
            wb_[i] = (neutral[i] > 1e-6f) ? 1.0f / neutral[i] : 1.0f;
    }
    if (!wb_valid_.exchange(true))
        LOGI("WB gains %.3f %.3f %.3f (valid)", wb_[0], wb_[1], wb_[2]);
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
                            int raw_w, int raw_h, int fps) {
    if (ready_) return true;
    meta_  = meta;
    raw_w_ = raw_w;
    raw_h_ = raw_h;
    fps_   = fps > 0 ? fps : 30;
    out_w_ = raw_w & ~1;
    out_h_ = raw_h & ~1;
    // Perceptual-bpp target from the real output geometry, clamped under the
    // HEVC L6.0 High-tier ceiling (see kTargetBpp/kMaxBitrate above).
    int64_t br = llround(kTargetBpp * out_w_ * out_h_ * fps_);
    bitrate_ = static_cast<int>(br < kMaxBitrate ? br : kMaxBitrate);

    if (meta_.has_neutral) {
        float n[3] = { static_cast<float>(meta_.as_shot_neutral[0]),
                       static_cast<float>(meta_.as_shot_neutral[1]),
                       static_cast<float>(meta_.as_shot_neutral[2]) };
        set_neutral(n);
    }
    derive_color_matrix();

    // ── AI Engine Setup ──────────────────────────────────────────────────────
    // DISABLED: AiDenoiser is dormant scaffolding — its run() below is commented
    // out, so init() does nothing useful. Worse, it brings up ncnn's OpenMP
    // runtime (libomp), which is process-global and NOT re-init-safe: the second
    // time android_main runs in the same process (the camera scene relaunched by
    // an embedding host), __kmp_parallel_initialize hits a debug assert and
    // aborts the whole process. Keep it off until AiDenoiser is actually wired in
    // (and OpenMP re-entry is solved). See ai_denoiser_.run() below.
    // ai_denoiser_.init(assets);

    if (!probe_encoder()) {
        LOGE("no P010 HEVC Main10 encoder — RAW video unavailable");
        return false;
    }
    if (!build_vulkan(assets)) {
        LOGE("Vulkan ISP setup failed — RAW video unavailable");
        return false;
    }
    ready_ = true;
    LOGI("RAW pipeline ready: %dx%d Bayer -> %dx%d P010 PQ @ %d Mbps (buffer input)",
         raw_w_, raw_h_, out_w_, out_h_, bitrate_ / 1'000'000);
    return true;
}

// Builds one compute pipeline (descriptor-set layout + pipeline layout +
// pipeline) from a SPIR-V asset. `bindings`/`binding_count` describe the set;
// `push_size` is the push-constant block size.
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
    VkResult pr = vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, nullptr, &pipeline);
    vkDestroyShaderModule(dev, shader, nullptr);
    if (pr != VK_SUCCESS) { LOGE("compute pipeline creation failed for %s", spv); return false; }
    return true;
}

bool RawVideoPipeline::build_vulkan(AAssetManager* assets) {
    if (!vk_.init()) return false;
    VkDevice dev = vk_.device();

    // All bindings are storage buffers now (raw + denoised + P010 are all
    // uint16/uint32-packed buffers — no images, no upload).
    // ── Denoise pass (spatio-temporal NLM): binding 0 = current frame, 1 =
    // denoised out, 2/3 = the kTemporalPast past frames (causal temporal window).
    // nlm_bayer.slang searches patches across all of them; downstream is unchanged.
    VkDescriptorSetLayoutBinding dn_bind[2 + kTemporalPast]{};
    for (uint32_t i = 0; i < 2 + kTemporalPast; ++i)
        dn_bind[i] = { i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    if (!build_compute(vk_, assets, "shaders/nlm_bayer.spv", dn_bind, 2 + kTemporalPast,
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

    // ── Temporal pass: 0 = raw, 1 = hist read, 2 = denoised out, 3 = hist write. ──
    VkDescriptorSetLayoutBinding td_bind[4]{};
    td_bind[0] = { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    td_bind[1] = { 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    td_bind[2] = { 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    td_bind[3] = { 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    if (!build_compute(vk_, assets, "shaders/temporal_denoise.spv", td_bind, 4,
                       sizeof(TemporalPush), temporal_dsl_, temporal_layout_, temporal_pipeline_))
        return false;

    // 8 sets per in-flight slot: denoise(2 + kTemporalPast bufs), green-from-raw(2),
    // green-from-denoised(2), ISP-from-denoised(4), ISP-from-raw(4), temporal-A(4),
    // temporal-B(4), chroma-denoise(2).
    VkDescriptorPoolSize sizes[1] = {
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, (24 + kTemporalPast) * kInFlight },
    };
    VkDescriptorPoolCreateInfo dpci{};
    dpci.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.maxSets       = 8 * kInFlight;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes    = sizes;
    if (vkCreateDescriptorPool(dev, &dpci, nullptr, &dpool_) != VK_SUCCESS) return false;

    // RAW16 buffers are uint16-packed (2 px per uint32): raw_w*raw_h*2 bytes.
    VkDeviceSize raw_bytes = static_cast<VkDeviceSize>(raw_w_) * raw_h_ * 2;

    // Shared denoised intermediate (device-local; denoise off by default).
    if (!vk_.create_buffer(raw_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, denoised_buf_)) {
        LOGE("denoised buffer creation failed");
        return false;
    }

    // Shared green plane (device-local; one float per pixel; HQ demosaic only).
    VkDeviceSize green_bytes = static_cast<VkDeviceSize>(raw_w_) * raw_h_ * 4;
    if (!vk_.create_buffer(green_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, green_buf_)) {
        LOGE("green buffer creation failed");
        return false;
    }

    // Temporal denoise history (device-local; same RAW16 layout). Two buffers
    // ping-pong: each frame reads the previous estimate and writes the next.
    for (int i = 0; i < 2; ++i) {
        if (!vk_.create_buffer(raw_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, hist_[i])) {
            LOGE("temporal history buffer %d creation failed", i);
            return false;
        }
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

    VkDeviceSize p010_bytes = static_cast<VkDeviceSize>(out_w_) * out_h_ * 3;  // Y + UV/2, 2B each
    bool out_cached = true;
    // Fixed buffer infos referenced by every slot's writes (binding 0 of the raw
    // sets is a placeholder repointed per frame in record_and_submit).
    VkDescriptorBufferInfo dn_info   { denoised_buf_.buf, 0, VK_WHOLE_SIZE };
    VkDescriptorBufferInfo green_info{ green_buf_.buf,    0, VK_WHOLE_SIZE };
    VkDescriptorBufferInfo raw0_info { staging_[0].buf,   0, VK_WHOLE_SIZE };
    VkDescriptorBufferInfo hist0_info{ hist_[0].buf,      0, VK_WHOLE_SIZE };
    VkDescriptorBufferInfo hist1_info{ hist_[1].buf,      0, VK_WHOLE_SIZE };
    for (int k = 0; k < kInFlight; ++k) {
        InFlight& f = inflight_[k];

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
        VkDeviceSize chroma_bytes = static_cast<VkDeviceSize>(out_w_) * out_h_;
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
        dsai.pSetLayouts        = &temporal_dsl_;
        if (vkAllocateDescriptorSets(dev, &dsai, &f.temporal_dset_a) != VK_SUCCESS) return false;
        dsai.pSetLayouts        = &temporal_dsl_;
        if (vkAllocateDescriptorSets(dev, &dsai, &f.temporal_dset_b) != VK_SUCCESS) return false;
        dsai.pSetLayouts        = &chroma_dsl_;
        if (vkAllocateDescriptorSets(dev, &dsai, &f.cd_dset) != VK_SUCCESS) return false;

        VkDescriptorBufferInfo out_info{ f.out_buf.buf,    0, VK_WHOLE_SIZE };
        VkDescriptorBufferInfo chroma_info{ f.chroma_buf.buf, 0, VK_WHOLE_SIZE };

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
        VkWriteDescriptorSet writes[16] = {
            wbuf(f.denoise_dset,   0, &raw0_info),    // current frame (placeholder)
            wbuf(f.denoise_dset,   1, &dn_info),      // denoised out
            wbuf(f.denoise_dset,   2, &raw0_info),    // past frame 0 (placeholder)
            wbuf(f.denoise_dset,   3, &raw0_info),    // past frame 1 (placeholder)
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
        vkUpdateDescriptorSets(dev, 16, writes, 0, nullptr);

        // Chroma denoise set: reads out_buf (luma guidance) + this slot's chroma
        // scratch; writes the cleaned CbCr back into out_buf.
        VkWriteDescriptorSet cd_writes[2] = {
            wbuf(f.cd_dset, 0, &out_info),     // P010 out (RW: read Y, write CbCr)
            wbuf(f.cd_dset, 1, &chroma_info),  // noisy chroma scratch (read)
        };
        vkUpdateDescriptorSets(dev, 2, cd_writes, 0, nullptr);

        // Temporal parity sets. Binding 0 (raw) is a placeholder repointed at the
        // live staging slot each frame; binding 2 is the shared denoised output.
        // The hist pair swaps read/write roles between the two sets.
        VkWriteDescriptorSet td_writes[8] = {
            wbuf(f.temporal_dset_a, 0, &raw0_info),  // raw (placeholder)
            wbuf(f.temporal_dset_a, 1, &hist0_info), // hist read  (prev)
            wbuf(f.temporal_dset_a, 2, &dn_info),    // denoised out
            wbuf(f.temporal_dset_a, 3, &hist1_info), // hist write (next)
            wbuf(f.temporal_dset_b, 0, &raw0_info),  // raw (placeholder)
            wbuf(f.temporal_dset_b, 1, &hist1_info), // hist read  (prev)
            wbuf(f.temporal_dset_b, 2, &dn_info),    // denoised out
            wbuf(f.temporal_dset_b, 3, &hist0_info), // hist write (next)
        };
        vkUpdateDescriptorSets(dev, 8, td_writes, 0, nullptr);

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
    AMediaFormat_setInt32(fmt, "width",  out_w_);
    AMediaFormat_setInt32(fmt, "height", out_h_);
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

bool RawVideoPipeline::start(const std::string& spill_dir) {
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
    enc_stride_bytes_ = out_w_ * 2;
    enc_slice_height_ = out_h_;
    using GetInputFormatFn = AMediaFormat* (*)(AMediaCodec*);
    auto get_input_format = reinterpret_cast<GetInputFormatFn>(
        dlsym(RTLD_DEFAULT, "AMediaCodec_getInputFormat"));
    AMediaFormat* in_fmt = get_input_format ? get_input_format(codec_) : nullptr;
    if (in_fmt) {
        int32_t v = 0;
        if (AMediaFormat_getInt32(in_fmt, "stride", &v) && v > 0)
            enc_stride_bytes_ = (v < out_w_ * 2) ? v * 2 : v;
        if (AMediaFormat_getInt32(in_fmt, "slice-height", &v) && v >= out_h_)
            enc_slice_height_ = v;
        AMediaFormat_delete(in_fmt);
    }
    LOGI("encoder started: stride %d B, slice height %d", enc_stride_bytes_, enc_slice_height_);

    for (bool& b : staging_busy_) b = false;
    sent_format_   = false;
    frames_in_ = frames_dropped_ = frames_encoded_ = 0;
    ts_anchored_ = false;   // re-pin the clock domain for this recording
    wb_warned_   = false;
    temporal_frame_idx_ = 0;  // fresh temporal history each recording (first frame passes through)
    temporal_prev_ts_   = 0;
    prof_count_ = 0; prof_gpu_ns_ = prof_copy_ns_ = prof_encwait_ns_ = prof_wall_ns_ = prof_last_ns_ = 0;
    prof_gpu_max_ns_ = prof_encwait_max_ns_ = prof_gap_max_ns_ = prof_gap_prev_ns_ = 0;
    prof_drops_base_ = 0;
    stopping_ = false;
    running_  = true;
    // Never-drop RAM+disk buffer between the camera and the offline NLM pipeline.
    // Created only now that the encoder is up, so an encoder failure above can't
    // leave the store's I/O thread running.
    store_.init(raw_w_, raw_h_, kFrameStoreBudget, spill_dir);
    LOGI("recording start: temporal=%s  nlm=%s  demosaic=%s  chroma=%s",
         temporal_enabled_.load(std::memory_order_relaxed) ? "on" : "off",
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
    // backlog (RAM + any disk spill) is processed, then queues the encoder EOS —
    // this is the finalize drain, so the join below can take a while (by design,
    // the caller runs stop() off a background finalize thread).
    stopping_ = true;
    store_.seal();
    if (pipeline_thread_.joinable()) pipeline_thread_.join();  // drains backlog, queues EOS on exit
    if (drain_thread_.joinable())    drain_thread_.join();     // exits on EOS
    running_ = false;
    store_.shutdown();   // join the store I/O thread, delete any spill chunks

    if (codec_) {
        AMediaCodec_stop(codec_);
        AMediaCodec_delete(codec_);
        codec_ = nullptr;
    }
    LOGI("stopped: %lld frames in, %lld encoded, %lld dropped",
         static_cast<long long>(frames_in_), static_cast<long long>(frames_encoded_),
         static_cast<long long>(frames_dropped_));
}

void RawVideoPipeline::shutdown() {
    stop();
    if (!vk_.ok()) { ready_ = false; return; }
    VkDevice dev = vk_.device();
    vkDeviceWaitIdle(dev);
    for (auto& f : inflight_) {
        if (f.fence) { vkDestroyFence(dev, f.fence, nullptr); f.fence = VK_NULL_HANDLE; }
        vk_.destroy_buffer(f.out_buf);
        vk_.destroy_buffer(f.chroma_buf);
    }
    for (auto& s : staging_) vk_.destroy_buffer(s);
    vk_.destroy_buffer(denoised_buf_);
    vk_.destroy_buffer(green_buf_);
    for (auto& hb : hist_) vk_.destroy_buffer(hb);
    if (dpool_)    { vkDestroyDescriptorPool(dev, dpool_, nullptr); dpool_ = VK_NULL_HANDLE; }
    if (chroma_pipeline_) { vkDestroyPipeline(dev, chroma_pipeline_, nullptr); chroma_pipeline_ = VK_NULL_HANDLE; }
    if (chroma_layout_)   { vkDestroyPipelineLayout(dev, chroma_layout_, nullptr); chroma_layout_ = VK_NULL_HANDLE; }
    if (chroma_dsl_)      { vkDestroyDescriptorSetLayout(dev, chroma_dsl_, nullptr); chroma_dsl_ = VK_NULL_HANDLE; }
    if (temporal_pipeline_) { vkDestroyPipeline(dev, temporal_pipeline_, nullptr); temporal_pipeline_ = VK_NULL_HANDLE; }
    if (temporal_layout_)   { vkDestroyPipelineLayout(dev, temporal_layout_, nullptr); temporal_layout_ = VK_NULL_HANDLE; }
    if (temporal_dsl_)      { vkDestroyDescriptorSetLayout(dev, temporal_dsl_, nullptr); temporal_dsl_ = VK_NULL_HANDLE; }
    if (pipeline_) { vkDestroyPipeline(dev, pipeline_, nullptr); pipeline_ = VK_NULL_HANDLE; }
    if (layout_)   { vkDestroyPipelineLayout(dev, layout_, nullptr); layout_ = VK_NULL_HANDLE; }
    if (dsl_)      { vkDestroyDescriptorSetLayout(dev, dsl_, nullptr); dsl_ = VK_NULL_HANDLE; }
    if (green_pipeline_) { vkDestroyPipeline(dev, green_pipeline_, nullptr); green_pipeline_ = VK_NULL_HANDLE; }
    if (green_layout_)   { vkDestroyPipelineLayout(dev, green_layout_, nullptr); green_layout_ = VK_NULL_HANDLE; }
    if (green_dsl_)      { vkDestroyDescriptorSetLayout(dev, green_dsl_, nullptr); green_dsl_ = VK_NULL_HANDLE; }
    if (denoise_pipeline_) { vkDestroyPipeline(dev, denoise_pipeline_, nullptr); denoise_pipeline_ = VK_NULL_HANDLE; }
    if (denoise_layout_)   { vkDestroyPipelineLayout(dev, denoise_layout_, nullptr); denoise_layout_ = VK_NULL_HANDLE; }
    if (denoise_dsl_)      { vkDestroyDescriptorSetLayout(dev, denoise_dsl_, nullptr); denoise_dsl_ = VK_NULL_HANDLE; }
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
    if (!ts_anchored_) {
        ts_offset_   = compute_ts_offset(ts_ns);
        ts_anchored_ = true;
    }
    ts_ns -= ts_offset_;

    // Worst gap between successive RAW frames arriving from the camera — a ~1Hz
    // spike here means the stall is upstream (camera/handler), not our pipeline.
    const int64_t arr = clock_ns(CLOCK_MONOTONIC);
    if (prof_gap_prev_ns_) {
        int64_t g = arr - prof_gap_prev_ns_;
        if (g > prof_gap_max_ns_) prof_gap_max_ns_ = g;
    }
    prof_gap_prev_ns_ = arr;

    ++frames_in_;

    // Hand the frame to the never-drop store (RAM, spilling to disk on overflow).
    // The store de-strides into a pooled buffer and wakes the pipeline thread.
    if (!store_.push(data, stride_bytes, ts_ns)) ++frames_dropped_;
}

// ── GPU dispatch + encode (pipeline thread) ──────────────────────────────────

bool RawVideoPipeline::record_and_submit(int inflight_idx, int staging_slot,
                                         const int* past_slots) {
    InFlight& f = inflight_[inflight_idx];

    const bool dn_on = denoise_enabled_.load(std::memory_order_relaxed);
    const bool hq_on = demosaic_hq_.load(std::memory_order_relaxed);
    const bool td_on = temporal_enabled_.load(std::memory_order_relaxed);
    const bool cd_on = chroma_enabled_.load(std::memory_order_relaxed);
    // Both the temporal pass and the legacy denoise pass produce denoised_buf_;
    // when either runs, green + ISP read denoised_buf_ instead of raw. Temporal
    // takes precedence (it owns denoised_buf_ and the standalone denoise is skipped).
    const bool src_denoised = td_on || dn_on;
    // Active hist ping-pong set for this frame (parity by frame index).
    VkDescriptorSet td_set = (temporal_frame_idx_ & 1) ? f.temporal_dset_b
                                                       : f.temporal_dset_a;

    // The Bayer input is this frame's camera-written staging buffer. Repoint
    // binding 0 of every set we'll dispatch that reads the raw buffer directly —
    // the retired slot's sets are free to update. The temporal/denoise pass reads
    // raw; with neither, the green prepass (if HQ) and the ISP read raw. Sets that
    // read denoised_buf_ (green_dset_dn, dset) are fixed at build.
    {
        // Buffer infos must outlive the vkUpdateDescriptorSets call below.
        VkDescriptorBufferInfo raw_info{ staging_[staging_slot].buf, 0, VK_WHOLE_SIZE };
        VkDescriptorBufferInfo past_info[kTemporalPast];
        for (int k = 0; k < kTemporalPast; ++k)
            past_info[k] = VkDescriptorBufferInfo{ staging_[past_slots[k]].buf, 0, VK_WHOLE_SIZE };

        VkWriteDescriptorSet ws[1 + kTemporalPast]{};
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
        if (td_on) {
            add(td_set, 0, &raw_info);
        } else if (dn_on) {
            // Spatio-temporal NLM: current frame (binding 0) + the past window
            // (bindings 2..). Binding 1 (denoised out) is fixed at build.
            add(f.denoise_dset, 0, &raw_info);
            for (int k = 0; k < kTemporalPast; ++k)
                add(f.denoise_dset, static_cast<uint32_t>(2 + k), &past_info[k]);
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

    // ── AI Denoise (ncnn) on RAW Bayer data ──────────────────────────────────
    // In-place processing via the AI Denoiser wrapper.
    // ai_denoiser_.run(reinterpret_cast<uint16_t*>(staging_[staging_slot].mapped), raw_w_, raw_h_);

    // ── Temporal denoise: raw + prev hist -> denoised_buf_ + next hist. ──
    if (td_on) {
        // denoised_buf_ (prev frame's ISP read) and the two hist buffers (prev
        // frame's temporal read/write) are all shared across the in-flight ring,
        // so this frame's writes/reads must follow the previous frame's on the
        // same queue. One COMPUTE->COMPUTE barrier over all three covers the
        // hazards (WAR on denoised_buf_ + hist_write, RAW on hist_read).
        VkBufferMemoryBarrier td_war[3]{};
        const VkBuffer td_bufs[3] = { denoised_buf_.buf, hist_[0].buf, hist_[1].buf };
        for (int i = 0; i < 3; ++i) {
            td_war[i].sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            td_war[i].srcAccessMask       = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            td_war[i].dstAccessMask       = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            td_war[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            td_war[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            td_war[i].buffer              = td_bufs[i];
            td_war[i].size                = VK_WHOLE_SIZE;
        }
        vkCmdPipelineBarrier(f.cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
                             3, td_war, 0, nullptr);

        // Dropped-frame trust: if more time than nominal elapsed since the last
        // frame, the scene may have moved more, so lower the temporal retention.
        float dt_factor = 1.0f;
        const int64_t ts = inflight_[inflight_idx].ts_ns;   // set by pipeline_loop before this call
        if (temporal_frame_idx_ > 0 && temporal_prev_ts_ > 0 && ts > temporal_prev_ts_) {
            const double nominal = 1e9 / (fps_ > 0 ? fps_ : 30);
            const double dt = static_cast<double>(ts - temporal_prev_ts_);
            dt_factor = static_cast<float>(std::min(1.0, std::max(0.5, nominal / dt)));
        }
        temporal_prev_ts_ = ts;

        TemporalPush tp{};
        tp.raw_w       = static_cast<uint32_t>(raw_w_);
        tp.raw_h       = static_cast<uint32_t>(raw_h_);
        tp.noise_k     = kNoiseK;
        tp.noise_floor = kNoiseFloor;
        for (int i = 0; i < 4; ++i) tp.black[i] = meta_.black_level[i];
        tp.params[0]  = static_cast<float>(meta_.white_level);  // white
        tp.params[1]  = kStaticSigma;
        tp.params[2]  = kTemporalMax;
        tp.params[3]  = dt_factor;
        tp.params2[0] = kMotionSigma;
        tp.flags[0]   = (temporal_frame_idx_ > 0) ? 1u : 0u;    // histValid

        vkCmdBindPipeline(f.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, temporal_pipeline_);
        vkCmdBindDescriptorSets(f.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, temporal_layout_,
                                0, 1, &td_set, 0, nullptr);
        vkCmdPushConstants(f.cmd, temporal_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(tp), &tp);
        // One thread per horizontal pixel PAIR -> grid x is raw_w/2.
        vkCmdDispatch(f.cmd, (static_cast<uint32_t>(raw_w_) / 2 + 7) / 8,
                      (static_cast<uint32_t>(raw_h_) + 7) / 8, 1);
        ++temporal_frame_idx_;

        // denoised_buf_ written by the temporal pass -> read by green/ISP.
        VkBufferMemoryBarrier td_rd{};
        td_rd.sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        td_rd.srcAccessMask       = VK_ACCESS_SHADER_WRITE_BIT;
        td_rd.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
        td_rd.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        td_rd.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        td_rd.buffer              = denoised_buf_.buf;
        td_rd.size                = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(f.cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
                             1, &td_rd, 0, nullptr);
    }  // if (td_on)

    // ── Denoise prepass: raw buf -> denoised_buf_ (Non-Local Means, RAW domain).
    // Skipped while temporal is on (temporal already owns denoised_buf_).
    if (dn_on && !td_on) {
        // denoised_buf_ is shared across the in-flight ring, so this write must
        // wait for the previous frame's ISP read of it (WAR) — a COMPUTE->COMPUTE
        // execution dependency covers the hazard.
        VkBufferMemoryBarrier dn_war{};
        dn_war.sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        dn_war.srcAccessMask       = VK_ACCESS_SHADER_READ_BIT;
        dn_war.dstAccessMask       = VK_ACCESS_SHADER_WRITE_BIT;
        dn_war.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        dn_war.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        dn_war.buffer              = denoised_buf_.buf;
        dn_war.size                = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(f.cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
                             1, &dn_war, 0, nullptr);

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
        vkCmdDispatch(f.cmd, (static_cast<uint32_t>(raw_w_) / 2 + 7) / 8,
                      (static_cast<uint32_t>(raw_h_) + 7) / 8, 1);

        // denoised_buf_ written by denoise -> read by the ISP dispatch.
        VkBufferMemoryBarrier dn_rd{};
        dn_rd.sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        dn_rd.srcAccessMask       = VK_ACCESS_SHADER_WRITE_BIT;
        dn_rd.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
        dn_rd.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        dn_rd.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        dn_rd.buffer              = denoised_buf_.buf;
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

        // Green reads the same source the ISP will read (denoised when temporal
        // or the legacy denoise produced it, else the raw staging buffer).
        VkDescriptorSet gn_set = src_denoised ? f.green_dset_dn : f.green_dset_raw;
        vkCmdBindPipeline(f.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, green_pipeline_);
        vkCmdBindDescriptorSets(f.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, green_layout_,
                                0, 1, &gn_set, 0, nullptr);
        vkCmdPushConstants(f.cmd, green_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(gp), &gp);
        // One thread per pixel (full-res float plane).
        vkCmdDispatch(f.cmd, (static_cast<uint32_t>(raw_w_) + 7) / 8,
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
    pc.out_w          = static_cast<uint32_t>(out_w_);
    pc.out_h          = static_cast<uint32_t>(out_h_);
    pc.stride_pixels  = static_cast<uint32_t>(out_w_);
    pc.uv_word_offset = static_cast<uint32_t>(out_w_) * out_h_ / 2;  // Y uint16s / 2
    pc.raw_w          = static_cast<uint32_t>(raw_w_);
    pc.raw_h          = static_cast<uint32_t>(raw_h_);
    // Low 2 bits: CFA pattern. Bit 8: demosaic mode (1=HQ directional, reads the
    // green plane the prepass just wrote; 0=Malvar, ignores it). Bit 9: chroma
    // denoise (1=divert CbCr into the scratch for the CD pass below).
    pc.cfa            = static_cast<uint32_t>(meta_.cfa) | (hq_on ? (1u << 8) : 0u)
                                                         | (cd_on ? (1u << 9) : 0u);
    pc.white          = static_cast<float>(meta_.white_level);
    for (int i = 0; i < 4; ++i) pc.black[i] = meta_.black_level[i];
    {
        std::lock_guard<std::mutex> lk(wb_mtx_);
        for (int i = 0; i < 3; ++i) pc.wb[i] = wb_[i];
    }
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
    vkCmdDispatch(f.cmd, (static_cast<uint32_t>(out_w_) / 2 + 7) / 8,
                  (static_cast<uint32_t>(out_h_) / 2 + 7) / 8, 1);

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
        cp.out_w          = static_cast<uint32_t>(out_w_);
        cp.out_h          = static_cast<uint32_t>(out_h_);
        cp.stride_pixels  = static_cast<uint32_t>(out_w_);
        cp.uv_word_offset = static_cast<uint32_t>(out_w_) * out_h_ / 2;
        cp.chroma_stride  = static_cast<uint32_t>(out_w_) / 2;
        cp.radius         = kChromaRadius;
        cp.sigma_s        = kChromaSigmaS;
        cp.sigma_l        = kChromaSigmaL;

        vkCmdBindPipeline(f.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, chroma_pipeline_);
        vkCmdBindDescriptorSets(f.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, chroma_layout_,
                                0, 1, &f.cd_dset, 0, nullptr);
        vkCmdPushConstants(f.cmd, chroma_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(cp), &cp);
        // One thread per chroma site (one per 2x2 luma quad).
        vkCmdDispatch(f.cmd, (static_cast<uint32_t>(out_w_) / 2 + 7) / 8,
                      (static_cast<uint32_t>(out_h_) / 2 + 7) / 8, 1);
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
    vkResetFences(vk_.device(), 1, &f.fence);
    const int64_t gpu_dt = clock_ns(CLOCK_MONOTONIC) - f.submit_ns;
    prof_gpu_ns_ += gpu_dt;
    if (gpu_dt > prof_gpu_max_ns_) prof_gpu_max_ns_ = gpu_dt;
    if (wr != VK_SUCCESS) { LOGE("ISP dispatch fence timeout"); return false; }
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
    if (idx < 0) { ++frames_dropped_; return false; }

    size_t cap = 0;
    uint8_t* dst = AMediaCodec_getInputBuffer(codec_, static_cast<size_t>(idx), &cap);
    const size_t need = static_cast<size_t>(enc_stride_bytes_) * enc_slice_height_ +
                        static_cast<size_t>(enc_stride_bytes_) * (out_h_ / 2);
    if (!dst || cap < need) {
        LOGE("input buffer too small (%zu < %zu)", cap, need);
        AMediaCodec_queueInputBuffer(codec_, static_cast<size_t>(idx), 0, 0, 0, 0);
        return false;
    }

    const int64_t copy_t0 = clock_ns(CLOCK_MONOTONIC);
    const auto* src = static_cast<const uint8_t*>(out.mapped);
    const int tight = out_w_ * 2;
    if (enc_stride_bytes_ == tight && enc_slice_height_ == out_h_) {
        std::memcpy(dst, src, static_cast<size_t>(tight) * out_h_ * 3 / 2);
    } else {
        for (int y = 0; y < out_h_; ++y)
            std::memcpy(dst + static_cast<size_t>(y) * enc_stride_bytes_,
                        src + static_cast<size_t>(y) * tight, tight);
        uint8_t* dst_uv = dst + static_cast<size_t>(enc_stride_bytes_) * enc_slice_height_;
        const uint8_t* src_uv = src + static_cast<size_t>(tight) * out_h_;
        for (int y = 0; y < out_h_ / 2; ++y)
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
        long long drops = static_cast<long long>(frames_dropped_ - prof_drops_base_);
        LOGI("%.1f fps | gpu %.1f/%.0fms  enc-wait %.1f/%.0fms  cam-gap %.0fms  copy %.1fms  drops %lld | td=%s nlm=%s dm=%s cd=%s",
             fps,
             prof_gpu_ns_ / 1e6 / prof_count_, prof_gpu_max_ns_ / 1e6,
             prof_encwait_ns_ / 1e6 / prof_count_, prof_encwait_max_ns_ / 1e6,
             prof_gap_max_ns_ / 1e6,
             prof_copy_ns_ / 1e6 / prof_count_, drops,
             temporal_enabled_.load(std::memory_order_relaxed) ? "on" : "off",
             denoise_enabled_.load(std::memory_order_relaxed) ? "on" : "off",
             demosaic_hq_.load(std::memory_order_relaxed) ? "HQ" : "Malvar",
             chroma_enabled_.load(std::memory_order_relaxed) ? "on" : "off");
        prof_count_ = 0; prof_gpu_ns_ = prof_copy_ns_ = prof_encwait_ns_ = prof_wall_ns_ = 0;
        prof_gpu_max_ns_ = prof_encwait_max_ns_ = prof_gap_max_ns_ = 0;
        prof_drops_base_ = frames_dropped_;
    }
    return true;
}

void RawVideoPipeline::pipeline_loop() {
    // Keep this heavy ~33ms-cadence thread on the SoC's fast cluster so the
    // scheduler can't migrate it onto a little core mid-recording (a ~1Hz hitch).
    cpuaff::pin_current_thread_to_fast_cores("RawVideo");
    cpuaff::raise_current_thread_priority(-10);

    // Spatio-temporal NLM needs a causal window: the current (centre) frame is
    // denoised using itself + the kTemporalPast most recent frames as extra patch
    // sources. The window's staging slots stay resident: a slot stays alive for
    // kTemporalPast more frames after being the centre, then is freed as it ages
    // out. GPU compute for frame N overlaps with CPU readback+encode of frame N-1
    // via the kInFlight-deep in-flight ring. Runs during AND after capture; exits
    // once the store is sealed and fully drained (the finalize tail).
    const size_t frame_bytes = static_cast<size_t>(raw_w_) * raw_h_ * 2;
    int hist[kTemporalPast];                    // staging slots of recent past frames (hist[0]=t-1)
    for (int k = 0; k < kTemporalPast; ++k) hist[k] = -1;

    // In-flight ring rotation: submit frame N's GPU work, then retire+encode
    // frame N-1 while the GPU runs. This overlaps GPU compute with CPU
    // readback+encode, nearly doubling throughput vs the serial path.
    int cur = 0;            // current in-flight ring slot
    int prev = -1;          // previous slot with pending GPU work (-1 = none)

    for (;;) {
        // Develop nothing until white balance is valid, else the clip opens on a
        // green (1/1/1 gain) cast. Frames keep buffering in the store while we wait.
        if (!wb_valid_.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
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

        // Past slots, clamped to the newest available at the clip's start.
        int past[kTemporalPast];
        for (int k = 0; k < kTemporalPast; ++k)
            past[k] = (hist[k] >= 0) ? hist[k] : (k == 0 ? sslot : past[k - 1]);

        // Submit this frame's GPU work on the current in-flight slot.
        inflight_[cur].staging_slot = sslot;
        inflight_[cur].ts_ns        = ts;
        bool submitted = record_and_submit(cur, sslot, past);

        // Retire + encode the PREVIOUS frame while this one's GPU work runs.
        // This is where the overlap happens: the GPU computes frame N while the
        // CPU reads back + encodes frame N-1.
        if (prev >= 0) {
            if (retire(prev))
                submit_to_encoder(inflight_[prev].out_buf, inflight_[prev].ts_ns);
            else
                ++frames_dropped_;
        }

        if (submitted) {
            prev = cur;
            cur  = (cur + 1) % kInFlight;
        } else {
            prev = -1;
            ++frames_dropped_;
        }

        // Slide the window: the oldest past slot ages out and is freed (the GPU
        // is done with it after retire()); the centre becomes hist[0].
        int aged = hist[kTemporalPast - 1];
        if (aged >= 0 && aged != sslot) staging_busy_[aged] = false;
        for (int k = kTemporalPast - 1; k > 0; --k) hist[k] = hist[k - 1];
        hist[0] = sslot;
    }

    // Drain the last in-flight frame that hasn't been retired yet.
    if (prev >= 0) {
        if (retire(prev))
            submit_to_encoder(inflight_[prev].out_buf, inflight_[prev].ts_ns);
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
                           static_cast<int>(csd_size), out_w_, out_h_);
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
                        on_format_(data, info.size, out_w_, out_h_);
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
