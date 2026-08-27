#include "ndk_session.hh"

#include "../logger.hh"
#include "still_writer.hh"

#include <android/hardware_buffer.h>

#include <chrono>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace ndkcam {
namespace {

constexpr int kPreviewBuffers = 4;
// The ISP consumes frames slower than the sensor produces them, so the reader
// needs enough depth to absorb a burst without the HAL stalling the stream.
constexpr int kRawBuffers     = 6;
// One-shot still requests only need one in flight, but a couple spare keep the
// HAL from stalling if two shutters land back to back.
constexpr int kStillBuffers   = 3;

struct StreamSize { int32_t w, h; };

// All output sizes the device advertises for `format`, from
// ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS (quads of
// format/width/height/isInput).
std::vector<StreamSize> output_sizes(const ACameraMetadata* meta, int32_t format) {
    std::vector<StreamSize> out;
    ACameraMetadata_const_entry e{};
    if (ACameraMetadata_getConstEntry(meta, ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS,
                                      &e) != ACAMERA_OK)
        return out;
    for (uint32_t i = 0; i + 3 < e.count; i += 4) {
        if (e.data.i32[i] != format) continue;
        if (e.data.i32[i + 3] != ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT) continue;
        out.push_back({ e.data.i32[i + 1], e.data.i32[i + 2] });
    }
    return out;
}

bool has_capability(const ACameraMetadata* meta, uint8_t cap) {
    ACameraMetadata_const_entry e{};
    if (ACameraMetadata_getConstEntry(meta, ACAMERA_REQUEST_AVAILABLE_CAPABILITIES,
                                      &e) != ACAMERA_OK)
        return false;
    for (uint32_t i = 0; i < e.count; ++i) if (e.data.u8[i] == cap) return true;
    return false;
}

// The sensor's true active-array aspect. NOT the max-area output size: the
// framework moves sub-30fps modes onto the high-resolution list, so on some
// sensors the largest full-rate output is square and a max-area heuristic
// silently records a 1:1 crop (the bug the Java path had to work around).
float sensor_aspect(const ACameraMetadata* meta) {
    ACameraMetadata_const_entry e{};
    if (ACameraMetadata_getConstEntry(meta, ACAMERA_SENSOR_INFO_ACTIVE_ARRAY_SIZE,
                                      &e) == ACAMERA_OK && e.count >= 4) {
        const float w = float(e.data.i32[2] - e.data.i32[0]);
        const float h = float(e.data.i32[3] - e.data.i32[1]);
        if (w > 0.0f && h > 0.0f) return w / h;
    }
    return 4.0f / 3.0f;
}

// Highest fixed (min == max) AE range, so exposure can't drop the frame rate
// mid-clip. Falls back to the range with the highest max.
void pick_fps_range(const ACameraMetadata* meta, int32_t& lo, int32_t& hi) {
    lo = hi = 0;
    ACameraMetadata_const_entry e{};
    if (ACameraMetadata_getConstEntry(meta, ACAMERA_CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES,
                                      &e) != ACAMERA_OK)
        return;
    for (uint32_t i = 0; i + 1 < e.count; i += 2) {
        const int32_t a = e.data.i32[i], b = e.data.i32[i + 1];
        const bool fixed_now  = (lo == hi) && lo != 0;
        const bool fixed_cand = (a == b);
        if (fixed_cand && (!fixed_now || b > hi)) { lo = a; hi = b; }
        else if (!fixed_now && !fixed_cand && b > hi) { lo = a; hi = b; }
    }
}

} // namespace

Session::~Session() { shutdown(); }

// ── Setup ────────────────────────────────────────────────────────────────────

