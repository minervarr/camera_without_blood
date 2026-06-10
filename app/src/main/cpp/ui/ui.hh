#pragma once

#include "canvas.hh"
#include "../recorder/recorder.hh"

namespace ui {

class UI {
public:
    explicit UI(Canvas& canvas);
    ~UI() = default;

    // Called once per frame. Draws the interface and handles recording state.
    // device_orientation is the physical orientation in degrees (0/90/180/270);
    // the whole overlay is rotated by it so labels/icons stay upright to the user
    // while the camera preview underneath stays fixed.
    void update(rec::Recorder& recorder, int device_orientation);
    enum class Action { NONE, TOGGLE_MODE, SHUTTER };
    Action on_touch(float x, float y);
    bool is_video_mode() const { return mode_ == Mode::VIDEO; }

private:
    void draw_photo_mode(int device_orientation);
    void draw_video_mode(int device_orientation, bool is_recording, int64_t duration_ms);

    Canvas& canvas_;
    
    enum class Mode { PHOTO, VIDEO };
    Mode mode_ = Mode::PHOTO;
    
    struct Rect { float x, y, w, h; };
    Rect shutter_btn_ = {0, 0, 0, 0};
    Rect toggle_btn_ = {0, 0, 0, 0};
};

} // namespace ui
