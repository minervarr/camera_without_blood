#pragma once

#include <android/asset_manager.h>
#include <media/NdkMediaCodec.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "../camera/dng_writer.hh"
#include "vk_compute.hh"
#include "frame_store.hh"

namespace isp {

// The native RAW video path: RAW16 Bayer frames in, encoded HEVC Main10
// packets out. Owns a Vulkan compute ISP (debayer_isp.slang: Malvar demosaic,
// white balance, CCM to BT.2020, PQ ST 2084, P010) and an AMediaCodec HEVC
// encoder fed P010 byte buffers. PTS is the camera sensor timestamp — exact,
// no surface-clock guessing. No DynamicRangeProfiles anywhere.
//
// Lifecycle: init() once (Vulkan + encoder probe); start()/stop() per
// recording (fresh encoder each time for clean per-file csd); on_frame() is
// called from the camera's Java thread and never blocks — it copies into a
// free staging slot or drops the frame.
class RawVideoPipeline {
public:
    using FormatCb = std::function<void(const uint8_t* csd, int len, int w, int h)>;
    using PacketCb = std::function<void(const uint8_t* data, int len,
                                        int64_t pts_us, bool key)>;

    RawVideoPipeline()  = default;
    ~RawVideoPipeline() { shutdown(); }

    // Builds the Vulkan ISP for the given RAW geometry and verifies the device
    // has a P010-capable HEVC Main10 encoder. Returns false if anything is
    // missing — caller falls back to the legacy path.
    // `bin2` selects the half-resolution binned path: the ISP bins each 2x2 CFA
    // quad to one output pixel (bin_isp.slang) and denoises + develops that
    // quarter-size plane (nlm_rgb.slang), instead of denoising and demosaicing at
    // full sensor resolution. Output geometry becomes raw/2 in each axis. This is
    // what makes 30 fps survive GPU thermal throttling on a long take — see
    // kBinNoiseScale and the comment on bin2_ below.
    bool init(AAssetManager* assets, const dng::DngMeta& meta,
              int raw_w, int raw_h, int fps, bool bin2);
    bool ready() const { return ready_; }

    // The ENCODED picture size — out_* rounded up to the encoder's CTU grid.
    // That is what the muxer and the format callback must report, because it is
    // what the bitstream actually contains.
    int  out_width()  const { return pad_w_; }
    int  out_height() const { return pad_h_; }
    // The real, un-padded picture. Nothing is cropped away from it; pad_* only
    // ever adds. See kCtuAlign in the .cc.
    int  src_width()  const { return out_w_; }
    int  src_height() const { return out_h_; }
    int  fps()        const { return fps_; }

    void set_callbacks(FormatCb on_format, PacketCb on_packet);

    // Per-clip white balance (camera-space neutral from the capture result).
    void set_neutral(const float neutral[3]);

    // The sensor's measured noise model for the current frame
    // (ACAMERA_SENSOR_NOISE_PROFILE: variance = S*x + O at normalised signal x).
    // Called per frame from the camera thread; makes the denoise strength track
    // the real ISO instead of the compiled-in kNoiseK/kNoiseFloor guesses. Safe
    // to never call — the constants remain the fallback.
    void set_noise_profile(const double np[8], int count);

    // Toggles the RAW-domain spatial denoise prepass — now Non-Local Means
    // (nlm_bayer.slang), on by default — that fills denoised_buf_ for the demosaic
    // (A/B comparison; effective next frame).
    void set_denoise(bool on) { denoise_enabled_.store(on, std::memory_order_relaxed); }

    // True when running the half-resolution binned path (see init()).
    bool binned() const { return bin2_; }

    // Toggles the HQ (RCD-style directional) demosaic vs the default Malvar
    // (A/B comparison; effective next frame). HQ adds the green-plane prepass.
    // Ignored on the binned path: there is no demosaic there to switch, and the
    // green prepass pipeline is never created.
    void set_demosaic_hq(bool on) {
        if (bin2_) return;
        demosaic_hq_.store(on, std::memory_order_relaxed);
    }