bool Session::pick_camera() {
    ACameraIdList* ids = nullptr;
    if (ACameraManager_getCameraIdList(manager_, &ids) != ACAMERA_OK || !ids) {
        LOGE("ndk: getCameraIdList failed");
        return false;
    }

    bool found = false;
    for (int i = 0; i < ids->numCameras && !found; ++i) {
        ACameraMetadata* meta = nullptr;
        if (ACameraManager_getCameraCharacteristics(manager_, ids->cameraIds[i], &meta) != ACAMERA_OK)
            continue;

        ACameraMetadata_const_entry e{};
        const bool back =
            ACameraMetadata_getConstEntry(meta, ACAMERA_LENS_FACING, &e) == ACAMERA_OK &&
            e.count >= 1 && e.data.u8[0] == ACAMERA_LENS_FACING_BACK;

        if (back && !found) {
            const bool has_raw =
                has_capability(meta, ACAMERA_REQUEST_AVAILABLE_CAPABILITIES_RAW);
            auto raws = has_raw ? output_sizes(meta, AIMAGE_FORMAT_RAW16)
                                : std::vector<StreamSize>{};
            if (true) {
                if (!raws.empty()) {
                    // Diagnostic: the full advertised list, not just the winner.
                    // Most sensors advertise exactly one RAW16 size, but if this
                    // device offers a smaller one, selecting it would cut the
                    // camera-thread memcpy and the FrameStore's ~0.4 GB as well —
                    // savings the GPU-side 2x2 bin cannot reach. Note the size is
                    // picked TWICE and independently (here, and again in
                    // dng_meta_source.cc's load_static_meta, which is the copy the
                    // ISP is actually sized from): change one without the other
                    // and RawVideoPipeline::on_frame drops every frame on a
                    // geometry mismatch.
                    for (const auto& r : raws)
                        LOGI("ndk: RAW16 size available %dx%d", r.w, r.h);
                    // Largest RAW16 stream: the sensor's full binned readout.
                    auto best = *std::max_element(raws.begin(), raws.end(),
                        [](const StreamSize& a, const StreamSize& b) {
                            return int64_t(a.w) * a.h < int64_t(b.w) * b.h;
                        });
                    raw_w_ = best.w;
                    raw_h_ = best.h;
                }

                // Preview: the largest PRIVATE output at (or under) 1080p whose
                // aspect matches the sensor, so the on-screen framing is honest.
                const float want = sensor_aspect(meta);
                int64_t best_area = 0;
                for (const auto& s : output_sizes(meta, AIMAGE_FORMAT_PRIVATE)) {
                    if (s.h > 1080) continue;
                    const float ar = float(s.w) / float(s.h);
                    if (std::abs(ar - want) > 0.05f) continue;
                    const int64_t area = int64_t(s.w) * s.h;
                    if (area > best_area) { best_area = area; prev_w_ = s.w; prev_h_ = s.h; }
                }
                if (prev_w_ == 0) { prev_w_ = 1280; prev_h_ = 720; }

                // The loupe's source: the LARGEST PRIVATE output at the same
                // aspect, uncapped. Magnifying the 1080p preview would only
                // upscale it — the extra pixels have to come from the sensor,
                // and this stream is alive only while the loupe is open (never
                // during a recording), so the usual bandwidth argument for
                // keeping the preview small does not apply to it. An earlier
                // 2160-line cap looked harmless and was not: on the S23 the
                // 4:3 PRIVATE sizes step 1920x1440 -> 4000x3000, so the cap
                // threw away the only size that made the loupe worth having.
                int64_t hi_area = 0;
                for (const auto& s : output_sizes(meta, AIMAGE_FORMAT_PRIVATE)) {
                    const float ar = float(s.w) / float(s.h);
                    if (std::abs(ar - want) > 0.05f) continue;
                    const int64_t area = int64_t(s.w) * s.h;
                    if (area > hi_area) { hi_area = area; prev_hi_w_ = s.w; prev_hi_h_ = s.h; }
                }

                // Manual focus needs both a lens that moves and an AF mode the
                // HAL lets us switch off. A fixed-focus module reports a
                // minimum focus distance of 0 (== infinity), which is the
                // documented "this lens cannot focus" signal.
                ACameraMetadata_const_entry fd{};
                if (ACameraMetadata_getConstEntry(
                        meta, ACAMERA_LENS_INFO_MINIMUM_FOCUS_DISTANCE, &fd) == ACAMERA_OK &&
                    fd.count > 0) {
                    max_diopters_ = fd.data.f[0];
                }
                ACameraMetadata_const_entry af{};
                if (ACameraMetadata_getConstEntry(
                        meta, ACAMERA_CONTROL_AF_AVAILABLE_MODES, &af) == ACAMERA_OK) {
                    for (uint32_t k = 0; k < af.count; ++k)
                        if (af.data.u8[k] == ACAMERA_CONTROL_AF_MODE_OFF) af_off_ok_ = true;
                }
                LOGI("ndk: manual focus %s (max %.2f diopters, AF_OFF %d), loupe %dx%d",
                     (af_off_ok_ && max_diopters_ > 0.0f) ? "available" : "unavailable",
                     max_diopters_, (int)af_off_ok_, prev_hi_w_, prev_hi_h_);

                pick_fps_range(meta, fps_min_, fps_max_);
                camera_id_ = ids->cameraIds[i];
                found = true;

                // MANUAL_POST_PROCESSING lets the video path take the HAL's
                // baked-in contrast curve out of the loop (flat tonemap, NR
                // off) — the concrete image-quality win over the Java path.
                has_manual_post_ = has_capability(
                    meta, ACAMERA_REQUEST_AVAILABLE_CAPABILITIES_MANUAL_POST_PROCESSING);

                // Stills are the opposite of clips: they want the HAL's best
                // in-silicon noise reduction, not the flat raw look. HIGH_
                // QUALITY is optional per device, so fall back through what
                // the HAL actually advertises.
                still_nr_     = ACAMERA_NOISE_REDUCTION_MODE_FAST;
                still_nr_off_ = ACAMERA_NOISE_REDUCTION_MODE_FAST;
                still_edge_ = ACAMERA_EDGE_MODE_FAST;
                ACameraMetadata_const_entry nm{};
                if (ACameraMetadata_getConstEntry(
                        meta, ACAMERA_NOISE_REDUCTION_AVAILABLE_NOISE_REDUCTION_MODES,
                        &nm) == ACAMERA_OK) {
                    for (uint32_t k = 0; k < nm.count; ++k) {
                        if (nm.data.u8[k] == ACAMERA_NOISE_REDUCTION_MODE_HIGH_QUALITY)
                            still_nr_ = ACAMERA_NOISE_REDUCTION_MODE_HIGH_QUALITY;
                        // OFF is optional too; without it FAST is the floor.
                        if (nm.data.u8[k] == ACAMERA_NOISE_REDUCTION_MODE_OFF)
                            still_nr_off_ = ACAMERA_NOISE_REDUCTION_MODE_OFF;
                    }
                }
                LOGI("ndk: still NR modes: on=%u off=%u", still_nr_, still_nr_off_);
                if (ACameraMetadata_getConstEntry(
                        meta, ACAMERA_EDGE_AVAILABLE_EDGE_MODES, &nm) == ACAMERA_OK) {
                    for (uint32_t k = 0; k < nm.count; ++k)
                        if (nm.data.u8[k] == ACAMERA_EDGE_MODE_HIGH_QUALITY)
                            still_edge_ = ACAMERA_EDGE_MODE_HIGH_QUALITY;
                }

                // Video candidates, largest first, at the sensor's aspect. The
                // encoder decides which is real (see Encoder::configure): a
                // capability-table heuristic is what pinned the Java path to
                // 1440x1088 on a device whose encoder does 2560x1440@30.
                const float vwant = sensor_aspect(meta);
                for (const auto& sz : output_sizes(meta, AIMAGE_FORMAT_PRIVATE)) {
                    const float ar = float(sz.w) / float(sz.h);
                    if (std::abs(ar - vwant) > 0.05f) continue;
                    video_sizes_.push_back(Encoder::Size{ sz.w, sz.h });
                }
                std::sort(video_sizes_.begin(), video_sizes_.end(),
                          [](const Encoder::Size& a, const Encoder::Size& b) {
                              return int64_t(a.w) * a.h > int64_t(b.w) * b.h;
                          });

                // Still candidates: the largest full-resolution YUV_420_888
                // output, same pick the retired Java still path made.
                int64_t best_still = 0;
                for (const auto& s : output_sizes(meta, AIMAGE_FORMAT_YUV_420_888)) {
                    const int64_t area = int64_t(s.w) * s.h;
                    if (area > best_still) { best_still = area; still_w_ = s.w; still_h_ = s.h; }
                }
                ACameraMetadata_const_entry so{};
                if (ACameraMetadata_getConstEntry(meta, ACAMERA_SENSOR_ORIENTATION, &so)
                        == ACAMERA_OK && so.count >= 1)
                    sensor_orientation_ = so.data.i32[0];

                // Reuse the DNG metadata gatherer's tag set by filling here.
                ACameraMetadata_const_entry q{};
                if (ACameraMetadata_getConstEntry(meta, ACAMERA_SENSOR_INFO_COLOR_FILTER_ARRANGEMENT, &q) == ACAMERA_OK && q.count >= 1)
                    static_meta_.cfa = q.data.u8[0];
                if (ACameraMetadata_getConstEntry(meta, ACAMERA_SENSOR_INFO_WHITE_LEVEL, &q) == ACAMERA_OK && q.count >= 1)
                    static_meta_.white_level = q.data.i32[0];
                if (ACameraMetadata_getConstEntry(meta, ACAMERA_SENSOR_BLACK_LEVEL_PATTERN, &q) == ACAMERA_OK && q.count >= 4)
                    for (int k = 0; k < 4; ++k) static_meta_.black_level[k] = float(q.data.i32[k]);
                // AE bracket limits. Compensation is requested in STEP units;
                // both the range and the step size are characteristics.
                if (ACameraMetadata_getConstEntry(meta, ACAMERA_CONTROL_AE_COMPENSATION_RANGE, &q) == ACAMERA_OK && q.count >= 2) {
                    ae_comp_min_ = q.data.i32[0];
                    ae_comp_max_ = q.data.i32[1];
                }
                if (ACameraMetadata_getConstEntry(meta, ACAMERA_CONTROL_AE_COMPENSATION_STEP, &q) == ACAMERA_OK && q.count >= 1) {
                    ae_comp_step_num_ = q.data.r[0].numerator;
                    ae_comp_step_den_ = q.data.r[0].denominator ? q.data.r[0].denominator : 3;
                }
                static_meta_.width  = raw_w_;
                static_meta_.height = raw_h_;
            }
        }
        ACameraMetadata_free(meta);
    }
    ACameraManager_deleteCameraIdList(ids);

    if (!found) {
        LOGI("ndk: no usable back camera — the Java session must handle this device");
        return false;
    }
    if (raw_w_ > 0) {
        LOGI("ndk: camera %s  RAW16 %dx%d  preview %dx%d  fps [%d,%d]",
             camera_id_.c_str(), raw_w_, raw_h_, prev_w_, prev_h_, fps_min_, fps_max_);
    } else {
        LOGI("ndk: camera %s  no RAW  preview %dx%d  still YUV %dx%d (orient %d)  "
             "fps [%d,%d]  manual-post=%d  %zu video sizes",
             camera_id_.c_str(), prev_w_, prev_h_, still_w_, still_h_,
             sensor_orientation_, fps_min_, fps_max_,
             int(has_manual_post_), video_sizes_.size());
    }
    return true;
}

