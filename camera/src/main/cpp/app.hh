#pragma once

#include "app_view.hh"
#include "host.hh"
#include "android_platform.hh"

#include <memory>
#include <string>
#include <vector>

#include "font.hh"
#include "orientation.hh"

class Renderer;
class Canvas;

namespace ui  { class UI; }
namespace rec { class Recorder; }

class App : public AppView {
public:
    explicit App(std::unique_ptr<Host> host);
    ~App();

    void run();

    // ── AppView callbacks ──────────────────────────────────────────────────
    void onHostResized() override;
    void shutdown() override;
    void onSurfaceLost() override;
    bool onSurfaceRecreated() override;
    void onAppBackgrounded() override;
    void onAppForegrounded() override;
    void onLButtonDown(int x, int y) override;
    void onNavBack() override;
    void onTimer(int timerId) override;

private:
    enum Timer { TIMER_FINALIZE = 1 };

    void init_vulkan();
    void destroy_vulkan();           // tears down renderer/canvas/ui ONLY (surface-tied)
    void destroy_recorder();         // stops + frees the recorder (app shutdown only)
    void draw_frame();
    void maybe_start_recording();    // opens camera when foreground + permitted
    void stop_recording();           // releases camera when backgrounded
    void maybe_finish_session();     // back pressed: finalize, then return files + finish
    const std::string& output_base();// dir for captures (host's, else Documents)

    std::unique_ptr<Host> host_;

    Renderer*      renderer_  = nullptr;
    AndroidFrameWaker frame_waker_{ALooper_forThread()};
    Font           overlay_font_;
    bool           font_loaded_ = false;
    std::vector<float> canvas_data_;
    Canvas*        canvas_    = nullptr;
    ui::UI*        ui_        = nullptr;
    rec::Recorder* recorder_ = nullptr;
    bool permissions_granted_ = false;
    bool permissions_requested_ = false;
    bool recording_started_ = false;
    bool focused_ = false;
    bool resumed_ = false;
    // Force a few UI redraws after the surface returns (even mid-recording, when we
    // otherwise don't draw), to fill the freshly-recreated blank swapchain — else
    // returning from the background while recording shows a black screen with no
    // Stop button.
    int  ui_repaint_frames_ = 0;
    // Persisted photo/video toggle, so it survives UI rebuilds on surface recreate.
    bool ui_video_mode_ = false;
    // RAW photo output mode (0=RAW_BRACKET, 1=RAW_MERGED, 2=HDR_IMAGE) + whether the
    // in-scene selector is shown. Seeded from the host CameraActivity; persisted
    // across UI rebuilds; cycled by the in-scene chip.
    int  ui_photo_mode_ = 0;
    bool ui_photo_mode_ui_enabled_ = true;

    // Embedded-scene exit: set when the user presses Back. The session then waits
    // out any in-flight finalize, returns the captured files to the host
    // (CameraActivity.finishWithResults), and finishes — once.
    bool exit_requested_ = false;
    bool results_sent_   = false;
    // Capture output directory, resolved once from the host CameraActivity
    // (getExternalFilesDir-based, scoped-storage-safe); falls back to Documents
    // for the standalone demo.
    std::string out_base_;

    vce::platform::Orientation orientation_;
};