    // Toggles the chroma denoise (CD) post-ISP pass (A/B comparison; effective
    // next frame). Purely spatial and chroma-only: the luma plane is byte-for-byte
    // identical to the CD-off path, so it never smears motion and never touches
    // luminance detail. Orthogonal to the upstream denoise/demosaic toggles.
    // Full-res path: the luma-guided chroma bilateral (chroma_denoise.slang).
    // Binned path: the 3x3 chroma MEDIAN (chroma_median.slang), which is a
    // different filter with different guarantees — it selects a neighbouring
    // value rather than blending, so it cannot produce the coloured edge halo a
    // bilateral can. On by default there; see init(). Retained as a switch so a
    // regression can be bisected without a rebuild.
    void set_chroma(bool on) { chroma_enabled_.store(on, std::memory_order_relaxed); }

    bool start();                              // fresh encoder + worker threads + frame store
    void stop();    // seal the store, drain the backlog, EOS flush, join threads, release encoder

    // One RAW16 Bayer frame (camera thread). Copies into the bounded RAM frame
    // store and returns immediately — the offline NLM pipeline drains the store
    // during AND after capture.
    void on_frame(const uint8_t* data, int w, int h, int stride_bytes, int64_t ts_ns);

    // Finalize progress (frames accepted vs. processed). After stop() seals the
    // store these converge; the UI shows done/total as a percentage.
    uint64_t backlog_total() const { return store_.pushed(); }
    uint64_t backlog_done()  const { return store_.popped(); }

    void shutdown();  // full teardown (Vulkan + encoder)

private:
    // Causal temporal window: the spatio-temporal NLM (nlm_bayer.slang) denoises
    // the current frame using this many recent PAST frames as extra patch sources.
    // MUST match kPast in nlm_bayer.slang (and the past-frame binding count).
    // Retained only to size kSlots; the pipeline is spatial-only and reads no
    // past frames (see nlm_bayer.slang).
    static constexpr int kTemporalPast = 2;

    // GPU staging buffers: the working set the pipeline copies frames into from the
    // FrameStore. Must hold the temporal window (current + kTemporalPast past) plus
    // a spare to pop the next frame into.
    static constexpr int kSlots = kTemporalPast + 3;

    bool build_vulkan(AAssetManager* assets);
    bool build_vulkan_binned(VkDevice dev);   // buffers/descriptors for bin2_
    bool probe_encoder();
    AMediaCodec* make_encoder();           // create+configure+start, or null
    void derive_color_matrix();            // DngMeta -> sensor->BT.2020 CCM
    void pipeline_loop();
    void drain_loop();
    // Record + submit one frame's GPU work (upload + optional denoise + ISP) into
    // the given in-flight slot; does NOT wait — returns right after the submit so
    // the CPU can encode an already-finished frame while this one runs.
    bool record_and_submit(int inflight_idx, int staging_slot);
    // The bin2_ variant: bin_isp then nlm_rgb, two dispatches at quarter area.
    bool record_and_submit_binned(int inflight_idx, int staging_slot);
    void release_staging(int slot);
    // Block until an in-flight slot's GPU work completes (profiles GPU latency).
    bool retire(int inflight_idx);
    // P010 readback (from the given slot's output buffer) -> codec input.
    bool submit_to_encoder(const VkCompute::Buffer& out, int64_t ts_ns);
    // Pins the sensor-timestamp clock domain to the audio domain
    // (CLOCK_MONOTONIC) on the first frame of a recording; returns the fixed
    // per-recording offset to subtract from every sensor timestamp.
    int64_t compute_ts_offset(int64_t sensor_ts);

