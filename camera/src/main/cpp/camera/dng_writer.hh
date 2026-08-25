#pragma once

#include <cstdint>
#include <string>

namespace dng {

// All DNG-relevant tags gathered from the camera. Static fields come from the
// camera characteristics (resolved once at open()); the dynamic fields
// (as-shot neutral, black/white level, iso/exposure) come from the per-shot
// capture result. `has_*` flags guard optional tags so a device missing one
// never aborts the save.
struct DngMeta {
    // Buffer geometry of the RAW16 Bayer plane.
    int32_t  width        = 0;
    int32_t  height       = 0;
    int32_t  stride_bytes = 0;   // row stride in bytes (may exceed width*2)

    // ACAMERA_SENSOR_INFO_COLOR_FILTER_ARRANGEMENT: 0=RGGB 1=GRBG 2=GBRG 3=BGGR.
    int32_t  cfa = 0;

    // Active array within the full pixel array: [x, y, width, height].
    int32_t  active_xywh[4] = {0, 0, 0, 0};
    bool     has_active = false;

    int32_t  white_level    = 65535;
    float    black_level[4] = {0, 0, 0, 0};  // per 2x2 CFA element

    // Color calibration matrices (row-major 3x3), illuminants are EXIF LightSource
    // codes (Camera2 reference illuminant enums map 1:1 to these).
    bool     has_cm1 = false, has_cm2 = false;
    bool     has_fm1 = false, has_fm2 = false;
    bool     has_cc1 = false, has_cc2 = false;
    double   color_matrix1[9]{}, color_matrix2[9]{};
    double   forward_matrix1[9]{}, forward_matrix2[9]{};
    double   calibration1[9]{}, calibration2[9]{};
    int32_t  illuminant1 = 0, illuminant2 = 0;

    bool     has_neutral = false;
    double   as_shot_neutral[3] = {1.0, 1.0, 1.0};

    // DNG BaselineExposure (50730), in stops. Non-zero when the stored data is
    // deliberately scaled so WhiteLevel is NOT diffuse white: a merged bracket
    // reserves the top of its range for highlights recovered above the
    // reference exposure, which puts everything else that many stops down. The
    // tag is how a developer knows to open back up; without it the merged file
    // renders exactly `max_boost` stops darker than the single shot of the same
    // scene, which is what made STATIC look "less light" next to FAST.
    double   baseline_exposure = 0.0;

    // DNG NoiseProfile (51041): one (S, O) pair per CFA channel, in CFAPattern
    // order. Variance at normalised signal x is S*x + O — shot noise scales with
    // the signal, read noise does not. Straight from ACAMERA_SENSOR_NOISE_PROFILE,
    // per shot, so it tracks the ISO actually used.
    //
    // This is what lets a denoiser downstream know how noisy the frame really is
    // instead of estimating it from the pixels. It matters most for a learned
    // model: told the wrong noise level, it either smears real detail away or
    // synthesises texture the scene never had.
    bool     has_noise_profile = false;
    int32_t  noise_profile_count = 0;          // 2 * CFA channels (8 for Bayer)
    double   noise_profile[8]{};

    int32_t  orientation_deg = 0;  // sensor mount orientation (0/90/180/270)

    std::string camera_model = "camera_without_blood";
};

// Writes `data` (16-bit Bayer, host little-endian) to a DNG file at `path`.
// Returns true on success; on failure sets nothing and the caller logs.
bool write_dng(const std::string& path, const uint16_t* data, const DngMeta& m);

} // namespace dng
