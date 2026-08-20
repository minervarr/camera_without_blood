#include "dng_writer.hh"

#include "../logger.hh"

#define TINY_DNG_WRITER_IMPLEMENTATION
#include "tiny_dng_writer.h"

#include <cstring>   // std::memcpy (was only reaching it transitively)
#include <vector>

#undef LOG_TAG
#define LOG_TAG "DngWriter"

namespace dng {

// ACAMERA color filter arrangement -> DNG CFAPattern (0=R 1=G 2=B), 2x2 row-major.
static void cfa_pattern(int32_t cfa, unsigned char out[4]) {
    switch (cfa) {
        case 0: out[0]=0; out[1]=1; out[2]=1; out[3]=2; break; // RGGB
        case 1: out[0]=1; out[1]=0; out[2]=2; out[3]=1; break; // GRBG
        case 2: out[0]=1; out[1]=2; out[2]=0; out[3]=1; break; // GBRG
        case 3: out[0]=2; out[1]=1; out[2]=1; out[3]=0; break; // BGGR
        default: out[0]=0; out[1]=1; out[2]=1; out[3]=2; break;
    }
}

// Sensor mount degrees -> TIFF orientation enum (1=top-left ... 8).
static unsigned short tiff_orientation(int32_t deg) {
    switch (((deg % 360) + 360) % 360) {
        case 90:  return 6;
        case 180: return 3;
        case 270: return 8;
        default:  return 1;
    }
}

bool write_dng(const std::string& path, const uint16_t* data, const DngMeta& m) {
    if (!data || m.width <= 0 || m.height <= 0) {
        LOGE("write_dng: invalid input");
        return false;
    }

    tinydngwriter::DNGImage dng;
    dng.SetBigEndian(false);

    dng.SetSubfileType(false, false, false);
    dng.SetImageWidth(static_cast<unsigned int>(m.width));
    dng.SetImageLength(static_cast<unsigned int>(m.height));
    dng.SetRowsPerStrip(static_cast<unsigned int>(m.height));
    dng.SetSamplesPerPixel(1);

    unsigned short bps = 16;
    dng.SetBitsPerSample(1, &bps);
    unsigned short sfmt = 1; // unsigned integer
    dng.SetSampleFormat(1, &sfmt);

    dng.SetPlanarConfig(1);                 // chunky
    dng.SetCompression(1);                  // none
    dng.SetPhotometric(32803);              // CFA
    dng.SetOrientation(tiff_orientation(m.orientation_deg));

    dng.SetDNGVersion(1, 4, 0, 0);
    dng.SetDNGBackwardVersion(1, 4, 0, 0);
    dng.SetUniqueCameraModel(m.camera_model);
    dng.SetSoftware("camera_without_blood");

    // Bayer mosaic layout.
    dng.SetCFARepeatPatternDim(2, 2);
    unsigned char cfa[4];
    cfa_pattern(m.cfa, cfa);
    dng.SetCFAPattern(4, cfa);

    // Black/white levels.
    dng.SetBlackLevelRepeatDim(2, 2);
    unsigned short black[4];
    for (int i = 0; i < 4; ++i) {
        double v = m.black_level[i];
        black[i] = static_cast<unsigned short>(v < 0 ? 0 : v + 0.5);
    }
    dng.SetBlackLevel(4, black);
    // WhiteLevel must be SHORT/LONG, not RATIONAL, or Adobe dng_sdk (Android's
    // RAW decoder, e.g. ImageDecoder/BitmapFactory) rejects the file. Write it
    // as LONG — same value, no data loss.
    unsigned int whitev[1] = {static_cast<unsigned int>(m.white_level)};
    dng.SetWhiteLevel(1, whitev);

    if (m.has_active) {
        unsigned int aa[4] = {
            static_cast<unsigned int>(m.active_xywh[1]),                       // top
            static_cast<unsigned int>(m.active_xywh[0]),                       // left
            static_cast<unsigned int>(m.active_xywh[1] + m.active_xywh[3]),    // bottom
            static_cast<unsigned int>(m.active_xywh[0] + m.active_xywh[2]),    // right
        };
        dng.SetActiveArea(aa);
    }

    // Color calibration.
    if (m.has_cm1) dng.SetColorMatrix1(3, m.color_matrix1);
    if (m.has_cm2) dng.SetColorMatrix2(3, m.color_matrix2);
    if (m.has_fm1) dng.SetForwardMatrix1(3, m.forward_matrix1);
    if (m.has_fm2) dng.SetForwardMatrix2(3, m.forward_matrix2);
    if (m.has_cc1) dng.SetCameraCalibration1(3, m.calibration1);
    if (m.has_cc2) dng.SetCameraCalibration2(3, m.calibration2);
    if (m.illuminant1) dng.SetCalibrationIlluminant1(static_cast<unsigned short>(m.illuminant1));
    if (m.illuminant2) dng.SetCalibrationIlluminant2(static_cast<unsigned short>(m.illuminant2));

    if (m.has_neutral) dng.SetAsShotNeutral(3, m.as_shot_neutral);

    // Pixel data: copy into a tight little-endian 16-bit buffer, dropping any
    // row padding (RAW16 stride often exceeds width*2).
    const size_t tight_stride = static_cast<size_t>(m.width) * 2;
    const size_t src_stride =
        m.stride_bytes > 0 ? static_cast<size_t>(m.stride_bytes) : tight_stride;
    if (m.width <= 0 || m.height <= 0 || src_stride < tight_stride) {
        LOGE("bad RAW geometry %dx%d stride=%d — refusing to write %s",
             m.width, m.height, m.stride_bytes, path.c_str());
        return false;
    }
    std::vector<unsigned char> buf(tight_stride * m.height);
    const unsigned char* src = reinterpret_cast<const unsigned char*>(data);
    for (int y = 0; y < m.height; ++y) {
        std::memcpy(buf.data() + y * tight_stride, src + y * src_stride, tight_stride);
    }
    dng.SetImageData(buf.data(), buf.size());

    tinydngwriter::DNGWriter writer(false);
    writer.AddImage(&dng);

    std::string err;
    if (!writer.WriteToFile(path.c_str(), &err)) {
        LOGE("DNG WriteToFile failed: %s", err.c_str());
        return false;
    }
    LOGI("DNG written: %s (%dx%d)", path.c_str(), m.width, m.height);
    return true;
}

} // namespace dng
