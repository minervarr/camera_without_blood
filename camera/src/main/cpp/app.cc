#include "app.hh"
#include <android/asset_manager.h>
#include "permissions.hh"
#include "android_platform.hh"
#include "renderer.hh"
#include "canvas.hh"
#include "ui/ui.hh"
#include "recorder/recorder.hh"
#include "jni/jni_camera.hh"
#include "archive.hh"

#include <iomanip>
#include <sstream>
#include <chrono>

#include "logger.hh"
#undef LOG_TAG
#define LOG_TAG "CameraApp"

// Orientation sensor looper ID: must be >4 to avoid colliding with
// AndroidHost's eventFd(3) and timerFd(4).
static constexpr int kOrientationLooperId = 5;

App::App(std::unique_ptr<Host> host) : host_(std::move(host)) {
    // Embedded-scene launch options from the host CameraActivity (safe no-ops
    // under a plain NativeActivity). Start each scene with a clean capture list.
    JavaVM* vm   = host_->javaVm();
    jobject act  = static_cast<jobject>(host_->activityObject());
    jni::session_files_reset();
    ui_video_mode_         = jni::host_start_video(vm, act);
    ui_photo_mode_         = jni::host_photo_mode(vm, act);
    ui_photo_mode_ui_enabled_ = jni::host_photo_mode_ui(vm, act);
    orientation_.start(kOrientationLooperId);
}

const std::string& App::output_base() {
    if (out_base_.empty()) {
        JavaVM* vm  = host_->javaVm();
        jobject act = static_cast<jobject>(host_->activityObject());
        // Host-provided, scoped-storage-safe dir (CameraActivity.cameraOutputDir).
        out_base_ = jni::host_output_dir(vm, act);
        // Fallback for a non-CameraActivity host: legacy public Documents.
        if (out_base_.empty()) {
            out_base_ = archive::get_documents_path(vm, act, "camera_without_blood");
        }
    }
    return out_base_;
}

void App::maybe_finish_session() {
    if (!exit_requested_ || results_sent_) return;
    if (recorder_) {
        rec::State s = recorder_->state();
        if (s == rec::State::SAVING) {
            recorder_->stop_saving();   // -> FINALIZING (offline drain)
            return;                     // wait for finalize before returning files
        }
        if (s == rec::State::FINALIZING) {
            return;                     // still draining; retry next tick (Processing…%)
        }
    }
    // PREVIEW/IDLE (or no recorder): the session is settled — hand the captured
    // files back to the host and finish the scene. Done exactly once.
    results_sent_ = true;
    JavaVM* vm  = host_->javaVm();
    jobject act = static_cast<jobject>(host_->activityObject());
    jni::host_finish_with_results(vm, act, jni::session_files());
}

App::~App() {
    orientation_.stop();
    destroy_recorder();
    destroy_vulkan();
}

// ── Event loop ───────────────────────────────────────────────────────────────

void App::run() {
    while (!host_->quitRequested()) {
        bool finalizing = recorder_ && recorder_->state() == rec::State::FINALIZING;

        // host_->pump() blocks if !haveWork, returns immediately otherwise.
        // While finalizing we need periodic redraws for "Processing…%" — use a
        // 100ms repeating timer so pump() wakes us even when there is no camera
        // frame.
        bool haveWork = ui_repaint_frames_ > 0 || finalizing;
        host_->pump(haveWork);

        // Back pressed: drive the graceful exit (finalize if needed, then return
        // the captured files to the host and finish). While finalizing, the loop
        // keeps ticking via the finalizing timer and shows "Processing…%".
        maybe_finish_session();

        // Don't present while recording: the swapchain present periodically stalls
        // the ISP GPU compute ~90ms (the compositor holds our image), dropping
        // frames. With the preview also dropped from the capture request, this loop
        // mostly idles during recording — the GPU is the native ISP's.
        // EXCEPTION: the few repaints we owe a freshly-recreated swapchain after
        // returning to the foreground, so the Stop button is actually on screen
        // (returning from background mid-recording would otherwise show black).
        bool ready  = renderer_ && renderer_->consume_frame_ready();
        bool saving = recorder_ && recorder_->state() == rec::State::SAVING;
        if (renderer_ && (ui_repaint_frames_ > 0 || (!saving && (ready || finalizing)))) {
            draw_frame();
            if (ui_repaint_frames_ > 0) --ui_repaint_frames_;
        }
    }
}

// ── AppView callbacks ────────────────────────────────────────────────────────