bool Session::init(jni::PreviewSink preview, jni::RawSink raw, jni::RawVideoSink raw_video,
                   jni::RecordSink record) {
    // Idempotent: re-initialising over a live session would overwrite manager_
    // and device_ and leak both, leaving the camera open forever. The HAL then
    // refuses every subsequent client ("Could not initialize client from HAL"),
    // which wedges the camera for the whole device until reboot.
    shutdown();

    preview_sink_   = std::move(preview);
    raw_sink_       = std::move(raw);
    raw_video_sink_ = std::move(raw_video);
    record_sink_    = std::move(record);

    manager_ = ACameraManager_create();
    if (!manager_) { LOGE("ndk: ACameraManager_create failed"); return false; }
    if (!pick_camera()) { shutdown(); return false; }

    dev_cbs_.context        = this;
    dev_cbs_.onDisconnected = on_disconnected;
    dev_cbs_.onError        = on_device_error;
    if (ACameraManager_openCamera(manager_, camera_id_.c_str(), &dev_cbs_, &device_) != ACAMERA_OK) {
        LOGE("ndk: openCamera failed");
        shutdown();
        return false;
    }

    // Non-RAW video path: resolve the encoder size and create its input surface
    // once, up front. Recording only swaps it INTO the session (see start_-
    // recording); photo mode swaps it out again.
    if (raw_w_ == 0) {
        if (video_sizes_.empty()) {
            LOGI("ndk: no video sizes at the sensor aspect");
            shutdown();
            return false;
        }
        const int32_t fps = fps_max_ > 0 ? fps_max_ : 30;
        if (!encoder_.configure(video_sizes_.data(),
                                static_cast<int>(video_sizes_.size()), fps, record_sink_)) {
            shutdown();
            return false;
        }
        video_available_ = true;
    }

    if (!configure_session(/*with_video=*/false)) { shutdown(); return false; }
    available_ = true;
    if (still_target_) still_worker_ = std::thread(&Session::still_worker_loop, this);
    return true;
}

