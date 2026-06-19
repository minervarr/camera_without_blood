#include "app.hh"
#include <android/asset_manager.h>
#include "permissions.hh"
#include "fullscreen.hh"
#include "renderer.hh"
#include "canvas.hh"
#include "ui/ui.hh"
#include "recorder/recorder.hh"
#include "archive.hh"

#include <iomanip>
#include <sstream>
#include <chrono>

#include "logger.hh"
#undef LOG_TAG
#define LOG_TAG "CameraApp"

App::App(android_app* state) : state_(state) {
    state_->userData = this;
    state_->onAppCmd = handle_cmd;
    state_->onInputEvent = handle_input;
    orientation_.start();  // sensor queue on this (glue) thread's looper
}

App::~App() {
    orientation_.stop();
    destroy_recorder();
    destroy_vulkan();
}

void App::run() {
    while (true) {
        int events;
        android_poll_source* source;

        // Block until a camera frame (or input/sensor/system event) wakes us.
        // Camera frames call ALooper_wake() from update_camera_frame(); input and
        // orientation sensor events arrive on their own fds. Using -1 (block
        // forever) stops us from presenting duplicate frames on a timer, which was
        // what made SurfaceFlinger hunt the LTPO refresh rate and stall
        // vkWaitForFences ~90ms once per second.
        // While finalizing there are no camera frames to wake us, but the UI must
        // keep refreshing the "Processing…%" counter — poll on a short timeout so
        // we redraw periodically. Otherwise block until the next frame/event (-1).
        bool finalizing = recorder_ && recorder_->state() == rec::State::FINALIZING;
        int ident;
        // Idle: block until the next frame/event (-1). While finalizing, tick on a
        // 100ms timeout to advance "Processing…%". While we still owe the surface a
        // few post-resume UI repaints, spin (0) so we draw them promptly.
        int timeout_ms = (ui_repaint_frames_ > 0) ? 0 : (finalizing ? 100 : -1);
        while ((ident = ALooper_pollOnce(timeout_ms, nullptr, &events,
                                reinterpret_cast<void**>(&source))) >= 0) {
            if (source) source->process(state_, source);
            if (ident == Orientation::LOOPER_ID) orientation_.handleEvents();
            if (state_->destroyRequested) return;
            timeout_ms = 0;  // drain any remaining events without blocking
        }

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

void App::init_vulkan() {
    // The Renderer owns the VkSurface/swapchain and is the ONLY thing tied to the
    // window surface. It may be created and destroyed many times during the app's
    // life (surface destroyed/recreated on rotate, lock/unlock, lifecycle churn).
    // The camera/recorder is deliberately NOT tied to this lifecycle.
    renderer_ = new Renderer(state_->window, state_->activity->assetManager);
    // Let the camera callback thread wake this (main) thread's looper when a new
    // frame arrives, so run() can present at the camera's cadence (see App::run).
    renderer_->set_wake_looper(ALooper_forThread());

    // Load the overlay font once (used for the UI text). Not tied to the surface
    // lifecycle, so only load on the first init.
    if (!font_loaded_) {
        AAssetManager* mgr = state_->activity->assetManager;
        AAsset* a = AAssetManager_open(mgr, "fonts/font.otf", AASSET_MODE_BUFFER);
        if (a) {
            size_t sz = AAsset_getLength(a);
            std::vector<uint8_t> buf(sz);
            AAsset_read(a, buf.data(), sz);
            AAsset_close(a);
            font_loaded_ = overlay_font_.loadFromMemory(buf.data(), sz);
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
        recorder_ = new rec::Recorder(state_->activity->assetManager,
                                      state_->activity->vm,
                                      state_->activity->clazz);
    }

    std::string out_path;
    if (state_->activity->externalDataPath) {
        out_path = std::string(state_->activity->externalDataPath) + "/test.mkv";
    } else {
        out_path = "/storage/emulated/0/Android/data/io.nava.camera/files/test.mkv";
    }

    recorder_->set_capture_orientation(orientation_.degrees());
    if (recorder_->start_preview(renderer_)) {
        recording_started_ = true;
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

int32_t App::handle_input(android_app* app, AInputEvent* event) {
    if (AInputEvent_getType(event) == AINPUT_EVENT_TYPE_MOTION) {
        auto* self = reinterpret_cast<App*>(app->userData);
        if (self->ui_ && self->recorder_) {
            // Lock the UI while the previous clip is finalizing — no new recording
            // and no mode switch until the file is written.
            if (self->recorder_->state() == rec::State::FINALIZING) return 1;
            int32_t action = AMotionEvent_getAction(event) & AMOTION_EVENT_ACTION_MASK;
            if (action == AMOTION_EVENT_ACTION_DOWN) {
                float x = AMotionEvent_getX(event, 0);
                float y = AMotionEvent_getY(event, 0);
                ui::UI::Action act = self->ui_->on_touch(x, y);
                // The RAW-pipeline A/B toggles (DN/DM/TD/CD) were removed from the
                // UI once the best config was locked in as the default (HQ demosaic
                // + chroma denoise on). The implementations live on in the pipeline
                // (Recorder::set_denoise/set_demosaic_hq/set_temporal/set_chroma)
                // for later refinement; they're just no longer driven from the UI.
                if (act == ui::UI::Action::SHUTTER) {
                    auto now = std::chrono::system_clock::now();
                    std::time_t now_c = std::chrono::system_clock::to_time_t(now);
                    std::stringstream ss;
                    ss << std::put_time(std::localtime(&now_c), "%Y%m%d_%H%M%S");
                    std::string ts = ss.str();

                    if (self->ui_->is_video_mode()) {
                        if (self->recorder_->state() == rec::State::SAVING) {
                            self->recorder_->stop_saving();
                        } else {
                            std::string dir = archive::get_documents_path(app, "camera_without_blood/video");
                            if (dir.empty()) {
                                LOGE("Failed to resolve Documents path");
                                return 1;
                            }
                            std::string out_path = dir + "/VID_" + ts + ".mkv";
                            self->recorder_->start_saving(out_path);
                        }
                    } else {
                        std::string dir = archive::get_documents_path(app, "camera_without_blood/photo");
                        if (!dir.empty()) {
                            std::string out_path = dir + "/IMG_" + ts + ".dng";
                            self->recorder_->take_photo(out_path);
                        }
                    }
                }
            }
        }
        return 1;
    }
    return 0;
}

void App::handle_cmd(android_app* app, int32_t cmd) {
    auto* self = reinterpret_cast<App*>(app->userData);
    switch (cmd) {
        case APP_CMD_INIT_WINDOW:
            if (!app->window) break;
            // Hide the status bar.
            enable_immersive(app);
            // Always bring up Vulkan/UI so something is on screen immediately.
            self->permissions_granted_ = has_permissions(app);
            if (!self->renderer_) {
                self->init_vulkan();
            } else {
                self->maybe_start_recording();
            }
            // Ask for any missing permissions exactly once.
            if (!self->permissions_granted_ && !self->permissions_requested_) {
                request_permissions(app);
                self->permissions_requested_ = true;
            }
            break;
        case APP_CMD_GAINED_FOCUS:
            self->focused_ = true;
            // Re-apply status bar hide when focus returns.
            enable_immersive(app);
            // The user may have just answered the permission dialog: re-check
            // and start the recorder if it is now allowed.
            if (!self->permissions_granted_) {
                self->permissions_granted_ = has_permissions(app);
            }
            if (self->permissions_granted_ && !self->renderer_ && app->window) {
                self->init_vulkan();
            }
            // Returning to the foreground: repaint the UI so the controls are
            // visible/fresh (covers the case where the surface persisted and the
            // swapchain wasn't recreated by init_vulkan).
            self->ui_repaint_frames_ = 4;
            self->maybe_start_recording();
            break;
        case APP_CMD_LOST_FOCUS:
            self->focused_ = false;
            break;
        case APP_CMD_RESUME:
            // App is in the foreground: (re)acquire the camera + orientation sensor.
            self->resumed_ = true;
            self->orientation_.enable();
            self->maybe_start_recording();
            break;
        case APP_CMD_PAUSE:
            // App is leaving the foreground. If we are recording a video, keep the
            // camera running in the background. Otherwise, release it so Android
            // doesn't force-revoke it and break our session.
            self->resumed_ = false;
            self->orientation_.disable();
            if (self->recorder_ && self->recorder_->state() != rec::State::SAVING && 
                self->recorder_->state() != rec::State::FINALIZING) {
                self->stop_recording();
            }
            break;
        case APP_CMD_TERM_WINDOW:
            // Surface is gone: tear down ONLY the renderer. The camera keeps
            // running and is re-attached when the surface (and renderer) return.
            self->destroy_vulkan();
            break;
        case APP_CMD_DESTROY:
            // Real app shutdown: now it is safe to stop the camera.
            self->destroy_recorder();
            break;
        default: break;
    }
}
