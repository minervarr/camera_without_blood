#pragma once

#include <android_native_app_glue.h>
#include <memory>
#include <vector>

#include "font.hh"
#include "orientation.hh"

class Renderer;
class Canvas;

namespace ui  { class UI; }
namespace rec { class Recorder; }

class App {
public:
    explicit App(android_app* state);
    ~App();

    void run();

private:
    void init_vulkan();
    void destroy_vulkan();           // tears down renderer/canvas/ui ONLY (surface-tied)
    void destroy_recorder();         // stops + frees the recorder (app shutdown only)
    void draw_frame();
    void maybe_start_recording();    // opens camera when foreground + permitted
    void stop_recording();           // releases camera when backgrounded

    static void handle_cmd(android_app* app, int32_t cmd);
    static int32_t handle_input(android_app* app, AInputEvent* event);

    android_app* state_;

    Renderer*      renderer_  = nullptr;
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

    Orientation orientation_;
};