bool Session::configure_session(const bool with_video) {
    if (!device_) return false;

    // Close any previous capture session first, and WAIT for it: the readers
    // below outlive most rebuilds, but the loupe's size change deletes the
    // preview one, and freeing a consumer the HAL is still queueing into is
    // what produced a second of "Failed to queue buffer to client" and a long
    // preview blink.
    close_session_and_wait();
    if (outputs_) { ACaptureSessionOutputContainer_free(outputs_); outputs_ = nullptr; }
    if (preview_target_) { ACameraOutputTarget_free(preview_target_);   preview_target_ = nullptr; }
    if (raw_target_)     { ACameraOutputTarget_free(raw_target_);       raw_target_ = nullptr; }
    if (video_target_)   { ACameraOutputTarget_free(video_target_);     video_target_ = nullptr; }
    if (still_target_)   { ACameraOutputTarget_free(still_target_);     still_target_ = nullptr; }
    if (preview_output_) { ACaptureSessionOutput_free(preview_output_); preview_output_ = nullptr; }
    if (raw_output_)     { ACaptureSessionOutput_free(raw_output_);     raw_output_ = nullptr; }
    if (video_output_)   { ACaptureSessionOutput_free(video_output_);   video_output_ = nullptr; }
    if (still_output_)   { ACaptureSessionOutput_free(still_output_);   still_output_ = nullptr; }

    // Preview: PRIVATE + GPU_SAMPLED so the AHardwareBuffer imports straight
    // into the Vulkan renderer with no copy. The loupe raises the size so the
    // magnified inset has real pixels; the reader is rebuilt only when the
    // wanted size actually differs from the live one (it normally survives
    // every session rebuild, and recreating it costs a buffer round trip).
    const int32_t want_w = (loupe_on_ && prev_hi_w_ > 0) ? prev_hi_w_ : prev_w_;
    const int32_t want_h = (loupe_on_ && prev_hi_h_ > 0) ? prev_hi_h_ : prev_h_;
    if (preview_reader_ && (want_w != prev_live_w_ || want_h != prev_live_h_)) {
        AImageReader_setImageListener(preview_reader_, nullptr);
        AImageReader_delete(preview_reader_);
        preview_reader_ = nullptr;
        preview_window_ = nullptr;   // owned by the reader we just deleted
    }
    if (!preview_reader_ &&
        AImageReader_newWithUsage(want_w, want_h, AIMAGE_FORMAT_PRIVATE,
                                  AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE,
                                  kPreviewBuffers, &preview_reader_) != AMEDIA_OK) {
        LOGE("ndk: preview AImageReader failed (%dx%d)", want_w, want_h);
        return false;
    }
    prev_live_w_ = want_w;
    prev_live_h_ = want_h;
    preview_listener_ = { this, on_preview_image };
    AImageReader_setImageListener(preview_reader_, &preview_listener_);
    if (AImageReader_getWindow(preview_reader_, &preview_window_) != AMEDIA_OK) return false;

    const bool raw_path = raw_w_ > 0;
    if (raw_path) {
        if (!raw_reader_) {
            if (AImageReader_new(raw_w_, raw_h_, AIMAGE_FORMAT_RAW16, kRawBuffers,
                                 &raw_reader_) != AMEDIA_OK) {
                LOGE("ndk: RAW16 AImageReader failed");
                return false;
            }
            raw_listener_ = { this, on_raw_image };
            AImageReader_setImageListener(raw_reader_, &raw_listener_);
            if (AImageReader_getWindow(raw_reader_, &raw_window_) != AMEDIA_OK) return false;
        }
    } else if (!with_video) {
        // Full-resolution YUV still stream — photo mode's second output. This
        // is the exact stream topology the retired Java still path used.
        //
        // CPU_READ_OFTEN matters: framework ImageReader allocates YUV consumer
        // buffers with CPU-read usage, and without it this HAL hands out
        // zero-filled buffers the ISP apparently never writes into.
        if (!still_reader_ && still_w_ > 0) {
            if (AImageReader_newWithUsage(still_w_, still_h_, AIMAGE_FORMAT_YUV_420_888,
                                          AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN,
                                          kStillBuffers, &still_reader_) != AMEDIA_OK) {
                LOGE("ndk: YUV still AImageReader failed");
            } else {
                still_listener_ = { this, on_still_image };
                AImageReader_setImageListener(still_reader_, &still_listener_);
                if (AImageReader_getWindow(still_reader_, &still_window_) != AMEDIA_OK) {
                    release_still_stream();
                }
            }
        }
    } else {
        // Recording session: the encoder's own input surface becomes the video
        // stream, so frames never touch the CPU. The encoder itself was
        // configured once in init(); this only changes session membership.
    }

    // Photo session (non-RAW): preview + full-res YUV still — the topology the
    // retired Java still path ran. Recording session: preview + encoder
    // surface. RAW session: preview + RAW16, always.
    if (ACaptureSessionOutputContainer_create(&outputs_) != ACAMERA_OK) return false;
    if (ACaptureSessionOutput_create(preview_window_, &preview_output_) != ACAMERA_OK) return false;
    ACaptureSessionOutputContainer_add(outputs_, preview_output_);
    if (raw_path) {
        if (ACaptureSessionOutput_create(raw_window_, &raw_output_) != ACAMERA_OK) return false;
        ACaptureSessionOutputContainer_add(outputs_, raw_output_);
    } else if (with_video) {
        if (ACaptureSessionOutput_create(encoder_.input_surface(), &video_output_) != ACAMERA_OK)
            return false;
        ACaptureSessionOutputContainer_add(outputs_, video_output_);
    } else if (still_window_) {
        if (ACaptureSessionOutput_create(still_window_, &still_output_) == ACAMERA_OK)
            ACaptureSessionOutputContainer_add(outputs_, still_output_);
        else
            release_still_stream();
    }

    sess_cbs_.context   = this;
    sess_cbs_.onReady   = on_session_ready;
    sess_cbs_.onActive  = on_session_active;
    sess_cbs_.onClosed  = on_session_closed;
    if (ACameraDevice_createCaptureSession(device_, outputs_, &sess_cbs_, &session_) != ACAMERA_OK) {
        LOGE("ndk: createCaptureSession failed (with_video=%d)", with_video);
        return false;
    }

    if (ACameraOutputTarget_create(preview_window_, &preview_target_) != ACAMERA_OK) return false;
    if (raw_path) {
        if (ACameraOutputTarget_create(raw_window_, &raw_target_) != ACAMERA_OK) return false;
    } else if (with_video) {
        if (ACameraOutputTarget_create(encoder_.input_surface(), &video_target_) != ACAMERA_OK)
            return false;
    } else if (still_window_) {
        if (ACameraOutputTarget_create(still_window_, &still_target_) != ACAMERA_OK)
            release_still_stream();
    }
    return true;
}

// Drops the YUV still stream entirely, leaving a working preview-only setup.
// Used at teardown and whenever the still reader cannot be built.
void Session::release_still_stream() {
    if (still_target_) { ACameraOutputTarget_free(still_target_);     still_target_ = nullptr; }
    if (still_output_) { ACaptureSessionOutput_free(still_output_);   still_output_ = nullptr; }
    if (still_reader_) {
        AImageReader_setImageListener(still_reader_, nullptr);
        AImageReader_delete(still_reader_);
        still_reader_ = nullptr;
    }
    still_window_ = nullptr;
}

// ── Repeating request ────────────────────────────────────────────────────────

bool Session::build_repeating(bool with_raw) {
    if (!device_ || !session_) return false;

    ACameraCaptureSession_stopRepeating(session_);
    if (request_) { ACaptureRequest_free(request_); request_ = nullptr; }

    const ACameraDevice_request_template tmpl =
        with_raw ? TEMPLATE_RECORD : TEMPLATE_PREVIEW;
    if (ACameraDevice_createCaptureRequest(device_, tmpl, &request_) != ACAMERA_OK) {
        LOGE("ndk: createCaptureRequest failed");
        return false;
    }

    // The preview target stays attached while recording. Dropping it (what the
    // Java path did) froze the on-screen image; the ISP now submits its passes
    // in preemptible bands, so the compositor can be served without stalling it.
    ACaptureRequest_addTarget(request_, preview_target_);
    if (with_raw) {
        ACaptureRequest_addTarget(request_, raw_target_ ? raw_target_ : video_target_);
    }

    if (with_raw && fps_max_ > 0) {
        const int32_t fps[2] = { fps_min_, fps_max_ };
        ACaptureRequest_setEntry_i32(request_, ACAMERA_CONTROL_AE_TARGET_FPS_RANGE, 2, fps);
    }
    if (with_raw) {
        if (raw_target_) {
            // One white balance per clip: lock AWB so the ISP's gains stay valid
            // for the whole recording, and report the locked neutral once.
            const uint8_t lock = 1;
            ACaptureRequest_setEntry_u8(request_, ACAMERA_CONTROL_AWB_LOCK, 1, &lock);
        }
    }
    apply_focus_entries(request_);
    neutral_sent_.store(false, std::memory_order_release);
    // Recording-only: the flat tonemap exists to make CLIPS gradeable. The
    // idle/still requests must stay exactly like the Java path's (template +
    // target, nothing else) — this HAL bleeds repeating-request state into
    // later one-shots, and a preview pinned to the video curve made every
    // still come out flat.
    if (with_raw && !raw_target_ && has_manual_post_) {
        // The video path's actual image-quality win: take the HAL's baked-in
        // contrast curve and its edge/noise processing out of the loop. A
        // straight-line tonemap is the flattest curve the API can express, and
        // it is what makes the recording gradeable instead of pre-cooked.
        // Recording-only: the preview/still requests stay exactly like the
        // Java path's (template + target, nothing else).
        const uint8_t tm_mode = ACAMERA_TONEMAP_MODE_CONTRAST_CURVE;
        ACaptureRequest_setEntry_u8(request_, ACAMERA_TONEMAP_MODE, 1, &tm_mode);
        const float curve[4] = { 0.0f, 0.0f, 1.0f, 1.0f };   // (in,out) pairs
        ACaptureRequest_setEntry_float(request_, ACAMERA_TONEMAP_CURVE_RED,   4, curve);
        ACaptureRequest_setEntry_float(request_, ACAMERA_TONEMAP_CURVE_GREEN, 4, curve);
        ACaptureRequest_setEntry_float(request_, ACAMERA_TONEMAP_CURVE_BLUE,  4, curve);
        const uint8_t nr_off = ACAMERA_NOISE_REDUCTION_MODE_OFF;
        ACaptureRequest_setEntry_u8(request_, ACAMERA_NOISE_REDUCTION_MODE, 1, &nr_off);
        const uint8_t sharp_off = ACAMERA_EDGE_MODE_OFF;
        ACaptureRequest_setEntry_u8(request_, ACAMERA_EDGE_MODE, 1, &sharp_off);
    }

    ACameraCaptureSession_captureCallbacks cbs{};
    cbs.context             = this;
    cbs.onCaptureCompleted  = on_capture_completed;
    if (ACameraCaptureSession_setRepeatingRequest(session_, &cbs, 1, &request_, nullptr)
        != ACAMERA_OK) {
        LOGE("ndk: setRepeatingRequest failed");
        return false;
    }
    return true;
}

