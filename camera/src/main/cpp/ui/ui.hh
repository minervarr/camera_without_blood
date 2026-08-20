#pragma once

#include "canvas.hh"
#include "../recorder/recorder.hh"

namespace ui {

class UI {
public:
    explicit UI(Canvas& canvas);
    ~UI() = default;

    // Called once per frame. Draws the interface and handles recording state.
    void update(rec::Recorder& recorder);
    enum class Action { NONE, TOGGLE_MODE, SHUTTER, CYCLE_PHOTO_MODE };
    Action on_touch(float x, float y);
    bool is_video_mode() const { return mode_ == Mode::VIDEO; }
    // Restore the photo/video selection after the UI is rebuilt (surface recreate),
    // so returning from the background mid-recording keeps the Stop button.
    void set_video_mode(bool v) { mode_ = v ? Mode::VIDEO : Mode::PHOTO; }

    // RAW photo output mode chip: 0=RAW_BRACKET, 1=RAW_MERGED, 2=HDR_IMAGE. Only
    // shown (and tappable) in PHOTO mode when the host enabled it.
    void set_photo_mode_index(int m) { photo_mode_ = ((m % 3) + 3) % 3; }
    void set_photo_mode_ui_enabled(bool e) { photo_mode_ui_enabled_ = e; }

private:
    // Shared control-bar geometry: both modes place the shutter and the
    // mode toggle identically, so the placement lives in one place.
    struct Layout {
        float w, h;          // canvas size
        float y_offset;      // letterbox band above the 1080x1920 design frame
        float top_y;         // baseline for the top status line
        float btn_size;      // shutter diameter
        float bottom_y;      // control-bar centre line
        float toggle_w, toggle_h;
    };
    Layout compute_layout() const;

    void draw_photo_mode();
    void draw_video_mode(bool is_recording, int64_t duration_ms,
                         bool finalizing, int finalize_pct);

    Canvas& canvas_;
    
    enum class Mode { PHOTO, VIDEO };
    Mode mode_ = Mode::PHOTO;
    // The RAW-pipeline denoise/demosaic A/B toggles were retired from the UI; the
    // chosen defaults now live in the pipeline (see RawVideoPipeline's atomics).

    struct Rect { float x, y, w, h; };
    Rect shutter_btn_ = {0, 0, 0, 0};
    Rect toggle_btn_ = {0, 0, 0, 0};

    // Photo output-mode selector chip (RAW / RAW¹ / HDR).
    int  photo_mode_ = 0;                 // 0=RAW_BRACKET 1=RAW_MERGED 2=HDR_IMAGE
    bool photo_mode_ui_enabled_ = true;
    Rect photo_mode_btn_ = {0, 0, 0, 0};
};

} // namespace ui
