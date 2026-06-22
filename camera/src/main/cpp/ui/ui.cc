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

    return ui::UI::Action::NONE;
}

void UI::update(rec::Recorder& recorder, int device_orientation) {
    if (mode_ == Mode::PHOTO) {
        draw_photo_mode(device_orientation);
    } else {
        rec::State st  = recorder.state();
        bool recording = (st == rec::State::SAVING);
        bool finalizing = (st == rec::State::FINALIZING);
        draw_video_mode(device_orientation, recording, recorder.duration_ms(),
                        finalizing, recorder.finalize_percent());
    }
}

void UI::draw_photo_mode(int device_orientation) {
    float w = canvas_.w();
    float h = canvas_.h();
    float raw_w = 1080.0f;
    float raw_h = 1920.0f;
    float scale = std::min(w / raw_w, h / raw_h);
    float draw_h = raw_h * scale;
    float y_offset = (h - draw_h) / 2.0f;
    
    // Top bar: empty in photo mode, as user requested "not that debug label PORTRAIT"
    // float top_y = (y_offset > 0.0f) ? (y_offset * 0.5f) : (h * 0.04f);
    
    float btn_size = std::min(w, h) * 0.15f;

    // Bottom bar: Controls
    float bottom_y = h - y_offset * 0.5f;
    
    // Clamp to avoid navigation bar overlap and ensure it fits safely on screen
    float safe_margin = btn_size * 0.8f + (h * 0.06f);
    if (bottom_y > h - safe_margin) {
        bottom_y = h - safe_margin;
    }
    
    // Shutter button (white circle for photo)
    shutter_btn_ = {w * 0.5f - btn_size * 0.5f, bottom_y - btn_size * 0.5f, btn_size, btn_size};
    canvas_.button(shutter_btn_.x, shutter_btn_.y, shutter_btn_.w, shutter_btn_.h, "", col::text, col::bg, btn_size * 0.5f);
    
    // Toggle button
    float toggle_w = std::min(w, h) * 0.2f;
    float toggle_h = std::min(w, h) * 0.08f;
    toggle_btn_ = {w * 0.8f - toggle_w * 0.5f, bottom_y - toggle_h * 0.5f, toggle_w, toggle_h};
    canvas_.button(toggle_btn_.x, toggle_btn_.y, toggle_btn_.w, toggle_btn_.h, "VIDEO", col::panel, col::text, toggle_h * 0.2f);

    // Photo output-mode chip (mirrors the toggle on the left). Only when the host
    // left the selector enabled. RAW3 = 3 sources + merged DNG; RAW1 = merged DNG
    // only; HDR = developed Radiance .hdr only.
    if (photo_mode_ui_enabled_) {
        static const char* kLabels[3] = {"RAW3", "RAW1", "HDR"};
        float chip_w = toggle_w, chip_h = toggle_h;
        photo_mode_btn_ = {w * 0.2f - chip_w * 0.5f, bottom_y - chip_h * 0.5f, chip_w, chip_h};
        canvas_.button(photo_mode_btn_.x, photo_mode_btn_.y, photo_mode_btn_.w, photo_mode_btn_.h,
                       kLabels[photo_mode_ % 3], col::panel, col::text, chip_h * 0.2f);
    } else {
        photo_mode_btn_ = {0, 0, 0, 0};
    }
}

void UI::draw_video_mode(int device_orientation, bool is_recording, int64_t duration_ms,
                         bool finalizing, int finalize_pct) {
    float w = canvas_.w();
    float h = canvas_.h();
    float raw_w = 1080.0f;
    float raw_h = 1920.0f;
    float scale = std::min(w / raw_w, h / raw_h);
    float draw_h = raw_h * scale;
    float y_offset = (h - draw_h) / 2.0f;

    // Top bar: Timer, or the offline finalize progress.
    float top_y = (y_offset > 0.0f) ? (y_offset * 0.5f) : (h * 0.04f);
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

    float btn_size = std::min(w, h) * 0.15f;

    // Bottom bar: Controls
    float bottom_y = h - y_offset * 0.5f;
    
    // Clamp to avoid navigation bar overlap and ensure it fits safely on screen
    float safe_margin = btn_size * 0.8f + (h * 0.06f);
    if (bottom_y > h - safe_margin) {
        bottom_y = h - safe_margin;
    }
    
    // Shutter button (red circle/square for video; dimmed + inert while finalizing).
    shutter_btn_ = {w * 0.5f - btn_size * 0.5f, bottom_y - btn_size * 0.5f, btn_size, btn_size};
    float radius = is_recording ? btn_size * 0.1f : btn_size * 0.5f;
    canvas_.button(shutter_btn_.x, shutter_btn_.y, shutter_btn_.w, shutter_btn_.h, "",
                   finalizing ? col::panel : col::red, col::text, radius);
    
    // Toggle button
    float toggle_w = std::min(w, h) * 0.2f;
    float toggle_h = std::min(w, h) * 0.08f;
    toggle_btn_ = {w * 0.8f - toggle_w * 0.5f, bottom_y - toggle_h * 0.5f, toggle_w, toggle_h};
    canvas_.button(toggle_btn_.x, toggle_btn_.y, toggle_btn_.w, toggle_btn_.h, "PHOTO", col::panel, col::text, toggle_h * 0.2f);

    // Audio mode button


    // The DN/DM/TD/CD A/B toggles were retired here once the best RAW-pipeline
    // config was made the default (HQ demosaic + chroma denoise on); recording UI
    // is now just the shutter + the PHOTO/VIDEO switch.
}

} // namespace ui