void App::onHostResized() {
    // Surface dimensions changed — recreate the swapchain (handled by
    // onSurfaceRecreated which is fired by the host for actual surface changes).
    // A resize that is NOT a new surface (Wayland configure) just needs a
    // redraw.
    if (renderer_) {
        ui_repaint_frames_ = 1;
    }
}

void App::shutdown() {
    destroy_recorder();
}

void App::onSurfaceLost() {
    destroy_vulkan();
}

bool App::onSurfaceRecreated() {
    if (!renderer_) {
        // Check permissions on first surface creation.
        if (!permissions_granted_) {
            permissions_granted_ = has_permissions(host_.get());
        }
        if (!permissions_granted_ && !permissions_requested_) {
            request_permissions(host_.get());
            permissions_requested_ = true;
        }
        init_vulkan();
    }
    return renderer_ != nullptr;
}

void App::onAppBackgrounded() {
    resumed_ = false;
    orientation_.disable();
    host_->stopTimer(TIMER_FINALIZE);
    if (recorder_ && recorder_->state() != rec::State::SAVING &&
        recorder_->state() != rec::State::FINALIZING) {
        stop_recording();
    }
}

void App::onAppForegrounded() {
    // App is in the foreground: (re)acquire the camera + orientation sensor.
    resumed_ = true;
    orientation_.enable();
    // Re-check permissions in case the user just answered the dialog.
    if (!permissions_granted_) {
        permissions_granted_ = has_permissions(host_.get());
    }
    maybe_start_recording();
    ui_repaint_frames_ = 4;
}

void App::onLButtonDown(int x, int y) {
    if (!ui_ || !recorder_) return;
    // Lock the UI while the previous clip is finalizing — no new recording
    // and no mode switch until the file is written.
    if (recorder_->state() == rec::State::FINALIZING) return;

    ui::UI::Action act = ui_->on_touch(x, y);
    if (act == ui::UI::Action::SHUTTER) {
        auto now = std::chrono::system_clock::now();
        std::time_t now_c = std::chrono::system_clock::to_time_t(now);
        std::stringstream ss;
        ss << std::put_time(std::localtime(&now_c), "%Y%m%d_%H%M%S");
        std::string ts = ss.str();

        const std::string& base = output_base();
        if (base.empty()) {
            LOGE("Failed to resolve output directory");
            return;
        }
        if (ui_->is_video_mode()) {
            if (recorder_->state() == rec::State::SAVING) {
                recorder_->stop_saving();
            } else {
                std::string vdir = base + "/video";
                archive::ensure_dir(vdir);
                std::string out_path = vdir + "/VID_" + ts + ".mkv";
                recorder_->start_saving(out_path);
                jni::session_record_file(out_path);
            }
        } else {
            std::string pdir = base + "/photo";
            archive::ensure_dir(pdir);
            std::string out_path = pdir + "/IMG_" + ts;
            recorder_->take_photo(out_path);
        }
    } else if (act == ui::UI::Action::TOGGLE_MODE) {
        if (recorder_ &&
            recorder_->state() != rec::State::SAVING &&
            recorder_->state() != rec::State::FINALIZING) {
            recorder_->set_photo_mode(!ui_->is_video_mode());
        }
    } else if (act == ui::UI::Action::CYCLE_PHOTO_MODE) {
        ui_photo_mode_ = (ui_photo_mode_ + 1) % 3;
        recorder_->set_photo_output_mode(ui_photo_mode_);
        ui_->set_photo_mode_index(ui_photo_mode_);
    }
}

void App::onNavBack() {
    exit_requested_ = true;
}

void App::onTimer(int timerId) {
    if (timerId == TIMER_FINALIZE) {
        // Finalizing timer: keep the UI updating "Processing…%". If finalize
        // completes, stop the timer.
        if (!recorder_ || recorder_->state() != rec::State::FINALIZING) {
            host_->stopTimer(TIMER_FINALIZE);
        }
    }
}

// ── Internal helpers ─────────────────────────────────────────────────────────

