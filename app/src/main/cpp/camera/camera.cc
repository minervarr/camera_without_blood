#include "camera.hh"

#include "../logger.hh"
#include <camera/NdkCameraMetadataTags.h>
#include <algorithm>
#include <vector>
#include <thread>
#include <chrono>
#undef LOG_TAG
#define LOG_TAG "Camera"

namespace cam {

// ── Helpers ────────────────────────────────────────────────────────────────────

struct StreamConfig {
    int32_t format;
    int32_t width;
    int32_t height;
};

// Parses ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS.
// Each entry is 4 int32: format, width, height, input(0=output/1=input).
static std::vector<StreamConfig> get_stream_configs(ACameraMetadata* meta) {
    ACameraMetadata_const_entry entry{};
    ACameraMetadata_getConstEntry(meta, ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS, &entry);

    std::vector<StreamConfig> configs;
    for (uint32_t i = 0; i + 3 < entry.count; i += 4) {
        int32_t fmt   = entry.data.i32[i + 0];
        int32_t w     = entry.data.i32[i + 1];
        int32_t h     = entry.data.i32[i + 2];
        int32_t input = entry.data.i32[i + 3];
        if (input == 0) configs.push_back({fmt, w, h});
    }
    return configs;
}

// From the available configs, pick the best resolution for a given format.
// "Best" = largest area, width preference among ties.
static bool pick_best_resolution(const std::vector<StreamConfig>& configs,
                                 int32_t target_format,
                                 int32_t& out_w, int32_t& out_h) {
    int64_t best_area = -1;
    for (auto& c : configs) {
        if (c.format != target_format) continue;
        int64_t area = (int64_t)c.width * c.height;
        if (area > best_area || (area == best_area && c.width > out_w)) {
            best_area = area;
            out_w = c.width;
            out_h = c.height;
        }
    }
    return best_area > 0;
}

// Picks the highest FIXED [min==max] AE target FPS range (e.g. [30,30]).
// Falls back to the range with the largest max. Returns false if none found.
static bool pick_max_fixed_fps_range(ACameraMetadata* meta, int32_t& out_min, int32_t& out_max) {
    ACameraMetadata_const_entry entry{};
    if (ACameraMetadata_getConstEntry(
            meta, ACAMERA_CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES, &entry) != ACAMERA_OK) {
        return false;
    }
    int32_t best_max = -1;
    bool best_fixed = false;
    for (uint32_t i = 0; i + 1 < entry.count; i += 2) {
        int32_t lo = entry.data.i32[i + 0];
        int32_t hi = entry.data.i32[i + 1];
        bool fixed = (lo == hi);
        // Prefer fixed ranges; among those, the highest max. Only consider a
        // non-fixed range if no fixed one has been found yet.
        bool better = (fixed && !best_fixed) ||
                      (fixed == best_fixed && hi > best_max);
        if (better) { best_max = hi; out_min = lo; out_max = hi; best_fixed = fixed; }
    }
    return best_max > 0;
}

// Dumps the device's relevant capture capabilities to the log so future work
// (RAW, higher fps) is data-driven and on-device negotiation is verifiable.
static void log_capabilities(ACameraMetadata* meta, const std::vector<StreamConfig>& configs) {
    ACameraMetadata_const_entry e{};

    if (ACameraMetadata_getConstEntry(meta, ACAMERA_REQUEST_AVAILABLE_CAPABILITIES, &e) == ACAMERA_OK) {
        bool has_raw = false;
        for (uint32_t i = 0; i < e.count; ++i)
            if (e.data.u8[i] == ACAMERA_REQUEST_AVAILABLE_CAPABILITIES_RAW) has_raw = true;
        LOGI("Capabilities: %u total, RAW=%s", e.count, has_raw ? "yes" : "no");
    }

    if (ACameraMetadata_getConstEntry(meta, ACAMERA_SENSOR_INFO_PIXEL_ARRAY_SIZE, &e) == ACAMERA_OK && e.count >= 2)
        LOGI("Sensor pixel array: %dx%d", e.data.i32[0], e.data.i32[1]);
    if (ACameraMetadata_getConstEntry(meta, ACAMERA_SENSOR_INFO_ACTIVE_ARRAY_SIZE, &e) == ACAMERA_OK && e.count >= 4)
        LOGI("Sensor active array: [%d %d %d %d]", e.data.i32[0], e.data.i32[1], e.data.i32[2], e.data.i32[3]);

    auto log_max = [&](int32_t fmt, const char* name) {
        int32_t w = 0, h = 0;
        if (pick_best_resolution(configs, fmt, w, h)) LOGI("Max %s: %dx%d", name, w, h);
        else LOGI("Max %s: (not supported)", name);
    };
    log_max(AIMAGE_FORMAT_YUV_420_888, "YUV_420_888");
    log_max(AIMAGE_FORMAT_RAW16,       "RAW16/RAW_SENSOR");

    if (ACameraMetadata_getConstEntry(meta, ACAMERA_CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES, &e) == ACAMERA_OK) {
        for (uint32_t i = 0; i + 1 < e.count; i += 2)
            LOGI("AE fps range: [%d %d]", e.data.i32[i], e.data.i32[i + 1]);
    }
}

// Reads a rational[9] static tag into a row-major double[9]. Returns true if present.
static bool read_matrix9(ACameraMetadata* meta, uint32_t tag, double out[9]) {
    ACameraMetadata_const_entry e{};
    if (ACameraMetadata_getConstEntry(meta, tag, &e) != ACAMERA_OK || e.count < 9) return false;
    for (int i = 0; i < 9; ++i) {
        int32_t den = e.data.r[i].denominator;
        out[i] = den ? double(e.data.r[i].numerator) / den : 0.0;
    }
    return true;
}

// ── Camera ─────────────────────────────────────────────────────────────────────

Camera::Camera()  = default;
Camera::~Camera() { close(); }

// Harvests the static DNG tags from the camera characteristics once at open().
void Camera::gather_static_meta(ACameraMetadata* meta) {
    dng::DngMeta& m = static_meta_;
    ACameraMetadata_const_entry e{};

    if (ACameraMetadata_getConstEntry(meta, ACAMERA_SENSOR_INFO_COLOR_FILTER_ARRANGEMENT, &e) == ACAMERA_OK && e.count >= 1)
        m.cfa = e.data.u8[0];

    if (ACameraMetadata_getConstEntry(meta, ACAMERA_SENSOR_INFO_WHITE_LEVEL, &e) == ACAMERA_OK && e.count >= 1)
        m.white_level = e.data.i32[0];

    if (ACameraMetadata_getConstEntry(meta, ACAMERA_SENSOR_BLACK_LEVEL_PATTERN, &e) == ACAMERA_OK && e.count >= 4)
        for (int i = 0; i < 4; ++i) m.black_level[i] = float(e.data.i32[i]);

    // Active array reported as [left, top, right, bottom]; store as [x, y, w, h].
    if (ACameraMetadata_getConstEntry(meta, ACAMERA_SENSOR_INFO_ACTIVE_ARRAY_SIZE, &e) == ACAMERA_OK && e.count >= 4) {
        m.active_xywh[0] = e.data.i32[0];
        m.active_xywh[1] = e.data.i32[1];
        m.active_xywh[2] = e.data.i32[2] - e.data.i32[0];
        m.active_xywh[3] = e.data.i32[3] - e.data.i32[1];
        m.has_active = true;
    }

    m.has_cm1 = read_matrix9(meta, ACAMERA_SENSOR_COLOR_TRANSFORM1,       m.color_matrix1);
    m.has_cm2 = read_matrix9(meta, ACAMERA_SENSOR_COLOR_TRANSFORM2,       m.color_matrix2);
    m.has_fm1 = read_matrix9(meta, ACAMERA_SENSOR_FORWARD_MATRIX1,        m.forward_matrix1);
    m.has_fm2 = read_matrix9(meta, ACAMERA_SENSOR_FORWARD_MATRIX2,        m.forward_matrix2);
    m.has_cc1 = read_matrix9(meta, ACAMERA_SENSOR_CALIBRATION_TRANSFORM1, m.calibration1);
    m.has_cc2 = read_matrix9(meta, ACAMERA_SENSOR_CALIBRATION_TRANSFORM2, m.calibration2);

    if (ACameraMetadata_getConstEntry(meta, ACAMERA_SENSOR_REFERENCE_ILLUMINANT1, &e) == ACAMERA_OK && e.count >= 1)
        m.illuminant1 = e.data.u8[0];
    if (ACameraMetadata_getConstEntry(meta, ACAMERA_SENSOR_REFERENCE_ILLUMINANT2, &e) == ACAMERA_OK && e.count >= 1)
        m.illuminant2 = e.data.u8[0];

    if (ACameraMetadata_getConstEntry(meta, ACAMERA_SENSOR_ORIENTATION, &e) == ACAMERA_OK && e.count >= 1)
        m.orientation_deg = e.data.i32[0];

    m.width  = raw_w_;
    m.height = raw_h_;
    LOGI("DNG static meta: cfa=%d white=%d cm1=%d fm1=%d illum1=%d orient=%d",
         m.cfa, m.white_level, m.has_cm1, m.has_fm1, m.illuminant1, m.orientation_deg);
}

bool Camera::open() {
    manager_ = ACameraManager_create();
    if (!manager_) { LOGE("ACameraManager_create failed"); return false; }

    ACameraIdList* id_list = nullptr;
    ACameraManager_getCameraIdList(manager_, &id_list);

    ACameraMetadata* chosen_meta = nullptr;

    for (int i = 0; i < id_list->numCameras; ++i) {
        const char* id = id_list->cameraIds[i];
        ACameraMetadata* meta = nullptr;
        ACameraManager_getCameraCharacteristics(manager_, id, &meta);

        ACameraMetadata_const_entry facing{};
        ACameraMetadata_getConstEntry(meta, ACAMERA_LENS_FACING, &facing);

        if (facing.data.u8[0] == ACAMERA_LENS_FACING_BACK) {
            camera_id_   = id;
            chosen_meta  = meta;
            break;
        }
        ACameraMetadata_free(meta);
    }
    ACameraManager_deleteCameraIdList(id_list);

    if (camera_id_.empty()) { LOGE("No back camera found"); return false; }

    // ── Format + resolution negotiation ───────────────────────────────────────
    auto configs = get_stream_configs(chosen_meta);
    log_capabilities(chosen_meta, configs);

    // Resolve the highest fixed framerate the sensor offers (e.g. [30,30]).
    if (pick_max_fixed_fps_range(chosen_meta, fps_min_, fps_max_))
        LOGI("Locking AE target fps range to [%d %d]", fps_min_, fps_max_);

    // Prefer YUV_420_888 (least-processed displayable format, closer to sensor
    // than ISP-converted RGBA) at the full sensor resolution; RGBA as fallback.
    if (pick_best_resolution(configs, AIMAGE_FORMAT_YUV_420_888, width_, height_)) {
        format_ = AIMAGE_FORMAT_YUV_420_888;
        LOGI("Format: YUV_420_888 %dx%d", width_, height_);
    } else if (pick_best_resolution(configs, AIMAGE_FORMAT_RGBA_8888, width_, height_)) {
        format_ = AIMAGE_FORMAT_RGBA_8888;
        LOGI("Format: RGBA_8888 %dx%d", width_, height_);
    } else {
        LOGE("No supported capture format found");
        ACameraMetadata_free(chosen_meta);
        return false;
    }

    // RAW_SENSOR (16-bit Bayer) stream for stills — the rawest the sensor offers.
    if (pick_best_resolution(configs, AIMAGE_FORMAT_RAW16, raw_w_, raw_h_)) {
        LOGI("RAW stream: RAW16 %dx%d", raw_w_, raw_h_);
        gather_static_meta(chosen_meta);
    } else {
        LOGE("Device has no RAW_SENSOR stream — photos will be unavailable");
    }

    ACameraMetadata_free(chosen_meta);

    // ── Open device ───────────────────────────────────────────────────────────
    dev_cbs_ = {};
    dev_cbs_.context        = this;
    dev_cbs_.onDisconnected = on_disconnected;
    dev_cbs_.onError        = on_error;

    // A previously-killed process may still be releasing the camera, so the
    // first open can transiently fail with CAMERA_DISCONNECTED/CAMERA_IN_USE.
    // Retry a couple of times with a short backoff before giving up.
    camera_status_t status = ACAMERA_ERROR_CAMERA_DISCONNECTED;
    for (int attempt = 0; attempt < 3; ++attempt) {
        status = ACameraManager_openCamera(manager_, camera_id_.c_str(), &dev_cbs_, &device_);
        if (status == ACAMERA_OK) break;

        if (status == ACAMERA_ERROR_CAMERA_DISCONNECTED ||
            status == ACAMERA_ERROR_CAMERA_IN_USE) {
            LOGE("openCamera transient failure %d (attempt %d), retrying...", status, attempt + 1);
            if (device_) { ACameraDevice_close(device_); device_ = nullptr; }
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            continue;
        }
        break;  // non-transient error
    }
    if (status != ACAMERA_OK) { LOGE("openCamera failed: %d", status); return false; }

    LOGI("Camera opened: %s", camera_id_.c_str());
    return true;
}

void Camera::close() {
    stop_capture();
    if (device_)  { ACameraDevice_close(device_);    device_  = nullptr; }
    if (manager_) { ACameraManager_delete(manager_); manager_ = nullptr; }
}

bool Camera::start_capture(FrameCallback cb) {
    if (capturing_) return true;
    callback_ = std::move(cb);

    // Image reader for preview — 4 buffer slots to avoid stalls
    if (AImageReader_new(width_, height_, format_, 4, &reader_) != AMEDIA_OK) {
        LOGE("AImageReader_new failed for preview");
        return false;
    }

    // Image reader for RAW_SENSOR stills — 2 buffer slots
    if (raw_w_ > 0 && raw_h_ > 0) {
        if (AImageReader_new(raw_w_, raw_h_, AIMAGE_FORMAT_RAW16, 2, &raw_reader_) != AMEDIA_OK) {
            LOGE("AImageReader_new failed for RAW16");
            AImageReader_delete(reader_); reader_ = nullptr;
            return false;
        }
    }

    AImageReader_ImageListener listener{this, on_image_available};
    AImageReader_setImageListener(reader_, &listener);

    ANativeWindow* window = nullptr;
    AImageReader_getWindow(reader_, &window);

    ACaptureSessionOutputContainer_create(&output_container_);

    ACaptureSessionOutput_create(window, &output_);
    ACaptureSessionOutputContainer_add(output_container_, output_);

    if (raw_reader_) {
        AImageReader_ImageListener raw_listener{this, on_raw_available};
        AImageReader_setImageListener(raw_reader_, &raw_listener);

        ANativeWindow* raw_window = nullptr;
        AImageReader_getWindow(raw_reader_, &raw_window);
        ACaptureSessionOutput_create(raw_window, &raw_output_);
        ACaptureSessionOutputContainer_add(output_container_, raw_output_);
    }

    // TEMPLATE_PREVIEW: best compatibility with AImageReader across devices
    ACameraDevice_createCaptureRequest(device_, TEMPLATE_PREVIEW, &request_);

    ANativeWindow_acquire(window);
    ACameraOutputTarget* target = nullptr;
    ACameraOutputTarget_create(window, &target);
    ACaptureRequest_addTarget(request_, target);

    // Disable post-processing to stay as close to sensor as possible
    uint8_t off = ACAMERA_NOISE_REDUCTION_MODE_OFF;
    ACaptureRequest_setEntry_u8(request_, ACAMERA_NOISE_REDUCTION_MODE, 1, &off);
    uint8_t sharp_off = ACAMERA_EDGE_MODE_OFF;
    ACaptureRequest_setEntry_u8(request_, ACAMERA_EDGE_MODE, 1, &sharp_off);

    // Lock to the highest fixed framerate so AE can't drop it (default floats to ~15).
    if (fps_max_ > 0) {
        int32_t fps_range[2] = {fps_min_, fps_max_};
        ACaptureRequest_setEntry_i32(request_, ACAMERA_CONTROL_AE_TARGET_FPS_RANGE, 2, fps_range);
    }

    sess_cbs_ = {};
    sess_cbs_.context  = this;
    sess_cbs_.onReady  = on_session_ready;
    sess_cbs_.onClosed = on_session_closed;

    ACameraDevice_createCaptureSession(device_, output_container_, &sess_cbs_, &session_);
    ACameraCaptureSession_setRepeatingRequest(session_, nullptr, 1, &request_, nullptr);

    capturing_ = true;
    LOGI("Capture started");
    return true;
}

bool Camera::take_photo(const std::string& output_path) {
    if (!capturing_ || !session_ || !device_ || !raw_reader_) return false;

    pending_photo_path_ = output_path;
    // Seed the per-shot metadata with the static tags; the capture result fills
    // in the dynamic ones (as-shot neutral, dynamic black/white level).
    pending_meta_ = static_meta_;

    ACaptureRequest* req = nullptr;
    if (ACameraDevice_createCaptureRequest(device_, TEMPLATE_STILL_CAPTURE, &req) != ACAMERA_OK) {
        LOGE("Failed to create STILL_CAPTURE request");
        return false;
    }

    // Keep the sensor data raw: no noise reduction / edge enhancement.
    uint8_t off = ACAMERA_NOISE_REDUCTION_MODE_OFF;
    ACaptureRequest_setEntry_u8(req, ACAMERA_NOISE_REDUCTION_MODE, 1, &off);
    uint8_t edge_off = ACAMERA_EDGE_MODE_OFF;
    ACaptureRequest_setEntry_u8(req, ACAMERA_EDGE_MODE, 1, &edge_off);

    ANativeWindow* raw_window = nullptr;
    AImageReader_getWindow(raw_reader_, &raw_window);

    ACameraOutputTarget* target = nullptr;
    ACameraOutputTarget_create(raw_window, &target);
    ACaptureRequest_addTarget(req, target);

    ACameraCaptureSession_captureCallbacks cb{};
    cb.context             = this;
    cb.onCaptureCompleted  = on_capture_completed;
    cb.onCaptureFailed     = on_capture_failed;

    if (ACameraCaptureSession_capture(session_, &cb, 1, &req, nullptr) != ACAMERA_OK) {
        LOGE("Failed to submit STILL_CAPTURE request");
        ACameraOutputTarget_free(target);
        ACaptureRequest_free(req);
        return false;
    }

    ACameraOutputTarget_free(target);
    ACaptureRequest_free(req);
    LOGI("RAW photo capture requested: %s", output_path.c_str());
    return true;
}

void Camera::stop_capture() {
    if (!capturing_) return;
    if (session_) {
        ACameraCaptureSession_stopRepeating(session_);
        ACameraCaptureSession_close(session_);
        session_ = nullptr;
    }
    if (request_)           { ACaptureRequest_free(request_);                         request_          = nullptr; }
    if (output_)            { ACaptureSessionOutput_free(output_);                    output_           = nullptr; }
    if (raw_output_)        { ACaptureSessionOutput_free(raw_output_);                raw_output_       = nullptr; }
    if (output_container_)  { ACaptureSessionOutputContainer_free(output_container_); output_container_ = nullptr; }
    if (reader_)            { AImageReader_delete(reader_);                           reader_           = nullptr; }
    if (raw_reader_)        { AImageReader_delete(raw_reader_);                       raw_reader_       = nullptr; }
    capturing_ = false;
    LOGI("Capture stopped");
}

// ── Static callbacks ───────────────────────────────────────────────────────────

void Camera::on_disconnected(void*, ACameraDevice*) {
    LOGE("Camera disconnected");
}

void Camera::on_error(void*, ACameraDevice*, int error) {
    LOGE("Camera error: %d", error);
}

void Camera::on_image_available(void* ctx, AImageReader* reader) {
    auto* self = reinterpret_cast<Camera*>(ctx);

    AImage* image = nullptr;
    if (AImageReader_acquireLatestImage(reader, &image) != AMEDIA_OK) return;

    int32_t w = 0, h = 0, fmt = 0;
    int64_t ts = 0;
    AImage_getWidth(image, &w);
    AImage_getHeight(image, &h);
    AImage_getTimestamp(image, &ts);
    AImage_getFormat(image, &fmt);

    // Caller takes ownership of the image and MUST call AImage_delete(image)
    if (self->callback_) self->callback_(image, FrameInfo{w, h, ts, fmt});
    else AImage_delete(image);
}

// Merges the per-shot dynamic DNG tags from the capture result into pending_meta_.
void Camera::on_capture_completed(void* ctx, ACameraCaptureSession*,
                                  ACaptureRequest*, const ACameraMetadata* result) {
    auto* self = reinterpret_cast<Camera*>(ctx);
    dng::DngMeta& m = self->pending_meta_;
    ACameraMetadata_const_entry e{};

    // As-shot neutral (white balance reference) — rational[3].
    if (ACameraMetadata_getConstEntry(result, ACAMERA_SENSOR_NEUTRAL_COLOR_POINT, &e) == ACAMERA_OK && e.count >= 3) {
        for (int i = 0; i < 3; ++i) {
            int32_t den = e.data.r[i].denominator;
            m.as_shot_neutral[i] = den ? double(e.data.r[i].numerator) / den : 1.0;
        }
        m.has_neutral = true;
    }

    // Per-shot dynamic black level (float[4]) is more accurate than the static pattern.
    if (ACameraMetadata_getConstEntry(result, ACAMERA_SENSOR_DYNAMIC_BLACK_LEVEL, &e) == ACAMERA_OK && e.count >= 4)
        for (int i = 0; i < 4; ++i) m.black_level[i] = e.data.f[i];

    if (ACameraMetadata_getConstEntry(result, ACAMERA_SENSOR_DYNAMIC_WHITE_LEVEL, &e) == ACAMERA_OK && e.count >= 1)
        m.white_level = e.data.i32[0];
}

void Camera::on_capture_failed(void* ctx, ACameraCaptureSession*,
                               ACaptureRequest*, ACameraCaptureFailure*) {
    auto* self = reinterpret_cast<Camera*>(ctx);
    LOGE("RAW capture failed");
    self->pending_photo_path_.clear();
}

void Camera::on_raw_available(void* ctx, AImageReader* reader) {
    auto* self = reinterpret_cast<Camera*>(ctx);

    AImage* image = nullptr;
    if (AImageReader_acquireNextImage(reader, &image) != AMEDIA_OK) return;

    int32_t len = 0, stride = 0;
    uint8_t* data = nullptr;
    if (AImage_getPlaneData(image, 0, &data, &len) == AMEDIA_OK && data && len > 0 &&
        !self->pending_photo_path_.empty()) {
        AImage_getPlaneRowStride(image, 0, &stride);

        dng::DngMeta m = self->pending_meta_;
        AImage_getWidth(image, &m.width);
        AImage_getHeight(image, &m.height);
        m.stride_bytes = stride;

        if (!dng::write_dng(self->pending_photo_path_,
                            reinterpret_cast<const uint16_t*>(data), m)) {
            LOGE("Failed to write DNG: %s", self->pending_photo_path_.c_str());
        }
        self->pending_photo_path_.clear();
    } else if (self->pending_photo_path_.empty()) {
        // stray frame, ignore
    } else {
        LOGE("Failed to get RAW plane data");
        self->pending_photo_path_.clear();
    }

    AImage_delete(image);
}

void Camera::on_session_ready(void*, ACameraCaptureSession*) { LOGI("Session ready"); }
void Camera::on_session_closed(void*, ACameraCaptureSession*) { LOGI("Session closed"); }

} // namespace cam