    // ── Configuration ────────────────────────────────────────────────────────
    dng::DngMeta meta_{};
    int  raw_w_ = 0, raw_h_ = 0;
    int  out_w_ = 0, out_h_ = 0;
    // out_* rounded UP to a whole number of 64x64 HEVC coding tree units. An
    // unaligned picture forces the encoder to code partial CTUs at the right and
    // bottom edges and signal a conformance window; the reconstructed pad bleeds
    // into the visible edge, which is the streaked ~1 mm border this removes.
    // The pad is filled by replicating the last real column/row, so no sensor
    // field of view is lost and nothing is cropped.
    int  pad_w_ = 0, pad_h_ = 0;
    int  fps_   = 30;
    int  bitrate_ = 0;
    bool ready_  = false;
    // Half-resolution binned path (bin_isp + nlm_rgb) instead of the full-res
    // NLM + demosaic chain. Set once in init() and read on the pipeline thread
    // and from the toggles, never written after — so it needs no atomic.
    bool bin2_  = false;

    float ccm_[9]    = {1, 0, 0, 0, 1, 0, 0, 0, 1};  // sensor -> BT.2020
    // WB gains: written per capture result (camera thread), read once per frame
    // while recording the command buffer (pipeline thread). Lock-free on purpose
    // — a mutex here sat in the per-frame hot path. The three components are
    // independent atomics: a reader can at worst mix two adjacent neutrals,
    // which is imperceptible (they differ by far less than a quantization step).
    std::atomic<float> wb_[3]{1.0f, 1.0f, 1.0f};
    std::atomic<bool>  wb_valid_{false};  // set once a real neutral has arrived
    // Live sensor noise model (see set_noise_profile). Written on the camera
    // thread, read on the pipeline thread — independent relaxed atomics, since a
    // reader that mixes two adjacent frames' S and O is harmless (they move
    // together and only scale the denoise weight).
    std::atomic<float> noise_s_{0.0f}, noise_o_{0.0f};
    std::atomic<bool>  noise_valid_{false};
    // pipeline_loop parks here until the first neutral arrives (or the recording
    // stops) instead of polling — a stop with no WB ever delivered used to spin
    // the thread forever and deadlock the join in stop().
    std::mutex              wb_wait_mtx_;
    std::condition_variable wb_cv_;
    // Set when a submission could not be reclaimed (fence timeout past the grace
    // period, or device lost). Latched, never cleared during a recording: once an
    // in-flight slot holds a pending command buffer and fence, reusing it is
    // undefined behaviour, so pipeline_loop stops developing rather than rotate
    // onto it. Cleared by start() for the next clip. See retire().
    std::atomic<bool> gpu_lost_{false};
    // ON. Justified by the PICTURE measurement on a fixed scene (shadow luma
    // noise 13.0 -> 7.1, mid-tone 8.9 -> 2.3), NOT by encoder bitrate: an earlier
    // claim that the denoise halves the encoder's load was retracted, because the
    // two clips behind it were different scenes. See docs/DORMANT_METHODS.md.
    //
    // Separately established and still true: this device's encoder has a hard
    // ~254 Mbps ceiling and ignores any request above it, so encoder quality is
    // NOT tunable through kTargetBppBinned.
    std::atomic<bool> denoise_enabled_{true};
    // DISABLED: High-Quality (HQ) Demosaic and Chroma Denoise are turned off.
    // Reason: Running HQ demosaic and Chroma Denoise simultaneously requires massive GPU compute (~57ms per frame).
    // While it runs at 30fps initially when the phone is cool, the Android OS thermally throttles
    // the GPU after a few minutes of recording. When throttled, the frame processing time exceeds 33.3ms,
    // which drops the framerate to ~15fps. Disabling these guarantees a stable 30fps for long shoots.
    // false = Malvar-He-Cutler (single 5x5 pass), true = RCD-style directional
    // (adds the green prepass). NOT "nearest neighbour vs bilinear" — both paths
    // are gradient-corrected; HQ just spends a whole extra full-res pass on the
    // green plane before reconstructing R/B in colour-difference space.
    std::atomic<bool> demosaic_hq_{false};
    // Likewise dormant — init() no longer force-enables it on the binned path.
    std::atomic<bool> chroma_enabled_{false};
    // HEVC coding-tree-unit size. The encoded picture is rounded UP to a
    // multiple of this so the encoder never has to code a partial CTU and never
    // signals a conformance window — see the derivation in init(). 64 is the
    // largest (and the usual) CTU size; rounding to it is also correct for any
    // encoder that picks 32 or 16, since 64 is a multiple of both.
    // DORMANT — see docs/DORMANT_METHODS.md. Set kCtuPadEncode true to round the
    // encoded picture up to whole 64x64 CTUs (2040x1530 -> 2048x1536), filling the
    // surplus by replicating the last real column/row.
    //
    // Why it is OFF: it was built to remove a streaked ~1 mm border on the right
    // and bottom edges, on the theory that partial CTUs plus an HEVC conformance
    // window (which crops only from the right and bottom) were responsible. The
    // theory did NOT survive the device test — the border was still visible with
    // the pad in place. Worse, the pad is measurably its own artifact: the encoder
    // spends few bits on the flat replicated strip, so after lossy coding it no
    // longer matches the column it was copied from (measured 1.21-1.26x interior
    // gradient against 1.09-1.16x for the adjacent real columns), which reads as a
    // thin band in exactly the place the original complaint describes.
    //
    // Keeping it at 1 means no padding: the encoded picture is the real picture.
    static constexpr bool kCtuPadEncode = false;
    static constexpr int  kCtuAlign     = kCtuPadEncode ? 64 : 1;

