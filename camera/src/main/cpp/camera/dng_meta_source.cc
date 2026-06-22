#include "dng_meta_source.hh"

#include "../logger.hh"
#include <camera/NdkCameraManager.h>
#include <camera/NdkCameraMetadata.h>
#include <camera/NdkCameraMetadataTags.h>
#include <media/NdkImage.h>
#include <string>
#include <vector>

#undef LOG_TAG
#define LOG_TAG "DngMeta"

namespace dng {

static bool read_matrix9(ACameraMetadata* meta, uint32_t tag, double out[9]) {
    ACameraMetadata_const_entry e{};
    if (ACameraMetadata_getConstEntry(meta, tag, &e) != ACAMERA_OK || e.count < 9) return false;
    for (int i = 0; i < 9; ++i) {
        int32_t den = e.data.r[i].denominator;
        out[i] = den ? double(e.data.r[i].numerator) / den : 0.0;
    }
    return true;
}

bool load_static_meta(DngMeta& m, int& raw_width, int& raw_height) {
    ACameraManager* mgr = ACameraManager_create();
    if (!mgr) return false;

    ACameraIdList* ids = nullptr;
    if (ACameraManager_getCameraIdList(mgr, &ids) != ACAMERA_OK || !ids) {
        ACameraManager_delete(mgr);
        return false;
    }

    bool ok = false;
    for (int i = 0; i < ids->numCameras; ++i) {
        ACameraMetadata* meta = nullptr;
        if (ACameraManager_getCameraCharacteristics(mgr, ids->cameraIds[i], &meta) != ACAMERA_OK)
            continue;

        ACameraMetadata_const_entry facing{};
        if (ACameraMetadata_getConstEntry(meta, ACAMERA_LENS_FACING, &facing) == ACAMERA_OK &&
            facing.count >= 1 && facing.data.u8[0] == ACAMERA_LENS_FACING_BACK) {

            ACameraMetadata_const_entry e{};
            if (ACameraMetadata_getConstEntry(meta, ACAMERA_SENSOR_INFO_COLOR_FILTER_ARRANGEMENT, &e) == ACAMERA_OK && e.count >= 1)
                m.cfa = e.data.u8[0];
            if (ACameraMetadata_getConstEntry(meta, ACAMERA_SENSOR_INFO_WHITE_LEVEL, &e) == ACAMERA_OK && e.count >= 1)
                m.white_level = e.data.i32[0];
            if (ACameraMetadata_getConstEntry(meta, ACAMERA_SENSOR_BLACK_LEVEL_PATTERN, &e) == ACAMERA_OK && e.count >= 4)
                for (int k = 0; k < 4; ++k) m.black_level[k] = float(e.data.i32[k]);
            if (ACameraMetadata_getConstEntry(meta, ACAMERA_SENSOR_INFO_ACTIVE_ARRAY_SIZE, &e) == ACAMERA_OK && e.count >= 4) {
                m.active_xywh[0] = e.data.i32[0];
                m.active_xywh[1] = e.data.i32[1];
                m.active_xywh[2] = e.data.i32[2] - e.data.i32[0];
                m.active_xywh[3] = e.data.i32[3] - e.data.i32[1];
                m.has_active = true;
            }
            m.has_cm1 = read_matrix9(meta, ACAMERA_SENSOR_COLOR_TRANSFORM1,       m.color_matrix1);
            m.has_cm2 = read_matrix9(meta, ACAMERA_SENSOR_COLOR_TRANSFORM2,       m.color_matrix2);
            m.has_fm1 = read_matrix9(meta, ACAMERA_SENSOR_FORWARD_MATRIX1,        m.forward_matrix1);
            m.has_fm2 = read_matrix9(meta, ACAMERA_SENSOR_FORWARD_MATRIX2,        m.forward_matrix2);
            m.has_cc1 = read_matrix9(meta, ACAMERA_SENSOR_CALIBRATION_TRANSFORM1, m.calibration1);
            m.has_cc2 = read_matrix9(meta, ACAMERA_SENSOR_CALIBRATION_TRANSFORM2, m.calibration2);
            if (ACameraMetadata_getConstEntry(meta, ACAMERA_SENSOR_REFERENCE_ILLUMINANT1, &e) == ACAMERA_OK && e.count >= 1)
                m.illuminant1 = e.data.u8[0];
            if (ACameraMetadata_getConstEntry(meta, ACAMERA_SENSOR_REFERENCE_ILLUMINANT2, &e) == ACAMERA_OK && e.count >= 1)
                m.illuminant2 = e.data.u8[0];
            if (ACameraMetadata_getConstEntry(meta, ACAMERA_SENSOR_ORIENTATION, &e) == ACAMERA_OK && e.count >= 1)
                m.orientation_deg = e.data.i32[0];

            // RAW16 stream size from the available stream configurations.
            if (ACameraMetadata_getConstEntry(meta, ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS, &e) == ACAMERA_OK) {
                int64_t best = -1;
                for (uint32_t j = 0; j + 3 < e.count; j += 4) {
                    if (e.data.i32[j] == AIMAGE_FORMAT_RAW16 && e.data.i32[j + 3] == 0) {
                        int64_t area = int64_t(e.data.i32[j + 1]) * e.data.i32[j + 2];
                        if (area > best) { best = area; raw_width = e.data.i32[j + 1]; raw_height = e.data.i32[j + 2]; }
                    }
                }
                ok = best > 0;
            }
            m.width = raw_width; m.height = raw_height;
            ACameraMetadata_free(meta);
            break;
        }
        ACameraMetadata_free(meta);
    }

    ACameraManager_deleteCameraIdList(ids);
    ACameraManager_delete(mgr);
    if (ok) LOGI("Static DNG meta loaded: RAW %dx%d cfa=%d white=%d", raw_width, raw_height, m.cfa, m.white_level);
    else    LOGE("No back-camera RAW support for DNG");
    return ok;
}

} // namespace dng
