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
    enum class Action { NONE, TOGGLE_MODE, SHUTTER, CYCLE_PHOTO_MODE, TOGGLE_STILL_NR,
                        TOGGLE_FOCUS_MODE, CYCLE_LOUPE, CYCLE_PEAKING };
    Action on_touch(float x, float y);
    bool is_video_mode() const { return mode_ == Mode::VIDEO; }
    // Restore the photo/video selection after the UI is rebuilt (surface recreate),
    // so returning from the background mid-recording keeps the Stop button.
    void set_video_mode(bool v) { mode_ = v ? Mode::VIDEO : Mode::PHOTO; }

    // RAW photo output mode chip: 0=RAW_BRACKET, 1=RAW_MERGED, 2=HDR_IMAGE. Only
    // shown (and tappable) in PHOTO mode when the host enabled it.
    void set_photo_mode_index(int m) { photo_mode_ = ((m % 3) + 3) % 3; }
    void set_photo_mode_ui_enabled(bool e) { photo_mode_ui_enabled_ = e; }

    // Still-capture noise-reduction chip (photo mode's top-right corner): the
    // HAL's HIGH_QUALITY NR for the full-res YUV still. Default OFF — raw
    // sensor pixels; tapping it lights the chip and cleans up dim shots.
    //
    // It is HIDDEN on the RAW path, and that is not cosmetic. NOISE_REDUCTION_MODE
    // is a control over the HAL's ISP stage, and RAW16 is by Camera2's definition
    // the sensor data from BEFORE that stage — so on a RAW device the chip cannot
    // affect the DNG no matter what it is set to. It used to be drawn anyway (the
    // old comment here said "inert ... drawn regardless"), which put a dead
    // control on screen on exactly the devices this app is built for. A chip that
    // does nothing is worse than no chip: it invites the user to blame it for
    // noise it was never able to touch.
    bool still_nr_enabled() const { return still_nr_on_; }
    void set_still_nr_enabled(bool e) { still_nr_on_ = e; }
    void set_still_nr_ui_enabled(bool e) { still_nr_ui_enabled_ = e; }

    // ── Manual focus ────────────────────────────────────────────────────────
    // Drawn in both modes. The chip is hidden entirely when the device has no
    // movable lens (a fixed-focus module) or the session can't honour it, so
    // there is never a dead control on screen. While MF is armed a vertical
    // scale on the right edge shows where the lens is; the value itself is
    // scrubbed by dragging, not by touching the scale (see App::onMouseWheel).
    void set_focus_available(bool a) { focus_available_ = a; }
    void set_focus_manual(bool m)    { focus_manual_ = m; }
    // `d` is diopters (1/m) and `max_d` the lens's closest focus, so the scale
    // can place the thumb and print a distance.
    void set_focus_value(float d, float max_d) { focus_d_ = d; focus_max_d_ = max_d; }

    // ── Focus-check loupe ───────────────────────────────────────────────────
    // 0 = off, otherwise the magnification (2 or 4). The chip cycles
    // off -> x2 -> x4 -> off.
    void set_loupe_available(bool a) { loupe_available_ = a; }
    // The largest magnification backed by real pixels; the chip is hidden below
    // x2 because a loupe that only upscales cannot answer "is this in focus?".
    void set_loupe_max_factor(int f) { loupe_max_factor_ = f; }
    // Recording holds the preview stream fixed, so the loupe is refused then;
    // the chip is drawn dimmed rather than lying about being tappable.
    void set_loupe_locked(bool l) { loupe_locked_ = l; }
    // Where the renderer drew the magnified inset, so the overlay can frame it.
    // w == 0 means "not drawn"; the outline is then skipped.
    // Focus peaking level: 0 = off, 1 = normal, 2 = high sensitivity. Drawn in
    // both modes and always available — it needs no camera capability, only
    // pixels.
    void set_peak_level(int p) { peak_level_ = p; }

    void set_loupe_rect(float x, float y, float w, float h) {
        loupe_rect_ = {x, y, w, h};
    }
    void set_loupe_factor(int f)     { loupe_factor_ = f; }
    int  loupe_factor() const        { return loupe_factor_; }

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

    // The focus chip, loupe chip and focus scale are identical in both modes,
    // so they are drawn from one place.
    void draw_focus_controls(const Layout& l);

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

    // Still NR toggle (top-right in photo mode).
    bool still_nr_on_ = false;
    bool still_nr_ui_enabled_ = true;   // false on the RAW path — see set_still_nr_ui_enabled
    Rect nr_btn_ = {0, 0, 0, 0};

    // Manual focus + loupe (both modes).
    bool  focus_available_ = false;
    bool  focus_manual_    = false;
    float focus_d_         = 0.0f;   // diopters, 0 = infinity
    float focus_max_d_     = 0.0f;   // the lens's closest focus, in diopters
    Rect  focus_btn_       = {0, 0, 0, 0};
    bool  loupe_available_  = false;
    int   loupe_max_factor_ = 0;     // 0 = not worth offering, else 2 or 4
    bool  loupe_locked_     = false; // recording: refused, drawn dimmed
    int   loupe_factor_     = 0;     // 0 = off, else 2 or 4
    Rect  loupe_btn_       = {0, 0, 0, 0};
    Rect  loupe_rect_      = {0, 0, 0, 0};   // the inset the renderer drew
    int   peak_level_      = 0;              // 0 off, 1 normal, 2 high
    Rect  peak_btn_        = {0, 0, 0, 0};
};

} // namespace ui