    static constexpr float kPqScale = 0.10f;  // linear 1.0 -> 1000 nits

    // Guide weights — MUST match kLumaW in nlm_rgb.slang. The noise model below is
    // derived from these, so the two only stay consistent together.
    static constexpr float kLumaW[3] = { 0.25f, 0.65f, 0.10f };

    // Fallback noise rescale for the binned plane, used only when no white
    // balance has arrived yet; see bin_noise_scale() for the real derivation.
    // 0.28 = 0.25^2 + 0.65^2/2 + 0.10^2, i.e. the unity-gain case.
    static constexpr float kBinNoiseScale = 0.28f;

    // Converts the sensor's per-photosite noise model (variance = S*x + O at
    // normalised signal x) into the model for the plane nlm_rgb.slang actually
    // measures: the WHITE-BALANCED guide luma over 2x2-binned RGB.
    //
    // Two separate factors fall out, which is why a single scale was wrong once
    // the guide became gained (see nlm_rgb.slang's tile fill):
    //   A = sum(w_i^2 * g_i^2 * v_i)  — variance of the gained guide, where
    //       v_i = 1 for R and B (single photosites, variance unchanged by the
    //       bin) and 1/2 for G (the quad's two greens are averaged).
    //   B = sum(w_i * g_i)            — how much the gains scale the guide's
    //       SIGNAL, which the shot term is proportional to.
    // Since var = A*(S*c + O) while the shader sees centre = c*B, substituting
    // c = centre/B gives var = (S*A/B)*centre + (O*A). So the shot slope scales
    // by A/B and the read floor by A — different numbers, ~0.37 and ~0.49 at
    // typical gains, not the single 0.28 the un-gained guide used.
    void bin_noise_scale(float& shot, float& floor_) const {
        const float g[3] = { wb_[0].load(std::memory_order_relaxed),
                             wb_[1].load(std::memory_order_relaxed),
                             wb_[2].load(std::memory_order_relaxed) };
        const float var_after_bin[3] = { 1.0f, 0.5f, 1.0f };   // R, G (2 averaged), B
        float A = 0.0f, B = 0.0f;
        for (int i = 0; i < 3; ++i) {
            A += kLumaW[i] * kLumaW[i] * g[i] * g[i] * var_after_bin[i];
            B += kLumaW[i] * g[i];
        }
        if (!(A > 0.0f) || !(B > 0.0f)) { shot = kBinNoiseScale; floor_ = kBinNoiseScale; return; }
        shot   = A / B;
        floor_ = A;
    }