bool Session::start_preview()  { return build_repeating(false); }

void Session::stop_preview() {
    if (session_) ACameraCaptureSession_stopRepeating(session_);
}

void Session::apply_focus_entries(ACaptureRequest* req) const {
    if (!req || !focus_manual_.load(std::memory_order_relaxed)) return;
    // AF has to be switched off before the lens position is honoured: with any
    // auto mode running the HAL owns the lens and LENS_FOCUS_DISTANCE is
    // ignored (or fought over) on the next convergence.
    const uint8_t af_off = ACAMERA_CONTROL_AF_MODE_OFF;
    ACaptureRequest_setEntry_u8(req, ACAMERA_CONTROL_AF_MODE, 1, &af_off);
    float d = focus_diopters_.load(std::memory_order_relaxed);
    if (d < 0.0f) d = 0.0f;
    if (d > max_diopters_) d = max_diopters_;
    ACaptureRequest_setEntry_float(req, ACAMERA_LENS_FOCUS_DISTANCE, 1, &d);
}

void Session::resubmit_repeating() {
    if (!session_ || !request_) return;
    ACameraCaptureSession_captureCallbacks cbs{};
    cbs.context            = this;
    cbs.onCaptureCompleted = on_capture_completed;
    ACameraCaptureSession_setRepeatingRequest(session_, &cbs, 1, &request_, nullptr);
}

void Session::set_focus(bool manual, float diopters) {
    const bool was_manual = focus_manual_.exchange(manual, std::memory_order_relaxed);
    focus_diopters_.store(diopters, std::memory_order_relaxed);
    if (!session_ || !request_) return;

    if (manual) {
        // Cheap enough to run per drag tick: the request object is reused, so
        // this is an entry write plus a re-submit, not a rebuild.
        apply_focus_entries(request_);
        resubmit_repeating();
    } else if (was_manual) {
        // Handing the lens back needs the full rebuild — the template picks the
        // right auto mode (CONTINUOUS_PICTURE vs _VIDEO) for the current one,
        // and there is no "unset" for an entry already written into a request.
        build_repeating(recording_.load(std::memory_order_acquire));
    }
}

bool Session::set_loupe(bool on) {
    if (on == loupe_on_) return true;
    if (!loupe_available()) return false;
    if (recording_.load(std::memory_order_acquire)) {
        LOGE("ndk: loupe refused while recording");
        return false;
    }
    loupe_on_ = on;
    // The preview reader is recreated at the new size inside configure_session;
    // the session must be rebuilt around it because a reader's window is a
    // session output.
    if (!configure_session(/*with_video=*/false)) {
        LOGE("ndk: loupe reconfigure failed, reverting");
        loupe_on_ = !on;
        configure_session(/*with_video=*/false);
        build_repeating(false);
        return false;
    }
    if (!build_repeating(false)) return false;
    LOGI("ndk: loupe %s (preview %dx%d)", on ? "on" : "off", prev_live_w_, prev_live_h_);
    return true;
}

bool Session::start_recording() {
    // A recording never runs on the loupe's high-resolution preview: it would
    // eat the RAW path's frame budget. Clearing the flag here means the rebuild
    // below (or the explicit one for the RAW path) drops the big stream in the
    // same reconfigure that starts the recording, rather than in a second one.
    const bool had_loupe = loupe_on_;
    loupe_on_ = false;

    if (video_available_) {
        // Non-RAW devices cannot host preview + encoder-surface + full-res YUV
        // in one session: the HAL then black-fills the still stream, and if the
        // still stream rides the repeating request instead the ISP budget
        // collapses (single-digit fps system-wide). So the session is split,
        // like the Java path's was: photos run on preview+still, and REC swaps
        // in the encoder surface. Only REC boundaries pay the reconfigure.
        if (!configure_session(/*with_video=*/true)) return false;
        if (!encoder_.start()) return false;
    } else if (had_loupe) {
        // The RAW path does not otherwise rebuild the session to record, so the
        // preview stream has to be brought back down explicitly.
        if (!configure_session(/*with_video=*/false)) return false;
    }
    if (!build_repeating(true)) return false;
    recording_.store(true, std::memory_order_release);
    return true;
}

void Session::stop_recording() {
    recording_.store(false, std::memory_order_release);
    build_repeating(false);
    if (video_available_) {
        encoder_.stop();
        // Back to the photo-capable session (preview + full-res YUV still).
        if (!configure_session(/*with_video=*/false)) {
            LOGE("ndk: photo session rebuild failed");
            return;
        }
        build_repeating(false);
    }
}

