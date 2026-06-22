#include "orientation.hh"
#include <cmath>
#include <algorithm>
#include <cstdlib>

#include "logger.hh"
#undef LOG_TAG
#define LOG_TAG "Orientation"

namespace {
// OpenCamera's roundOrientation: only re-snap to the nearest 90° once the angle
// has moved past 45°+5° hysteresis from the current snapped value, so the value
// doesn't flicker when the phone is held near a diagonal.
int roundOrientation(float angleDeg, int prev) {
    int a = static_cast<int>(lroundf(angleDeg));
    a = ((a % 360) + 360) % 360;
    bool change;
    if (prev < 0) {
        change = true;
    } else {
        int dist = std::abs(a - prev);
        dist = std::min(dist, 360 - dist);
        change = dist >= 50;  // 45 + 5 hysteresis
    }
    if (change) return ((a + 45) / 90 * 90) % 360;
    return prev;
}
}  // namespace

void Orientation::start() {
    manager_ = ASensorManager_getInstance();
    if (!manager_) { LOGE("no sensor manager"); return; }
    accel_ = ASensorManager_getDefaultSensor(manager_, ASENSOR_TYPE_ACCELEROMETER);
    if (!accel_) { LOGE("no accelerometer"); return; }

    ALooper* looper = ALooper_forThread();
    queue_ = ASensorManager_createEventQueue(manager_, looper, LOOPER_ID, nullptr, nullptr);
    LOGI("orientation sensor ready");
}

void Orientation::enable() {
    if (!queue_ || !accel_) return;
    ASensorEventQueue_enableSensor(queue_, accel_);
    ASensorEventQueue_setEventRate(queue_, accel_, 100000);  // 100 ms
}

void Orientation::disable() {
    if (!queue_ || !accel_) return;
    ASensorEventQueue_disableSensor(queue_, accel_);
}

void Orientation::stop() {
    if (manager_ && queue_) ASensorManager_destroyEventQueue(manager_, queue_);
    queue_ = nullptr;
}

void Orientation::handleEvents() {
    if (!queue_) return;
    ASensorEvent e;
    bool got = false;
    // Drain; keep only the most recent sample.
    while (ASensorEventQueue_getEvents(queue_, &e, 1) > 0) {
        if (e.type == ASENSOR_TYPE_ACCELEROMETER) {
            float ax = e.acceleration.x;
            float ay = e.acceleration.y;
            // Angle of the gravity vector in the screen plane. Portrait-upright
            // (ay ≈ +g, ax ≈ 0) → 0°. Skip near-flat samples (face up/down) where
            // the in-plane direction is ill-defined.
            if (ax * ax + ay * ay > 4.0f) {  // ~2 m/s² of in-plane gravity
                rawAngle_ = std::atan2(ax, ay) * 57.29578f;  // rad→deg
                got = true;
            }
        }
    }
    if (got) orientation_ = roundOrientation(rawAngle_, orientation_);
}