    // Use the sensor's measured ACAMERA_SENSOR_NOISE_PROFILE instead of the
    // compiled-in kNoiseK/kNoiseFloor below. Measured on the S23 Ultra, the
    // constants overstate variance by ~30x (shot) and ~141x (read floor), which
    // made h^2 so large that every NLM candidate scored a weight of ~1 — the
    // "edge-preserving" NLM was in fact a plain 3x3 box blur. The profile also
    // tracks live ISO, which is what makes the denoise automatically strong in
    // the dark and gentle in daylight. Set false to A/B against the old guesses.
    static constexpr bool  kUseSensorNoiseProfile = true;

    // Bayer-domain denoise tuning (normalized [0,1] units, bit-depth agnostic).
    // Shot noise scales with sqrt(signal); the floor covers read noise. Raise
    // kNoiseK for more smoothing if the encoder still fights sensor grain.
    static constexpr float kNoiseK     = 0.04f;
    static constexpr float kNoiseFloor = 0.004f;

    // Non-Local Means tuning (nlm_bayer.slang). Radii are half-windows in
    // same-colour steps: search scans a (2R+1)^2 candidate grid, patch compares a
    // (2P+1)^2 same-colour neighbourhood (P clamped to 2 by the shader cache).
    // kNlmH scales the shot-noise sigma into the weight falloff — higher = more
    // smoothing (and more risk of plastic texture). Conservative defaults; the
    // initial window is sized to leave real-time headroom, then tuned on-device.
    // 8 candidates (a 3x3 window in same-colour steps). Must match MAX_S in
    // nlm_bayer.slang, which bakes it to keep the patch in registers.
    static constexpr int   kNlmSearchRadius = 1;
    static constexpr int   kNlmPatchRadius  = 1;     // 3x3 patch
    // How many sigma still counts as "the same surface". This is only meaningful
    // now that sigma is the sensor's MEASURED value (kUseSensorNoiseProfile); it
    // used to sit on top of a variance ~30-141x too large, and 1.25 was silently
    // compensating for that inflation rather than expressing a real threshold.
    //
    // The weight is exp(-ssd/h^2) with h = kNlmH*sigma, and two patches differing
    // only by noise have E[ssd] = 2*sigma^2, so a noise-only neighbour scores
    // exp(-2/kNlmH^2):
    //     1.25 -> 0.28  (barely averages: noise survives)
    //     4.0  -> 0.88  (averages noise hard)                 <-- here
    //     15   -> 0.99  (everything matches: a plain box blur)
    // and an edge at 10 sigma scores exp(-100/kNlmH^2):
    //     4.0  -> 0.002 (rejected, edge kept sharp)
    //     15   -> 0.64  (edge blurred away)
    // 15 is what the old inflated model effectively was, which is why the
    // "edge-preserving NLM" measured as an indiscriminate 3x3 blur. 4.0 keeps the
    // noise averaging while restoring the edge rejection that makes it an NLM.
    static constexpr float kNlmH            = 4.0f;

    // Chroma denoise tuning. A luma-guided (joint-bilateral) average over the
    // chroma plane only: radius is the half window in chroma sites; sigmaS shapes
    // the spatial falloff; sigmaL (in 10-bit PQ-luma units) is the edge-stop —
    // neighbours across a luma step larger than ~sigmaL are dropped so colour
    // never bleeds across object boundaries. Conservative defaults.
    static constexpr int   kChromaRadius = 3;      // 7x7 window (Restored for 30fps real-time)
    static constexpr float kChromaSigmaS = 2.0f;
    static constexpr float kChromaSigmaL = 20.0f;

