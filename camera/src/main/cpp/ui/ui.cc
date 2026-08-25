#include "ui.hh"
#include <algorithm>
#include <chrono>

namespace ui {

UI::UI(Canvas& canvas) : canvas_(canvas) {}

ui::UI::Action UI::on_touch(float x, float y) {
    if (x >= toggle_btn_.x && x <= toggle_btn_.x + toggle_btn_.w &&
        y >= toggle_btn_.y && y <= toggle_btn_.y + toggle_btn_.h) {
        mode_ = (mode_ == Mode::PHOTO) ? Mode::VIDEO : Mode::PHOTO;
        return ui::UI::Action::TOGGLE_MODE;
    }

    if (x >= shutter_btn_.x && x <= shutter_btn_.x + shutter_btn_.w &&
        y >= shutter_btn_.y && y <= shutter_btn_.y + shutter_btn_.h) {
        return ui::UI::Action::SHUTTER;
    }

    if (photo_mode_ui_enabled_ && mode_ == Mode::PHOTO &&
        x >= photo_mode_btn_.x && x <= photo_mode_btn_.x + photo_mode_btn_.w &&
        y >= photo_mode_btn_.y && y <= photo_mode_btn_.y + photo_mode_btn_.h) {
        return ui::UI::Action::CYCLE_PHOTO_MODE;
    }

    if (nr_btn_.w > 0.0f && mode_ == Mode::PHOTO &&
        x >= nr_btn_.x && x <= nr_btn_.x + nr_btn_.w &&
        y >= nr_btn_.y && y <= nr_btn_.y + nr_btn_.h) {
        return ui::UI::Action::TOGGLE_STILL_NR;
    }

    // The w > 0 tests matter: a hidden control keeps a {0,0,0,0} rect, which
    // would otherwise swallow a tap at the very top-left corner.
    if (focus_btn_.w > 0.0f &&
        x >= focus_btn_.x && x <= focus_btn_.x + focus_btn_.w &&
        y >= focus_btn_.y && y <= focus_btn_.y + focus_btn_.h) {
        return ui::UI::Action::TOGGLE_FOCUS_MODE;
    }

    if (peak_btn_.w > 0.0f &&
        x >= peak_btn_.x && x <= peak_btn_.x + peak_btn_.w &&
        y >= peak_btn_.y && y <= peak_btn_.y + peak_btn_.h) {
        return ui::UI::Action::CYCLE_PEAKING;
    }

    if (loupe_btn_.w > 0.0f && !loupe_locked_ &&
        x >= loupe_btn_.x && x <= loupe_btn_.x + loupe_btn_.w &&
        y >= loupe_btn_.y && y <= loupe_btn_.y + loupe_btn_.h) {
        return ui::UI::Action::CYCLE_LOUPE;
    }

    return ui::UI::Action::NONE;
}

// Focus chip, loupe chip, and — while manual focus is armed — a vertical scale
// on the right edge reading the lens position. Both modes get all three: a
// clip is exactly where autofocus hunting hurts most.
void UI::draw_focus_controls(const Layout& l) {
    const float chip_w = l.toggle_w, chip_h = l.toggle_h;
    const float row_y  = l.top_y - chip_h * 0.5f;

    if (focus_available_) {
        focus_btn_ = {l.w * 0.2f - chip_w * 0.5f, row_y, chip_w, chip_h};
        canvas_.button(focus_btn_.x, focus_btn_.y, focus_btn_.w, focus_btn_.h,
                       focus_manual_ ? "MF" : "AF",
                       focus_manual_ ? col::red : col::panel, col::text, chip_h * 0.2f);
    } else {
        focus_btn_ = {0, 0, 0, 0};
    }

    // Peaking sits under the focus chip: the two are used together, and unlike
    // the loupe it is always offered — it costs two texture taps and needs no
    // capability from the camera.
    peak_btn_ = {l.w * 0.2f - chip_w * 0.5f, row_y + chip_h * 1.3f, chip_w, chip_h};
    {
        const char* label = (peak_level_ == 1) ? "PEAK" : (peak_level_ == 2) ? "PEAK+" : "PEAK OFF";
        canvas_.button(peak_btn_.x, peak_btn_.y, peak_btn_.w, peak_btn_.h, label,
                       peak_level_ ? col::green : col::panel, col::text, chip_h * 0.2f);
    }

    if (loupe_available_ && loupe_max_factor_ >= 2) {
        loupe_btn_ = {l.w * 0.2f - chip_w * 0.5f, row_y + chip_h * 2.6f, chip_w, chip_h};
        const char* label = (loupe_factor_ == 2) ? "LOUPE x2"
                          : (loupe_factor_ == 4) ? "LOUPE x4" : "LOUPE";
        const Color bg = loupe_locked_ ? col::panel
                       : (loupe_factor_ ? col::red : col::panel);
        canvas_.button(loupe_btn_.x, loupe_btn_.y, loupe_btn_.w, loupe_btn_.h, label,
                       bg, loupe_locked_ ? col::dim : col::text, chip_h * 0.2f);
    } else {
        loupe_btn_ = {0, 0, 0, 0};
    }

    // Outline the magnified inset. Four thin bars rather than a stroked rect:
    // the canvas draws filled shapes, and a filled rect would cover the very
    // pixels the loupe exists to show.
    if (loupe_rect_.w > 0.0f && loupe_factor_ > 0) {
        const float t = std::max(2.0f, l.w * 0.004f);
        const Rect& r = loupe_rect_;
        canvas_.button(r.x - t,       r.y - t,       r.w + 2 * t, t, "", col::text, col::text, 0.0f);
        canvas_.button(r.x - t,       r.y + r.h,     r.w + 2 * t, t, "", col::text, col::text, 0.0f);
        canvas_.button(r.x - t,       r.y,           t,           r.h, "", col::text, col::text, 0.0f);
        canvas_.button(r.x + r.w,     r.y,           t,           r.h, "", col::text, col::text, 0.0f);
    }

    if (!focus_available_ || !focus_manual_) return;

    // The scale. Diopters run 0 (infinity) at the top to max (closest) at the
    // bottom, which is the direction the finger drags: pull down to pull focus
    // in. It is a readout, not a hit target — dragging anywhere scrubs it.
    const float track_x = l.w * 0.92f;
    const float track_w = std::max(4.0f, l.w * 0.012f);
    const float track_y = l.h * 0.28f;
    const float track_h = l.h * 0.36f;
    canvas_.button(track_x - track_w * 0.5f, track_y, track_w, track_h, "",
                   col::panel, col::text, track_w * 0.5f);

    const float frac  = (focus_max_d_ > 0.0f)
                      ? std::min(1.0f, std::max(0.0f, focus_d_ / focus_max_d_)) : 0.0f;
    const float thumb = track_y + frac * track_h;
    const float th_w  = l.w * 0.05f, th_h = std::max(6.0f, l.h * 0.008f);
    canvas_.button(track_x - th_w * 0.5f, thumb - th_h * 0.5f, th_w, th_h, "",
                   col::red, col::text, th_h * 0.5f);

    // Distance readout. Diopters are 1/m, so infinity is the 0 end; anything
    // beyond 10 m is infinity for focusing purposes and is labelled as such.
    char buf[16];
    if (focus_d_ <= 0.1f) snprintf(buf, sizeof(buf), "inf");
    else                  snprintf(buf, sizeof(buf), "%.2fm", 1.0f / focus_d_);
    canvas_.textCentered(buf, track_x - l.w * 0.06f, track_y + track_h + l.h * 0.03f,
                         l.w * 0.035f, col::text);
}

void UI::update(rec::Recorder& recorder) {
    if (mode_ == Mode::PHOTO) {
        draw_photo_mode();
    } else {
        rec::State st  = recorder.state();
        bool recording = (st == rec::State::SAVING);
        bool finalizing = (st == rec::State::FINALIZING);
        draw_video_mode(recording, recorder.duration_ms(),
                        finalizing, recorder.finalize_percent());
    }
}

// The overlay is laid out against a 1080x1920 design frame scaled to fit, with
// the control bar riding the bottom letterbox band — clamped so it never lands
// under the navigation bar.
UI::Layout UI::compute_layout() const {
    Layout l{};
    l.w = canvas_.w();
    l.h = canvas_.h();
    constexpr float kDesignW = 1080.0f, kDesignH = 1920.0f;
    const float scale  = std::min(l.w / kDesignW, l.h / kDesignH);
    const float draw_h = kDesignH * scale;
    l.y_offset = (l.h - draw_h) / 2.0f;
    l.top_y    = (l.y_offset > 0.0f) ? (l.y_offset * 0.5f) : (l.h * 0.04f);
    l.btn_size = std::min(l.w, l.h) * 0.15f;

    l.bottom_y = l.h - l.y_offset * 0.5f;
    const float safe_margin = l.btn_size * 0.8f + (l.h * 0.06f);
    if (l.bottom_y > l.h - safe_margin) l.bottom_y = l.h - safe_margin;

    l.toggle_w = std::min(l.w, l.h) * 0.2f;
    l.toggle_h = std::min(l.w, l.h) * 0.08f;
    return l;
}

void UI::draw_photo_mode() {
    const Layout l = compute_layout();
    // Top bar: the still-NR chip on the right (the rest stays empty).

    // Shutter button (white circle for photo)
    shutter_btn_ = {l.w * 0.5f - l.btn_size * 0.5f, l.bottom_y - l.btn_size * 0.5f,
                    l.btn_size, l.btn_size};
    canvas_.button(shutter_btn_.x, shutter_btn_.y, shutter_btn_.w, shutter_btn_.h, "",
                   col::text, col::bg, l.btn_size * 0.5f);

    // Toggle button
    toggle_btn_ = {l.w * 0.8f - l.toggle_w * 0.5f, l.bottom_y - l.toggle_h * 0.5f,
                   l.toggle_w, l.toggle_h};
    canvas_.button(toggle_btn_.x, toggle_btn_.y, toggle_btn_.w, toggle_btn_.h, "VIDEO",
                   col::panel, col::text, l.toggle_h * 0.2f);

    // Still NR toggle — lit red while the HAL's HIGH_QUALITY denoise is armed.
    // Drawn only where it can actually do something: the HAL's NR sits downstream
    // of the RAW16 tap, so on a RAW device this control is physically incapable of
    // changing the DNG. See set_still_nr_ui_enabled.
    if (still_nr_ui_enabled_) {
        nr_btn_ = {l.w * 0.8f - l.toggle_w * 0.5f, l.top_y - l.toggle_h * 0.5f,
                   l.toggle_w, l.toggle_h};
        canvas_.button(nr_btn_.x, nr_btn_.y, nr_btn_.w, nr_btn_.h,
                       still_nr_on_ ? "NR ON" : "NR OFF",
                       still_nr_on_ ? col::red : col::panel, col::text, l.toggle_h * 0.2f);
    } else {
        nr_btn_ = {0, 0, 0, 0};
    }

    draw_focus_controls(l);

    // Photo output-mode chip (mirrors the toggle on the left). Only when the host
    // left the selector enabled. FAST = 1 raw shot (instant, motion-proof);
    // STATIC = 3-shot merged DNG; STATIC+ = merged DNG + the 3 source DNGs.
    if (photo_mode_ui_enabled_) {
        static const char* kLabels[3] = {"FAST", "STATIC", "STATIC+"};
        const float chip_w = l.toggle_w, chip_h = l.toggle_h;
        photo_mode_btn_ = {l.w * 0.2f - chip_w * 0.5f, l.bottom_y - chip_h * 0.5f, chip_w, chip_h};
        canvas_.button(photo_mode_btn_.x, photo_mode_btn_.y, photo_mode_btn_.w, photo_mode_btn_.h,
                       kLabels[photo_mode_ % 3], col::panel, col::text, chip_h * 0.2f);
    } else {
        photo_mode_btn_ = {0, 0, 0, 0};
    }
}

void UI::draw_video_mode(bool is_recording, int64_t duration_ms,
                         bool finalizing, int finalize_pct) {
    const Layout l = compute_layout();
    const float w = l.w, h = l.h, top_y = l.top_y;

    // Top bar: Timer, or the offline finalize progress.
    if (finalizing) {
        char buf[24];
        snprintf(buf, sizeof(buf), "Processing %d%%", finalize_pct);
        canvas_.textCentered(buf, w * 0.5f, top_y, w * 0.045f, col::text);
        // A second line near the centre so it's unmistakable the file isn't ready yet.
        canvas_.textCentered("finalizing video…", w * 0.5f, h * 0.5f, w * 0.04f, col::text);
    } else if (is_recording) {
        int sec = (duration_ms / 1000) % 60;
        int min = (duration_ms / 60000);
        char buf[16];
        snprintf(buf, sizeof(buf), "%02d:%02d", min, sec);
        canvas_.textCentered(buf, w * 0.5f, top_y, w * 0.05f, col::red);
    } else {
        // Empty when not recording, user requested "not that debug label PORTRAIT"
    }

    // Shutter button (red circle/square for video; dimmed + inert while finalizing).
    shutter_btn_ = {w * 0.5f - l.btn_size * 0.5f, l.bottom_y - l.btn_size * 0.5f,
                    l.btn_size, l.btn_size};
    const float radius = is_recording ? l.btn_size * 0.1f : l.btn_size * 0.5f;
    canvas_.button(shutter_btn_.x, shutter_btn_.y, shutter_btn_.w, shutter_btn_.h, "",
                   finalizing ? col::panel : col::red, col::text, radius);

    // Toggle button
    toggle_btn_ = {w * 0.8f - l.toggle_w * 0.5f, l.bottom_y - l.toggle_h * 0.5f,
                   l.toggle_w, l.toggle_h};
    canvas_.button(toggle_btn_.x, toggle_btn_.y, toggle_btn_.w, toggle_btn_.h, "PHOTO",
                   col::panel, col::text, l.toggle_h * 0.2f);

    draw_focus_controls(l);

    // The DN/DM/TD/CD A/B toggles were retired here once the RAW-pipeline config
    // was locked into the pipeline's own defaults; the recording UI is now just
    // the shutter + the PHOTO/VIDEO switch.
}

} // namespace ui