bool Session::take_photo(const char* base_path, int shots) {
    if (!base_path || shots < 1) return false;
    if (recording_.load(std::memory_order_acquire)) {
        LOGE("ndk: takePhoto refused while recording");
        return false;
    }

    // Non-RAW device: the RAW burst below would aim at a null target (this is
    // exactly why the shutter used to produce nothing here). One full-res YUV
    // frame -> lossless JXL instead; always single-shot, since the bracket
    // merge is a Bayer-domain tool this stream can't feed.
    if (!raw_target_) {
        if (!still_target_) {
            LOGE("ndk: takePhoto: no RAW and no YUV still stream");
            return false;
        }
        {
            std::lock_guard<std::mutex> lk(still_mtx_);
            still_paths_.assign(1, std::string(base_path) + ".png");
            still_total_ = 1;
        }
        ACaptureRequest* still = nullptr;
        if (ACameraDevice_createCaptureRequest(device_, TEMPLATE_STILL_CAPTURE, &still)
                != ACAMERA_OK) {
            LOGE("ndk: still capture request failed");
            return false;
        }
        ACaptureRequest_addTarget(still, still_target_);
        // Noise reduction is the user's call (the photo-mode NR chip), and the
        // default is off — raw sensor pixels, matching the clip's flat look.
        // Tapping it arms the HAL's best in-silicon NR for dim rooms, where
        // this sensor lands around ISO 3000. Edge enhancement is not toggled.
        const uint8_t nr = still_nr_on_.load(std::memory_order_relaxed) ? still_nr_
                                                                        : still_nr_off_;
        ACaptureRequest_setEntry_u8(still, ACAMERA_NOISE_REDUCTION_MODE, 1, &nr);
        ACaptureRequest_setEntry_u8(still, ACAMERA_EDGE_MODE, 1, &still_edge_);
        apply_focus_entries(still);

        ACameraCaptureSession_captureCallbacks cbs{};
        cbs.context            = this;
        cbs.onCaptureCompleted = on_capture_completed;
        const bool ok = ACameraCaptureSession_capture(session_, &cbs, 1, &still, nullptr)
                        == ACAMERA_OK;
        ACaptureRequest_free(still);
        if (ok) LOGI("ndk: YUV still -> %s.png", base_path);
        else    LOGE("ndk: YUV still submit failed");
        return ok;
    }

    std::lock_guard<std::mutex> lk(still_mtx_);
    still_paths_.clear();
    still_total_ = shots;
    for (int i = 0; i < shots; ++i) {
        std::string p = base_path;
        still_paths_.push_back(shots == 1 ? p + ".dng"
                                          : p + "_" + std::to_string(i) + ".dng");
    }
    // The RAW stream is already part of the session, so a still is just a
    // one-shot request against the same target.
    //
    // A MULTI-SHOT still is an exposure bracket: -2 / 0 / +2 EV by burst
    // position, same order the Java fallback used (index 0 = darkest, which is
    // the convention hdr_merge's BracketFrame::index documents). The old code
    // queued `shots` copies of ONE request — three identical exposures, so the
    // merge had zero real highlight headroom and every blown LED core landed
    // pinned at white. Compensation is quantized to the HAL's step and clamped
    // to its range; on a device that cannot bracket, everything collapses to 0
    // and behaves exactly like the old single-request burst.
    const int nreq = shots;
    int comp[3] = {0, 0, 0};
    if (shots > 1 && ae_comp_step_den_ > 0) {
        const double step_ev = double(ae_comp_step_num_) / ae_comp_step_den_;
        int two = int(std::lround(2.0 / step_ev));
        two = std::max(ae_comp_min_, std::min(ae_comp_max_, two));
        const int pos_two = std::max(ae_comp_min_, std::min(ae_comp_max_, two));
        const int neg_two = std::max(ae_comp_min_, std::min(ae_comp_max_, -pos_two));
        comp[0] = neg_two;                       // darkest first
        comp[1] = 0;                             // reference
        comp[2] = (shots > 2) ? pos_two : 0;     // brightest last
    }
    std::vector<ACaptureRequest*> burst;
    burst.reserve(size_t(nreq));
    for (int i = 0; i < nreq; ++i) {
        ACaptureRequest* req = nullptr;
        if (ACameraDevice_createCaptureRequest(device_, TEMPLATE_STILL_CAPTURE, &req)
                != ACAMERA_OK) {
            for (auto* r : burst) ACaptureRequest_free(r);
            return false;
        }
        ACaptureRequest_addTarget(req, raw_target_);
        apply_focus_entries(req);
        if (shots > 1)
            ACaptureRequest_setEntry_i32(req, ACAMERA_CONTROL_AE_EXPOSURE_COMPENSATION,
                                         1, &comp[std::min(i, 2)]);
        burst.push_back(req);
    }
    // Expect exactly `shots` result metadata deliveries; on_capture_completed
    // latches their measured exposure into still_expo_ in the same order.
    expect_expo_ = shots;

    ACameraCaptureSession_captureCallbacks cbs{};
    cbs.context            = this;
    cbs.onCaptureCompleted = on_capture_completed;
    const bool ok = ACameraCaptureSession_capture(session_, &cbs, nreq,
                                                  burst.data(), nullptr)
                    == ACAMERA_OK;
    for (auto* r : burst) ACaptureRequest_free(r);
    if (!ok) {
        expect_expo_ = 0;
        still_expo_.clear();
    }
    return ok;
}

// ── Callbacks ────────────────────────────────────────────────────────────────

void Session::on_preview_image(void* ctx, AImageReader* reader) {
    auto* self = static_cast<Session*>(ctx);
    AImage* img = nullptr;
    if (AImageReader_acquireLatestImage(reader, &img) != AMEDIA_OK || !img) return;

    AHardwareBuffer* hb = nullptr;
    if (AImage_getHardwareBuffer(img, &hb) != AMEDIA_OK || !hb) {
        AImage_delete(img);
        return;
    }
    if (!self->preview_sink_.on_frame) { AImage_delete(img); return; }
    // The buffer is owned by the image, so the image must outlive the renderer's
    // use of it — exactly the contract PreviewSink documents.
    self->preview_sink_.on_frame(hb, [img] { AImage_delete(img); });
}