    // Clock-domain rebase: sensor timestamps may be BOOTTIME while audio is
    // MONOTONIC; this fixed offset (computed on the first frame) aligns them.
    // Written on the camera thread, read by the pipeline thread — hence atomic.
    std::atomic<int64_t> ts_offset_{0};
    std::atomic<bool>    ts_anchored_{false};

    FormatCb on_format_;
    PacketCb on_packet_;

    // ── Vulkan ───────────────────────────────────────────────────────────────
    VkCompute             vk_;
    // Denoise prepass (raw buffer -> denoised buffer). Pipeline/layout shared;
    // the input descriptor (binding 0) is repointed per frame at the live slot.
    VkDescriptorSetLayout denoise_dsl_      = VK_NULL_HANDLE;
    VkPipelineLayout      denoise_layout_   = VK_NULL_HANDLE;
    VkPipeline            denoise_pipeline_ = VK_NULL_HANDLE;
    // Green-plane prepass (raw/denoised buffer -> shared green buffer) for the HQ
    // demosaic. Same 2-binding layout as denoise; input (binding 0) is repointed
    // per frame (raw set) or fixed (denoised set), output is the shared green buf.
    VkDescriptorSetLayout green_dsl_      = VK_NULL_HANDLE;
    VkPipelineLayout      green_layout_   = VK_NULL_HANDLE;
    VkPipeline            green_pipeline_ = VK_NULL_HANDLE;
    VkCompute::Buffer     green_buf_{};                // full-res float green (shared)
    // Chroma denoise pass (out_buf Y + per-slot chroma scratch -> out_buf CbCr).
    // Reads the ISP-diverted noisy chroma + the luma plane (for edge guidance) and
    // writes the cleaned CbCr back into the same P010 out_buf. Per-slot scratch
    // (no cross-frame sharing) so no extra cross-frame barrier is needed.
    VkDescriptorSetLayout chroma_dsl_      = VK_NULL_HANDLE;
    VkPipelineLayout      chroma_layout_   = VK_NULL_HANDLE;
    VkPipeline            chroma_pipeline_ = VK_NULL_HANDLE;
    // ── Binned path (bin2_ only) ─────────────────────────────────────────────
    // Two passes replace the four above: bin_isp (raw buffer -> per-slot linear
    // RGB at half resolution) and nlm_rgb (that RGB -> denoise + develop -> the
    // per-slot P010 out_buf). When bin2_ is set these are the ONLY compute
    // pipelines built; denoise_/green_/chroma_/pipeline_ all stay VK_NULL_HANDLE.
    VkDescriptorSetLayout bin_dsl_      = VK_NULL_HANDLE;
    VkPipelineLayout      bin_layout_   = VK_NULL_HANDLE;
    VkPipeline            bin_pipeline_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout nrgb_dsl_      = VK_NULL_HANDLE;
    VkPipelineLayout      nrgb_layout_   = VK_NULL_HANDLE;
    VkPipeline            nrgb_pipeline_ = VK_NULL_HANDLE;
    // 3x3 chroma median (binned path only): reads the CbCr nlm_rgb diverted into
    // the per-slot scratch and writes the final CbCr into out_buf. Luma is never
    // touched, so enabling it leaves the Y plane byte-identical.
    VkDescriptorSetLayout cmed_dsl_      = VK_NULL_HANDLE;
    VkPipelineLayout      cmed_layout_   = VK_NULL_HANDLE;
    VkPipeline            cmed_pipeline_ = VK_NULL_HANDLE;

    // ISP pass (raw/denoised buffer + green buffer -> per-slot out_buf P010).
    VkDescriptorSetLayout dsl_      = VK_NULL_HANDLE;
    VkPipelineLayout      layout_   = VK_NULL_HANDLE;
    VkPipeline            pipeline_ = VK_NULL_HANDLE;
    VkDescriptorPool      dpool_    = VK_NULL_HANDLE;
    // RAW16 input ring: the camera writes these directly and the ISP reads them
    // as storage buffers — no staging->image upload (saves ~164 MB/frame of
    // memory traffic). Allocated DEVICE_LOCAL + HOST_VISIBLE so the GPU reads
    // stay cached on UMA.
    VkCompute::Buffer     staging_[kSlots]{};

