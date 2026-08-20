#include "ndk_session.hh"

#include "../logger.hh"

#include <android/hardware_buffer.h>

#include <algorithm>
#include <cstring>

namespace ndkcam {
namespace {

constexpr int kPreviewBuffers = 4;
// The ISP consumes frames slower than the sensor produces them, so the reader
// needs enough depth to absorb a burst without the HAL stalling the stream.
constexpr int kRawBuffers     = 6;

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

                pick_fps_range(meta, fps_min_, fps_max_);
                camera_id_ = ids->cameraIds[i];
                found = true;

                // MANUAL_POST_PROCESSING lets the video path take the HAL's
                // baked-in contrast curve out of the loop (flat tonemap, NR
                // off) — the concrete image-quality win over the Java path.
                has_manual_post_ = has_capability(
                    meta, ACAMERA_REQUEST_AVAILABLE_CAPABILITIES_MANUAL_POST_PROCESSING);

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

                // Reuse the DNG metadata gatherer's tag set by filling here.
                ACameraMetadata_const_entry q{};
                if (ACameraMetadata_getConstEntry(meta, ACAMERA_SENSOR_INFO_COLOR_FILTER_ARRANGEMENT, &q) == ACAMERA_OK && q.count >= 1)
                    static_meta_.cfa = q.data.u8[0];
                if (ACameraMetadata_getConstEntry(meta, ACAMERA_SENSOR_INFO_WHITE_LEVEL, &q) == ACAMERA_OK && q.count >= 1)
                    static_meta_.white_level = q.data.i32[0];
                if (ACameraMetadata_getConstEntry(meta, ACAMERA_SENSOR_BLACK_LEVEL_PATTERN, &q) == ACAMERA_OK && q.count >= 4)
                    for (int k = 0; k < 4; ++k) static_meta_.black_level[k] = float(q.data.i32[k]);
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
        LOGI("ndk: camera %s  no RAW  preview %dx%d  fps [%d,%d]  manual-post=%d  %zu video sizes",
             camera_id_.c_str(), prev_w_, prev_h_, fps_min_, fps_max_,
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

    if (!configure_session()) { shutdown(); return false; }
    available_ = true;
    return true;
}

bool Session::configure_session() {
    // Preview: PRIVATE + GPU_SAMPLED so the AHardwareBuffer imports straight
    // into the Vulkan renderer with no copy.
    if (AImageReader_newWithUsage(prev_w_, prev_h_, AIMAGE_FORMAT_PRIVATE,
                                  AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE,
                                  kPreviewBuffers, &preview_reader_) != AMEDIA_OK) {
        LOGE("ndk: preview AImageReader failed");
        return false;
    }
    preview_listener_ = { this, on_preview_image };
    AImageReader_setImageListener(preview_reader_, &preview_listener_);
    if (AImageReader_getWindow(preview_reader_, &preview_window_) != AMEDIA_OK) return false;

    const bool raw_path = raw_w_ > 0;
    if (raw_path) {
        if (AImageReader_new(raw_w_, raw_h_, AIMAGE_FORMAT_RAW16, kRawBuffers,
                             &raw_reader_) != AMEDIA_OK) {
            LOGE("ndk: RAW16 AImageReader failed");
            return false;
        }
        raw_listener_ = { this, on_raw_image };
        AImageReader_setImageListener(raw_reader_, &raw_listener_);
        if (AImageReader_getWindow(raw_reader_, &raw_window_) != AMEDIA_OK) return false;
    } else {
        // No RAW: the encoder's own input surface becomes the video stream, so
        // the camera writes encodable frames directly and nothing is copied.
        if (video_sizes_.empty()) {
            LOGI("ndk: no video sizes at the sensor aspect");
            return false;
        }
        const int32_t fps = fps_max_ > 0 ? fps_max_ : 30;
        if (!encoder_.configure(video_sizes_.data(),
                                static_cast<int>(video_sizes_.size()), fps, record_sink_))
            return false;
        video_available_ = true;
    }

    // Every stream stays configured for the whole session; recording only
    // changes which targets the repeating request carries, so REC never
    // reconfigures the session.
    if (ACaptureSessionOutputContainer_create(&outputs_) != ACAMERA_OK) return false;
    if (ACaptureSessionOutput_create(preview_window_, &preview_output_) != ACAMERA_OK) return false;
    ACaptureSessionOutputContainer_add(outputs_, preview_output_);
    if (raw_path) {
        if (ACaptureSessionOutput_create(raw_window_, &raw_output_) != ACAMERA_OK) return false;
        ACaptureSessionOutputContainer_add(outputs_, raw_output_);
    } else {
        if (ACaptureSessionOutput_create(encoder_.input_surface(), &video_output_) != ACAMERA_OK)
            return false;
        ACaptureSessionOutputContainer_add(outputs_, video_output_);
    }

    sess_cbs_.context   = this;
    sess_cbs_.onReady   = on_session_ready;
    sess_cbs_.onActive  = on_session_active;
    sess_cbs_.onClosed  = on_session_closed;
    if (ACameraDevice_createCaptureSession(device_, outputs_, &sess_cbs_, &session_) != ACAMERA_OK) {
        LOGE("ndk: createCaptureSession failed");
        return false;
    }

    if (ACameraOutputTarget_create(preview_window_, &preview_target_) != ACAMERA_OK) return false;
    if (raw_path) {
        if (ACameraOutputTarget_create(raw_window_, &raw_target_) != ACAMERA_OK) return false;
    } else {
        if (ACameraOutputTarget_create(encoder_.input_surface(), &video_target_) != ACAMERA_OK)
            return false;
    }
    return true;
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

    if (fps_max_ > 0) {
        const int32_t fps[2] = { fps_min_, fps_max_ };
        ACaptureRequest_setEntry_i32(request_, ACAMERA_CONTROL_AE_TARGET_FPS_RANGE, 2, fps);
    }
    if (with_raw && raw_target_) {
        // One white balance per clip: lock AWB so the ISP's gains stay valid for
        // the whole recording, and report the locked neutral once.
        const uint8_t lock = 1;
        ACaptureRequest_setEntry_u8(request_, ACAMERA_CONTROL_AWB_LOCK, 1, &lock);
        neutral_sent_.store(false, std::memory_order_release);
    }
    if (!raw_target_ && has_manual_post_) {
        // The video path's actual image-quality win: take the HAL's baked-in
        // contrast curve and its edge/noise processing out of the loop. A
        // straight-line tonemap is the flattest curve the API can express, and
        // it is what makes the recording gradeable instead of pre-cooked.
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

bool Session::start_recording() {
    // The encoder must be running before the camera starts filling its surface.
    if (video_available_ && !encoder_.start()) return false;
    if (!build_repeating(true)) return false;
    recording_.store(true, std::memory_order_release);
    return true;
}

void Session::stop_recording() {
    recording_.store(false, std::memory_order_release);
    build_repeating(false);
    if (video_available_) encoder_.stop();
}

bool Session::take_photo(const char* base_path, int shots) {
    if (!base_path || shots < 1) return false;
    if (recording_.load(std::memory_order_acquire)) {
        LOGE("ndk: takePhoto refused while recording");
        return false;
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
    ACaptureRequest* still = nullptr;
    if (ACameraDevice_createCaptureRequest(device_, TEMPLATE_STILL_CAPTURE, &still) != ACAMERA_OK)
        return false;
    ACaptureRequest_addTarget(still, raw_target_);

    std::vector<ACaptureRequest*> burst(static_cast<size_t>(shots), still);
    ACameraCaptureSession_captureCallbacks cbs{};
    cbs.context            = this;
    cbs.onCaptureCompleted = on_capture_completed;
    const bool ok = ACameraCaptureSession_capture(session_, &cbs, shots, burst.data(), nullptr)
                    == ACAMERA_OK;
    ACaptureRequest_free(still);
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
            {
                std::lock_guard<std::mutex> lk(self->still_mtx_);
                if (!self->still_paths_.empty()) {
                    path  = self->still_paths_.front();
                    self->still_paths_.erase(self->still_paths_.begin());
                    total = self->still_total_;
                    index = total - int(self->still_paths_.size()) - 1;
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
                                       0, 0, index, total);
            }
        }
    }
    AImage_delete(img);
}

void Session::on_capture_completed(void* ctx, ACameraCaptureSession*,
                                   ACaptureRequest*, const ACameraMetadata* result) {
    auto* self = static_cast<Session*>(ctx);

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

void Session::on_disconnected(void*, ACameraDevice*)      { LOGE("ndk: camera disconnected"); }
void Session::on_device_error(void*, ACameraDevice*, int e){ LOGE("ndk: camera error %d", e); }
void Session::on_session_ready(void*, ACameraCaptureSession*)  {}
void Session::on_session_active(void*, ACameraCaptureSession*) {}
void Session::on_session_closed(void*, ACameraCaptureSession*) { LOGI("ndk: session closed"); }

// ── Teardown ─────────────────────────────────────────────────────────────────

void Session::teardown_session() {
    if (session_) {
        ACameraCaptureSession_stopRepeating(session_);
        ACameraCaptureSession_close(session_);
        session_ = nullptr;
    }
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
    teardown_session();

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