void Session::on_raw_image(void* ctx, AImageReader* reader) {
    auto* self = static_cast<Session*>(ctx);
    AImage* img = nullptr;
    if (AImageReader_acquireNextImage(reader, &img) != AMEDIA_OK || !img) return;

    uint8_t* data = nullptr;
    int len = 0, stride = 0, w = 0, h = 0;
    int64_t ts = 0;
    AImage_getPlaneData(img, 0, &data, &len);
    AImage_getPlaneRowStride(img, 0, &stride);
    AImage_getWidth(img, &w);
    AImage_getHeight(img, &h);
    AImage_getTimestamp(img, &ts);

    if (data && len > 0) {
        if (self->recording_.load(std::memory_order_acquire)) {
            // RawVideoSink's contract: the pointer is valid only for this call,
            // so the consumer copies synchronously.
            if (self->raw_video_sink_.on_frame)
                self->raw_video_sink_.on_frame(data, w, h, stride, ts);
        } else {
            std::string path;
            int index = 0, total = 0;
            int64_t exposure_ns = 0;
            int iso = 0;
            {
                std::lock_guard<std::mutex> lk(self->still_mtx_);
                if (!self->still_paths_.empty()) {
                    path  = self->still_paths_.front();
                    self->still_paths_.erase(self->still_paths_.begin());
                    total = self->still_total_;
                    index = total - int(self->still_paths_.size()) - 1;
                }
                // The bracket's measured exposure for THIS frame, latched by
                // on_capture_completed in the same order frames are delivered.
                if (!self->still_expo_.empty()) {
                    exposure_ns = self->still_expo_.front().first;
                    iso         = self->still_expo_.front().second;
                    self->still_expo_.erase(self->still_expo_.begin());
                }
            }
            if (!path.empty() && self->raw_sink_.on_raw) {
                const float black[4] = {
                    self->static_meta_.black_level[0], self->static_meta_.black_level[1],
                    self->static_meta_.black_level[2], self->static_meta_.black_level[3] };
                float neutral[3] = {1.0f, 1.0f, 1.0f};
                const bool has_neutral =
                    self->have_neutral_.load(std::memory_order_acquire);
                if (has_neutral)
                    for (int k = 0; k < 3; ++k)
                        neutral[k] = self->last_neutral_[k].load(std::memory_order_relaxed);
                else
                    LOGE("ndk: still with no as-shot neutral - it will develop green");

                self->raw_sink_.on_raw(path.c_str(),
                                       reinterpret_cast<const uint16_t*>(data), w, h, stride,
                                       has_neutral ? neutral : nullptr,
                                       black, self->static_meta_.white_level,
                                       exposure_ns, iso, index, total);
            }
        }
    }
    AImage_delete(img);
}

// Non-RAW still delivery: copy the planes out synchronously (the camera needs
// its buffer back immediately), pair with the queued name FIFO, then hand the
// slow JXL encode to the worker thread.
void Session::on_still_image(void* ctx, AImageReader* reader) {
    auto* self = static_cast<Session*>(ctx);
    AImage* img = nullptr;
    if (AImageReader_acquireLatestImage(reader, &img) != AMEDIA_OK || !img) return;

    // The stream runs continuously in preview mode; without a queued name
    // there is nothing to develop — drop before the 18 MB copy.
    {
        std::lock_guard<std::mutex> lk(self->still_mtx_);
        if (self->still_paths_.empty()) { AImage_delete(img); return; }
    }

    StillJob j;
    uint8_t *yp = nullptr, *up = nullptr, *vp = nullptr;
    int ylen = 0, ulen = 0, vlen = 0;
    const bool ok =
        AImage_getWidth(img,  &j.w) == AMEDIA_OK &&
        AImage_getHeight(img, &j.h) == AMEDIA_OK &&
        AImage_getPlaneData(img, 0, &yp, &ylen) == AMEDIA_OK && yp && ylen > 0 &&
        AImage_getPlaneData(img, 1, &up, &ulen) == AMEDIA_OK && up && ulen > 0 &&
        AImage_getPlaneData(img, 2, &vp, &vlen) == AMEDIA_OK && vp && vlen > 0 &&
        AImage_getPlaneRowStride(img, 0, &j.y_stride) == AMEDIA_OK &&
        AImage_getPlaneRowStride(img, 1, &j.u_stride) == AMEDIA_OK &&
        AImage_getPlaneRowStride(img, 2, &j.v_stride) == AMEDIA_OK &&
        AImage_getPlanePixelStride(img, 1, &j.uv_pix) == AMEDIA_OK;

    if (ok) {
        j.y.assign(yp, yp + ylen);
        j.u.assign(up, up + ulen);
        j.v.assign(vp, vp + vlen);
        LOGI("ndk: still frame %dx%d yStride=%d uvPix=%d", j.w, j.h, j.y_stride, j.uv_pix);
        std::lock_guard<std::mutex> lk(self->still_mtx_);
        if (!self->still_paths_.empty()) {
            j.path = self->still_paths_.front();
            self->still_paths_.erase(self->still_paths_.begin());
        }
    }
    AImage_delete(img);

    if (!ok || j.path.empty()) {
        LOGE("ndk: YUV still dropped (copy=%d queued-name=%s)", ok,
             self->still_paths_.empty() ? "none" : "present");
        return;
    }
    {
        std::lock_guard<std::mutex> lk(self->still_job_mtx_);
        self->still_jobs_.push_back(std::move(j));
    }
    self->still_job_cv_.notify_one();
}

// Encodes queued YUV stills off the camera threads. Drains everything before
// exiting so a shot taken just before shutdown is not lost.
void Session::still_worker_loop() {
    for (;;) {
        StillJob j;
        {
            std::unique_lock<std::mutex> lk(still_job_mtx_);
            still_job_cv_.wait(lk, [this] {
                return still_worker_quit_ || !still_jobs_.empty();
            });
            if (still_jobs_.empty()) return;
            j = std::move(still_jobs_.front());
            still_jobs_.pop_front();
        }
        // Lossless PNG, not JXL. A device on this path has no RAW stream, so this
        // file IS the capture — there is no DNG behind it to fall back on, and it
        // has to open everywhere. Android cannot decode JXL (verified: the media
        // scanner reports width=NULL for a .jxl), so the format that was chosen
        // for size made the one deliverable these devices produce unviewable on
        // the device that shot it.
        cam::write_png_yuv420(j.path,
                              j.y.data(), j.u.data(), j.v.data(),
                              j.w, j.h, j.y_stride, j.u_stride, j.v_stride, j.uv_pix,
                              sensor_orientation_);
        // Register the deliverable even though this path never touched Java:
        // the host collects whatever the session reports.
        jni::session_record_file(j.path);
    }
}