    // Frames-in-flight ring: each entry is an independent GPU job (own command
    // buffer, fence, P010 output buffer, and the three descriptor sets) so the
    // CPU readback+encode of one frame overlaps the GPU compute of the next. The
    // Bayer input is the camera-written staging buffer for that frame (bound per
    // dispatch); the NLM output is per-slot (see InFlight::denoised_buf).
    //
    // TWO is the exact depth pipeline_loop uses: it submits slot N and then
    // immediately retires slot N-1, so a third slot could never hold pending work
    // — it only cost memory (a P010 out_buf plus a chroma scratch, ~49 MB at
    // 4080x3060) and a set of descriptors. Do not raise this without also
    // deepening the submit/retire schedule.
    static constexpr int kInFlight = 2;
    struct InFlight {
        VkCommandBuffer   cmd          = VK_NULL_HANDLE;
        VkFence           fence        = VK_NULL_HANDLE;
        VkCompute::Buffer out_buf{};                     // P010, tight layout
        // Per-slot NLM output. It used to be one shared buffer, which forced a
        // full-buffer WAR barrier every frame (this slot's denoise write against
        // the other slot's ISP read) and serialized the two in-flight slots into
        // no GPU-GPU overlap at all. Per-slot costs one extra Bayer buffer and
        // removes the barrier outright.
        VkCompute::Buffer denoised_buf{};                 // uint16-packed Bayer
        VkDescriptorSet   denoise_dset = VK_NULL_HANDLE;  // raw buf -> denoised_buf_
        VkDescriptorSet   green_dset_raw = VK_NULL_HANDLE; // green reads raw buf (dn off)
        VkDescriptorSet   green_dset_dn  = VK_NULL_HANDLE; // green reads denoised_buf_
        VkDescriptorSet   dset         = VK_NULL_HANDLE;  // ISP reads denoised_buf_ (+green)
        VkDescriptorSet   dset_raw     = VK_NULL_HANDLE;  // ISP reads raw buf (+green, dn off)
        // Chroma denoise: per-slot noisy-chroma scratch (ISP binding 3 target)
        // and the set that reads it + out_buf's luma and writes out_buf's chroma.
        VkCompute::Buffer chroma_buf{};                   // packed (Cb,Cr) per site
        VkDescriptorSet   cd_dset      = VK_NULL_HANDLE;   // out_buf + chroma_buf
        // Binned path: half-res linear camera RGB between bin_isp and nlm_rgb.
        // Per-slot, not shared — a shared intermediate would need a full-buffer
        // WAR barrier against the other slot's read every frame, which is exactly
        // what serialized the in-flight ring before denoised_buf became per-slot.
        VkCompute::Buffer rgb_buf{};                       // 3 floats per output px
        // Second chroma scratch: the three-pass median ping-pongs
        // chroma_buf -> chroma_buf2 -> chroma_buf -> out_buf's CbCr.
        VkCompute::Buffer chroma_buf2{};
        VkDescriptorSet   bin_dset     = VK_NULL_HANDLE;   // raw buf -> rgb_buf
        VkDescriptorSet   nrgb_dset    = VK_NULL_HANDLE;   // rgb_buf -> out_buf
        VkDescriptorSet   cmed_dset1   = VK_NULL_HANDLE;   // chroma_buf -> chroma_buf2
        VkDescriptorSet   cmed_dset2   = VK_NULL_HANDLE;   // chroma_buf2 -> chroma_buf
        // GPU timestamps for this slot's passes. Without these the only timing
        // available is submit->fence, which pipeline_loop's blocking pop pins to
        // the camera period — so it reports "we keep up", never the actual cost.
        VkQueryPool       qpool        = VK_NULL_HANDLE;
        int               ts_count     = 0;   // timestamps this frame actually wrote
        int               staging_slot = -1;              // input buffer this job reads
        int64_t           ts_ns        = 0;               // frame PTS (rebased)
        int64_t           submit_ns    = 0;               // submit time, for GPU profiling
    };
    InFlight inflight_[kInFlight];