void App::init_vulkan() {
    // The Renderer owns the VkSurface/swapchain and is the ONLY thing tied to the
    // window surface. It may be created and destroyed many times during the app's
    // life (surface destroyed/recreated on rotate, lock/unlock, lifecycle churn).
    // The camera/recorder is deliberately NOT tied to this lifecycle.
    renderer_ = new Renderer(host_->surfaceProvider(), host_->assetReader());

    // Load the overlay font once (used for the UI text). Not tied to the surface
    // lifecycle, so only load on the first init.
    if (!font_loaded_) {
        std::vector<uint8_t> fontData;
        if (host_->dataReader().read("fonts/font.otf", fontData) && !fontData.empty()) {
            font_loaded_ = overlay_font_.loadFromMemory(fontData.data(), fontData.size());
            LOGI("Overlay font load: %s", font_loaded_ ? "ok" : "FAILED");
        } else {
            LOGE("fonts/font.otf not found");
        }
    }

    canvas_   = new Canvas(canvas_data_, renderer_->width(), renderer_->height(),
                           font_loaded_ ? &overlay_font_ : nullptr, 0.0f, 0.0f, 0.0f, 0.0f);
    ui_       = new ui::UI(*canvas_);
    // Restore the photo/video toggle, and force VIDEO whenever a recording is in
    // progress (SAVING/FINALIZING) so the Stop button shows after returning from
    // the background mid-recording.
    {
        bool vm = ui_video_mode_;
        if (recorder_) {
            rec::State s = recorder_->state();
            if (s == rec::State::SAVING || s == rec::State::FINALIZING) vm = true;
        }
        ui_->set_video_mode(vm);
    }
    ui_->set_photo_mode_ui_enabled(ui_photo_mode_ui_enabled_);
    ui_->set_photo_mode_index(ui_photo_mode_);
    LOGI("Renderer/UI initialized");

    // If the camera is already running (renderer was recreated), re-point it at
    // the new renderer so frames composite into the new swapchain.
    if (recorder_) {
        recorder_->set_renderer(renderer_);
    }

    // The swapchain is brand-new (blank). Repaint the UI for a few frames so the
    // controls are on screen even mid-recording (when run() otherwise won't draw).
    ui_repaint_frames_ = 4;

    maybe_start_recording();
}

void App::maybe_start_recording() {
    // The camera is tied to the FOREGROUND lifecycle (resumed_), NOT to the
    // window surface: a backgrounded app is not allowed to hold the camera, so
    // Android force-revokes it (camera "green dot" off + a device-error storm).
    // We therefore open the camera on resume and release it on pause. The
    // renderer may not exist yet here — that's fine, frames are simply dropped
    // until init_vulkan() re-points the recorder at the new renderer.
    if (recording_started_ || !permissions_granted_ || !resumed_) {
        return;
    }

    if (!recorder_) {
        JavaVM* vm  = host_->javaVm();
        jobject act = static_cast<jobject>(host_->activityObject());
        // Recorder needs the raw AAssetManager for loading ML models from APK assets.
        auto& assetReader = static_cast<AndroidAssetReader&>(host_->assetReader());
        recorder_ = new rec::Recorder(assetReader.aassetManager(), vm, act);
    }

    recorder_->set_capture_orientation(orientation_.degrees());
    if (recorder_->start_preview(renderer_)) {
        recording_started_ = true;
        // Bring the session up matching the persisted PHOTO/VIDEO toggle (default
        // PHOTO). The flag is read when the async session (re)configures.
        recorder_->set_photo_mode(!ui_video_mode_);
        recorder_->set_photo_output_mode(ui_photo_mode_);
        LOGI("Preview started (orientation %d)", orientation_.degrees());
    } else {
        LOGE("Failed to start preview: %s", recorder_->last_error().c_str());
    }
}

void App::stop_recording() {
    // Cleanly release the camera (and audio/muxer) while we are backgrounded, so
    // the system does not have to forcibly revoke it. Reacquired on resume.
    if (recorder_ && recording_started_) {
        recorder_->stop_preview();
        LOGI("Preview stopped (app backgrounded; camera released)");
    }
    recording_started_ = false;
}

void App::destroy_vulkan() {
    // Detach the still-running camera from the renderer BEFORE the renderer is
    // deleted, and drop any camera frames it is holding, so no in-flight frame
    // binds into a destroyed Vulkan device.
    if (recorder_) recorder_->set_renderer(nullptr);
    if (renderer_) renderer_->clear_camera_frames();

    if (ui_) ui_video_mode_ = ui_->is_video_mode();   // remember toggle across rebuild
    delete ui_;       ui_       = nullptr;
    delete canvas_;   canvas_   = nullptr;
    delete renderer_; renderer_ = nullptr;
    LOGI("Renderer/UI destroyed (camera left running)");
}

void App::destroy_recorder() {
    if (!recorder_) return;
    recorder_->set_renderer(nullptr);
    delete recorder_;
    recorder_ = nullptr;
    recording_started_ = false;
    LOGI("Recorder destroyed");
}

void App::draw_frame() {
    // Rebuild the overlay geometry from scratch each frame.
    canvas_data_.clear();
    if (recorder_) ui_->update(*recorder_, orientation_.degrees());
    renderer_->draw(canvas_data_, 0);
}