void Session::on_capture_completed(void* ctx, ACameraCaptureSession*,
                                   ACaptureRequest*, const ACameraMetadata* result) {
    auto* self = static_cast<Session*>(ctx);

    // The bracket's real exposures. hdr_merge derives each frame's gain from
    // exposure×ISO when they are known and only falls back to nominal 2-EV
    // steps otherwise, so latching the measurement is what makes the merge
    // exact when AE lands off the requested compensation. Latched in burst
    // order (results for one capture() call arrive in request order) and
    // consumed FIFO by on_raw_image next to the path pop.
    {
        std::lock_guard<std::mutex> lk(self->still_mtx_);
        if (self->expect_expo_ > 0) {
            int64_t ns = 0;
            int iso = 0;
            ACameraMetadata_const_entry et{};
            if (ACameraMetadata_getConstEntry(result, ACAMERA_SENSOR_EXPOSURE_TIME, &et)
                    == ACAMERA_OK && et.count >= 1)
                ns = et.data.i64[0];
            ACameraMetadata_const_entry se{};
            if (ACameraMetadata_getConstEntry(result, ACAMERA_SENSOR_SENSITIVITY, &se)
                    == ACAMERA_OK && se.count >= 1)
                iso = se.data.i32[0];
            self->still_expo_.emplace_back(ns, iso);
            --self->expect_expo_;
        }
    }

    // Where the lens actually is. Read before the neutral bail-out below, so a
    // device that reports no neutral colour point still tracks focus: arming
    // manual focus adopts this value instead of snapping the lens to infinity.
    ACameraMetadata_const_entry lf{};
    if (ACameraMetadata_getConstEntry(result, ACAMERA_LENS_FOCUS_DISTANCE, &lf) == ACAMERA_OK &&
        lf.count > 0) {
        self->reported_diopters_.store(lf.data.f[0], std::memory_order_relaxed);
    }

    // The noise model for THIS result. Latched unconditionally, like the neutral
    // below: it varies with ISO, so the value that matters is the one from the
    // shot being written, not one read once at startup.
    ACameraMetadata_const_entry np{};
    if (ACameraMetadata_getConstEntry(result, ACAMERA_SENSOR_NOISE_PROFILE, &np) == ACAMERA_OK
        && np.count >= 2) {
        const int n = int(np.count) > 8 ? 8 : int(np.count);
        for (int i = 0; i < n; ++i)
            self->noise_profile_[i].store(np.data.d[i], std::memory_order_relaxed);
        self->noise_profile_count_.store(n, std::memory_order_release);
    }

    ACameraMetadata_const_entry e{};
    if (ACameraMetadata_getConstEntry(result, ACAMERA_SENSOR_NEUTRAL_COLOR_POINT, &e) != ACAMERA_OK
        || e.count < 3)
        return;
    // Reported as rationals; consumers want plain camera-space values.
    float neutral[3];
    for (int i = 0; i < 3; ++i) {
        const int32_t num = e.data.r[i].numerator, den = e.data.r[i].denominator;
        neutral[i] = den ? float(num) / float(den) : 1.0f;
    }

    // Keep the latest unconditionally — stills are shot while previewing, not
    // recording, and a still with no neutral develops green.
    for (int i = 0; i < 3; ++i)
        self->last_neutral_[i].store(neutral[i], std::memory_order_relaxed);
    self->have_neutral_.store(true, std::memory_order_release);

    // The video path wants exactly one per clip, taken once AWB is locked.
    if (!self->recording_.load(std::memory_order_acquire)) return;
    if (self->neutral_sent_.load(std::memory_order_acquire)) return;
    if (self->raw_video_sink_.on_neutral) self->raw_video_sink_.on_neutral(neutral);
    self->neutral_sent_.store(true, std::memory_order_release);
    LOGI("ndk: locked neutral %.4f %.4f %.4f", neutral[0], neutral[1], neutral[2]);
}

bool Session::noise_profile(double out[8], int& count) const {
    const int n = noise_profile_count_.load(std::memory_order_acquire);
    if (n <= 0) { count = 0; return false; }
    for (int i = 0; i < n; ++i) out[i] = noise_profile_[i].load(std::memory_order_relaxed);
    count = n;
    return true;
}

void Session::on_disconnected(void*, ACameraDevice*)      { LOGE("ndk: camera disconnected"); }
void Session::on_device_error(void*, ACameraDevice*, int e){ LOGE("ndk: camera error %d", e); }
void Session::on_session_ready(void*, ACameraCaptureSession*)  {}
void Session::on_session_active(void*, ACameraCaptureSession*) {}
void Session::on_session_closed(void* ctx, ACameraCaptureSession*) {
    LOGI("ndk: session closed");
    auto* self = static_cast<Session*>(ctx);
    if (!self) return;
    {
        std::lock_guard<std::mutex> lk(self->session_close_mtx_);
        self->session_closed_ = true;
    }
    self->session_close_cv_.notify_all();
}

void Session::close_session_and_wait() {
    if (!session_) return;
    {
        std::lock_guard<std::mutex> lk(session_close_mtx_);
        session_closed_ = false;
    }
    ACameraCaptureSession_stopRepeating(session_);
    ACameraCaptureSession_close(session_);
    session_ = nullptr;

    // Bounded: a HAL that never calls back must not deadlock the UI thread.
    // Proceeding after the timeout is no worse than not waiting at all.
    std::unique_lock<std::mutex> lk(session_close_mtx_);
    if (!session_close_cv_.wait_for(lk, std::chrono::milliseconds(400),
                                    [this] { return session_closed_; })) {
        LOGE("ndk: session close timed out; continuing");
    }
}

// ── Teardown ─────────────────────────────────────────────────────────────────

void Session::teardown_session() {
    close_session_and_wait();
    if (request_)        { ACaptureRequest_free(request_);                 request_ = nullptr; }
    if (preview_target_) { ACameraOutputTarget_free(preview_target_);      preview_target_ = nullptr; }
    if (raw_target_)     { ACameraOutputTarget_free(raw_target_);          raw_target_ = nullptr; }
    if (video_target_)   { ACameraOutputTarget_free(video_target_);        video_target_ = nullptr; }
    if (outputs_)        { ACaptureSessionOutputContainer_free(outputs_);  outputs_ = nullptr; }
    if (preview_output_) { ACaptureSessionOutput_free(preview_output_);    preview_output_ = nullptr; }
    if (raw_output_)     { ACaptureSessionOutput_free(raw_output_);        raw_output_ = nullptr; }
    if (video_output_)   { ACaptureSessionOutput_free(video_output_);      video_output_ = nullptr; }
}

void Session::shutdown() {
    recording_.store(false, std::memory_order_release);
    encoder_.stop();
    video_available_ = false;
    // Stop the JXL worker first; it touches no camera objects, but its queue
    // must be drained before the object can die. Pending shots flush.
    {
        std::lock_guard<std::mutex> lk(still_job_mtx_);
        still_worker_quit_ = true;
    }
    still_job_cv_.notify_all();
    if (still_worker_.joinable()) still_worker_.join();
    still_worker_quit_ = false;
    {
        std::lock_guard<std::mutex> lk(still_mtx_);
        still_paths_.clear();
    }
    teardown_session();
    release_still_stream();

    // Clear the listeners before deleting the readers, or a frame already in
    // flight can call back into a half-destroyed Session.
    if (preview_reader_) {
        AImageReader_setImageListener(preview_reader_, nullptr);
        AImageReader_delete(preview_reader_);
        preview_reader_ = nullptr;
    }
    if (raw_reader_) {
        AImageReader_setImageListener(raw_reader_, nullptr);
        AImageReader_delete(raw_reader_);
        raw_reader_ = nullptr;
    }
    if (device_)  { ACameraDevice_close(device_);   device_ = nullptr; }
    if (manager_) { ACameraManager_delete(manager_); manager_ = nullptr; }
    available_ = false;
}

} // namespace ndkcam