    // ── Frame store (camera thread -> pipeline thread) ──────────────────────
    // Never-drop RAM+disk FIFO: the camera pushes here, the pipeline pops. Decouples
    // the fixed 30 fps capture from the slower offline NLM process rate.
    FrameStore store_;
    // Which GPU staging buffers are currently bound to an in-flight job (pipeline
    // thread only — no cross-thread access, so no atomics needed).
    bool staging_busy_[kSlots] = { false };

    // ── Encoder ──────────────────────────────────────────────────────────────
    AMediaCodec* codec_ = nullptr;
    int enc_stride_bytes_ = 0;   // encoder Y/UV row stride (bytes)
    int enc_slice_height_ = 0;   // rows between Y plane start and UV plane

    std::thread       pipeline_thread_;
    std::thread       drain_thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopping_{false};
    bool              sent_format_ = false;

    // Frame accounting (logged at stop for fps visibility). frames_in_/
    // frames_dropped_ are written on the camera thread and read on the pipeline
    // thread (and vice-versa for drops on submit failure) — hence atomic.
    std::atomic<int64_t> frames_in_{0}, frames_dropped_{0};
    int64_t              frames_encoded_ = 0;   // drain thread only

    // Rolling per-stage profiling, flushed to logcat every kProfileWindow frames.
    // 30, not 60: when the pipeline is slow — exactly when the profile line is
    // needed — a 60-frame window can fail to complete for a whole clip and the
    // diagnostic never prints.
    static constexpr int kProfileWindow = 30;

    // Timestamps written per frame: one before the first dispatch, then one after
    // each pass. The binned path uses 6 (start, bin, nlm_rgb, median x3); the
    // full-res path writes fewer and leaves the rest unread.
    static constexpr int kTimestampSlots = 7;
    // True once the query pools exist and the queue family can timestamp.
    bool    gpu_timing_ = false;
    // Per-pass GPU nanoseconds accumulated over the profile window (index 0 is
    // the first pass). Reported as real milliseconds, unlike `gpu`.
    int64_t prof_pass_ns_[kTimestampSlots - 1]{};
    int     prof_pass_used_ = 0;   // how many passes the last frame actually wrote

public:
    // Each ISP pass is submitted as this many horizontal bands so the GPU can
    // preempt between them and service the compositor. One full-frame dispatch
    // is uninterruptible, which is why presenting during a RAW recording used to
    // stall ~90 ms.
    static constexpr uint32_t kDispatchBands = 8;
private:
    int     prof_count_   = 0;
    int64_t prof_gpu_ns_  = 0;   // submit -> fence wait (GPU compute)
    int64_t prof_copy_ns_ = 0;   // P010 memcpy into the encoder input buffer
    int64_t prof_encwait_ns_ = 0;// dequeueInputBuffer wait (encoder backpressure)
    int64_t prof_wall_ns_ = 0;   // wall time across the window (for fps)
    int64_t prof_last_ns_ = 0;   // timestamp of previous encoded frame
    int64_t prof_drops_base_ = 0;// frames_dropped_ at the window start
    // Per-window MAXIMA — these expose the ~1Hz spikes that the averages hide,
    // localizing the stall (gpu vs encoder vs upstream camera delivery).
    int64_t prof_gpu_max_ns_     = 0;
    int64_t prof_encwait_max_ns_ = 0;
    // Camera-thread writers, pipeline-thread reader/resetter — hence atomic.
    std::atomic<int64_t> prof_gap_max_ns_{0};   // worst on_frame inter-arrival
    std::atomic<int64_t> prof_gap_prev_ns_{0};  // previous on_frame timestamp
};

} // namespace isp
