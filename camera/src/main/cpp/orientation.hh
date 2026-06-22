#pragma once
#include <android/sensor.h>
#include <android/looper.h>

// ---------------------------------------------------------------------------
// Physical device orientation from the accelerometer (NDK ASensorManager, no
// JNI). The window is locked to portrait so it never rotates; this reports how
// the phone is actually being held so the UI overlay can rotate to match and
// captures can be tagged. Snapped to 0/90/180/270 with hysteresis (ported from
// OpenCamera's roundOrientation) to avoid jitter near the diagonals.
//
//   0   = portrait, upright
//   90  = rotated so the device's right edge is down (one landscape)
//   180 = upside down
//   270 = the other landscape
// ---------------------------------------------------------------------------
class Orientation {
public:
    static constexpr int LOOPER_ID = 3;  // distinct from glue's MAIN(1)/INPUT(2)

    // Create the sensor event queue on the current (glue) thread's looper.
    void start();
    void stop();

    // Battery: only listen while the app is in the foreground.
    void enable();
    void disable();

    // Drain pending sensor events and update the snapped orientation. Call when
    // ALooper_pollOnce returns LOOPER_ID.
    void handleEvents();

    int degrees() const { return orientation_; }

private:
    ASensorManager*    manager_ = nullptr;
    const ASensor*     accel_   = nullptr;
    ASensorEventQueue* queue_   = nullptr;

    int   orientation_ = 0;     // snapped 0/90/180/270
    float rawAngle_    = 0.0f;  // last continuous angle (deg, [0,360))
};
