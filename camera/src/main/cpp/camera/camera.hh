#pragma once

#include <camera/NdkCameraManager.h>
#include <camera/NdkCameraDevice.h>
#include <camera/NdkCameraCaptureSession.h>
#include <media/NdkImageReader.h>

#include "dng_writer.hh"

#include <cstdint>
#include <functional>
#include <string>

namespace cam {

struct FrameInfo {
    int32_t  width;
    int32_t  height;
    int64_t  timestamp_ns;
    int32_t  format;  // AIMAGE_FORMAT_RAW_SENSOR or AIMAGE_FORMAT_YUV_420_888
};

using FrameCallback = std::function<void(AImage*, const FrameInfo&)>;

class Camera {
public:
    Camera();
    ~Camera();

    // Opens the first back-facing camera. Returns false on failure.
    bool open();
    void close();

    // Starts capture. Frames are delivered on a background thread via cb.
    bool start_capture(FrameCallback cb);
    void stop_capture();

    // Captures a full RAW_SENSOR frame and writes it as a DNG to the given path.
    // Does not interrupt the preview feed.
    bool take_photo(const std::string& output_path);

    bool is_capturing() const { return capturing_; }

    int32_t width()  const { return width_; }
    int32_t height() const { return height_; }

private:
    static void on_disconnected(void* ctx, ACameraDevice* dev);
    static void on_error(void* ctx, ACameraDevice* dev, int error);
    static void on_image_available(void* ctx, AImageReader* reader);
    static void on_raw_available(void* ctx, AImageReader* reader);
    static void on_session_ready(void* ctx, ACameraCaptureSession* session);
    static void on_session_closed(void* ctx, ACameraCaptureSession* session);

    // Still-capture result callback: harvests the per-shot dynamic DNG tags.
    static void on_capture_completed(void* ctx, ACameraCaptureSession* session,
                                     ACaptureRequest* request,
                                     const ACameraMetadata* result);
    static void on_capture_failed(void* ctx, ACameraCaptureSession* session,
                                  ACaptureRequest* request, ACameraCaptureFailure* failure);

    // Fills static_meta_ from the camera characteristics (called once at open()).
    void gather_static_meta(ACameraMetadata* meta);

    ACameraManager*        manager_  = nullptr;
    ACameraDevice*         device_   = nullptr;
    ACameraCaptureSession* session_  = nullptr;
    AImageReader*          reader_   = nullptr;
    AImageReader*          raw_reader_ = nullptr;
    ACaptureRequest*       request_  = nullptr;
    ACaptureSessionOutput* output_   = nullptr;
    ACaptureSessionOutput* raw_output_ = nullptr;
    ACaptureSessionOutputContainer* output_container_ = nullptr;

    FrameCallback callback_;
    bool          capturing_ = false;
    int32_t       width_     = 0;
    int32_t       height_    = 0;
    int32_t       format_    = 0;  // resolved at open()
    int32_t       raw_w_     = 0;  // RAW_SENSOR stream size, resolved at open()
    int32_t       raw_h_     = 0;
    int32_t       fps_min_   = 0;  // AE target fps range, resolved at open()
    int32_t       fps_max_   = 0;

    std::string   camera_id_;
    std::string   pending_photo_path_;

    // DNG metadata: static fields resolved at open(); dynamic (as-shot neutral,
    // black/white level) merged in from each capture result before the RAW
    // buffer is written.
    dng::DngMeta  static_meta_;
    dng::DngMeta  pending_meta_;

    ACameraDevice_StateCallbacks         dev_cbs_{};
    ACameraCaptureSession_stateCallbacks sess_cbs_{};
};

} // namespace cam
