#include "jni_camera.hh"

#include "../logger.hh"
#include "../camera/still_writer.hh"
#include <android/hardware_buffer_jni.h>

#include <mutex>

#undef LOG_TAG
#define LOG_TAG "JniCamera"

namespace jni {

namespace {

JavaVM*    g_vm        = nullptr;
jobject    g_session   = nullptr;  // global ref to HdrCameraSession instance
jmethodID  g_start     = nullptr;
jmethodID  g_stop      = nullptr;
jmethodID  g_release   = nullptr;  // releaseImage(Image)
jmethodID  g_start_rec = nullptr;  // startRecording()
jmethodID  g_stop_rec  = nullptr;  // stopRecording()
jmethodID  g_take_photo = nullptr; // takePhoto(String)
jmethodID  g_request_usb = nullptr; // requestUsbAccess()
jmethodID  g_set_raw_mode = nullptr; // setRawVideoMode(boolean)
jmethodID  g_set_photo_mode = nullptr; // setPhotoMode(boolean)
int        g_usb_fd = 0;            // libusb fd from UsbManager (0 = none)
PreviewSink  g_sink{};
RecordSink   g_record{};
RawSink      g_raw{};
RawVideoSink g_raw_video{};

// Capture-file registry: exact output paths recorded as files are written.
std::mutex               g_session_files_mtx;
std::vector<std::string> g_session_files;

// Gets a JNIEnv for the current thread, attaching if necessary. Sets *detach to
// true if the caller must DetachCurrentThread afterwards.
JNIEnv* env_for_thread(bool* detach) {
    *detach = false;
    JNIEnv* env = nullptr;
    jint r = g_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    if (r == JNI_EDETACHED) {
        if (g_vm->AttachCurrentThread(&env, nullptr) == JNI_OK) *detach = true;
        else return nullptr;
    } else if (r != JNI_OK) {
        return nullptr;
    }
    return env;
}

// Loads an app class through the NativeActivity's class loader (FindClass alone
// can't see app classes from arbitrary threads).
jclass load_app_class(JNIEnv* env, jobject activity, const char* dotted_name) {
    // Every local ref created here is released before returning: this runs on
    // camera/worker threads whose local-ref table is not popped by a returning
    // Java frame, so leaking three refs per call eventually overflows it.
    jclass activity_cls = env->GetObjectClass(activity);
    if (!activity_cls) return nullptr;
    jmethodID get_cl = env->GetMethodID(activity_cls, "getClassLoader", "()Ljava/lang/ClassLoader;");
    jobject loader = get_cl ? env->CallObjectMethod(activity, get_cl) : nullptr;
    env->DeleteLocalRef(activity_cls);
    if (!loader) return nullptr;

    jclass loader_cls = env->FindClass("java/lang/ClassLoader");
    jmethodID load = loader_cls ? env->GetMethodID(loader_cls, "loadClass",
                                                  "(Ljava/lang/String;)Ljava/lang/Class;")
                                : nullptr;
    if (loader_cls) env->DeleteLocalRef(loader_cls);
    if (!load) { env->DeleteLocalRef(loader); return nullptr; }

    jstring name = env->NewStringUTF(dotted_name);
    jclass cls = static_cast<jclass>(env->CallObjectMethod(loader, load, name));
    env->DeleteLocalRef(name);
    env->DeleteLocalRef(loader);
    return cls;
}

} // namespace

bool hdr_init(JavaVM* vm, jobject activity, PreviewSink preview, RecordSink record,
              RawSink raw, RawVideoSink raw_video) {
    g_vm        = vm;
    g_sink      = preview;
    g_record    = record;
    g_raw       = raw;
    g_raw_video = raw_video;

    bool detach = false;
    JNIEnv* env = env_for_thread(&detach);
    if (!env) { LOGE("hdr_init: no JNIEnv"); return false; }

    bool ok = false;
    jclass cls = load_app_class(env, activity, "io.nava.camera.HdrCameraSession");
    if (cls && !env->ExceptionCheck()) {
        jmethodID ctor = env->GetMethodID(cls, "<init>", "(Landroid/content/Context;J)V");
        if (ctor) {
            // nativeCtx is unused (the sink captures its own context); pass 0.
            jobject obj = env->NewObject(cls, ctor, activity, static_cast<jlong>(0));
            if (obj) {
                g_session = env->NewGlobalRef(obj);
                g_start   = env->GetMethodID(cls, "startPreview", "()V");
                g_stop    = env->GetMethodID(cls, "stopPreview",  "()V");
                g_release = env->GetMethodID(cls, "releaseImage", "(Landroid/media/Image;)V");
                g_start_rec = env->GetMethodID(cls, "startRecording", "()V");
                g_stop_rec  = env->GetMethodID(cls, "stopRecording", "()V");
                g_take_photo = env->GetMethodID(cls, "takePhoto", "(Ljava/lang/String;I)V");
                g_request_usb = env->GetMethodID(cls, "requestUsbAccess", "()V");
                g_set_raw_mode = env->GetMethodID(cls, "setRawVideoMode", "(Z)V");
                g_set_photo_mode = env->GetMethodID(cls, "setPhotoMode", "(Z)V");
                ok = (g_start && g_stop && g_release && g_start_rec && g_stop_rec &&
                      g_take_photo && g_request_usb && g_set_raw_mode && g_set_photo_mode);
                env->DeleteLocalRef(obj);
            }
        }
    }
    if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); }
    if (cls) env->DeleteLocalRef(cls);
    if (detach) g_vm->DetachCurrentThread();

    if (ok) LOGI("HdrCameraSession created");
    else    LOGE("hdr_init failed");
    return ok;
}

void hdr_set_raw_video(bool enabled) {
    if (!g_session) return;
    bool detach = false;
    JNIEnv* env = env_for_thread(&detach);
    if (!env) return;
    env->CallVoidMethod(g_session, g_set_raw_mode, enabled ? JNI_TRUE : JNI_FALSE);
    if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); }
    if (detach) g_vm->DetachCurrentThread();
}

void hdr_set_photo_mode(bool photo) {
    if (!g_session) return;
    bool detach = false;
    JNIEnv* env = env_for_thread(&detach);
    if (!env) return;
    env->CallVoidMethod(g_session, g_set_photo_mode, photo ? JNI_TRUE : JNI_FALSE);
    if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); }
    if (detach) g_vm->DetachCurrentThread();
}

void hdr_start_preview() {
    if (!g_session) return;
    bool detach = false;
    JNIEnv* env = env_for_thread(&detach);
    if (!env) return;
    env->CallVoidMethod(g_session, g_start);
    if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); }
    if (detach) g_vm->DetachCurrentThread();
}

void hdr_stop_preview() {
    if (!g_session) return;
    bool detach = false;
    JNIEnv* env = env_for_thread(&detach);
    if (!env) return;
    env->CallVoidMethod(g_session, g_stop);
    if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); }
    if (detach) g_vm->DetachCurrentThread();
}

void hdr_start_recording() {
    if (!g_session) return;
    bool detach = false;
    JNIEnv* env = env_for_thread(&detach);
    if (!env) return;
    env->CallVoidMethod(g_session, g_start_rec);
    if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); }
    if (detach) g_vm->DetachCurrentThread();
}

void hdr_stop_recording() {
    if (!g_session) return;
    bool detach = false;
    JNIEnv* env = env_for_thread(&detach);
    if (!env) return;
    env->CallVoidMethod(g_session, g_stop_rec);
    if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); }
    if (detach) g_vm->DetachCurrentThread();
}

void hdr_request_usb() {
    if (!g_session) return;
    bool detach = false;
    JNIEnv* env = env_for_thread(&detach);
    if (!env) return;
    env->CallVoidMethod(g_session, g_request_usb);
    if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); }
    if (detach) g_vm->DetachCurrentThread();
}

int usb_fd() { return g_usb_fd; }

void hdr_take_photo(const char* base_path, int shots) {
    if (!g_session) return;
    bool detach = false;
    JNIEnv* env = env_for_thread(&detach);
    if (!env) return;
    jstring s = env->NewStringUTF(base_path);
    env->CallVoidMethod(g_session, g_take_photo, s, shots);
    if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); }
    env->DeleteLocalRef(s);
    if (detach) g_vm->DetachCurrentThread();
}

// ── Host (CameraActivity) integration ────────────────────────────────────────
namespace {
// Gets a JNIEnv on a possibly-foreign thread for the given vm (the public
// helpers here are called before/independent of hdr_init, so they can't rely on
// g_vm). Sets *detach.
JNIEnv* env_for_vm(JavaVM* vm, bool* detach) {
    *detach = false;
    if (!vm) return nullptr;
    JNIEnv* env = nullptr;
    jint r = vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    if (r == JNI_EDETACHED) {
        if (vm->AttachCurrentThread(&env, nullptr) == JNI_OK) *detach = true;
        else return nullptr;
    } else if (r != JNI_OK) {
        return nullptr;
    }
    return env;
}
} // namespace

std::string host_output_dir(JavaVM* vm, jobject activity) {
    if (!vm || !activity) return "";
    bool detach = false;
    JNIEnv* env = env_for_vm(vm, &detach);
    if (!env) return "";
    std::string result;
    jclass cls = env->GetObjectClass(activity);
    jmethodID m = cls ? env->GetMethodID(cls, "cameraOutputDir", "()Ljava/lang/String;") : nullptr;
    if (m) {
        jstring s = static_cast<jstring>(env->CallObjectMethod(activity, m));
        if (s) {
            const char* c = env->GetStringUTFChars(s, nullptr);
            if (c) { result.assign(c); env->ReleaseStringUTFChars(s, c); }
            env->DeleteLocalRef(s);
        }
    }
    if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); }
    if (cls) env->DeleteLocalRef(cls);
    if (detach) vm->DetachCurrentThread();
    return result;
}

bool host_start_video(JavaVM* vm, jobject activity) {
    if (!vm || !activity) return false;
    bool detach = false;
    JNIEnv* env = env_for_vm(vm, &detach);
    if (!env) return false;
    bool result = false;
    jclass cls = env->GetObjectClass(activity);
    jmethodID m = cls ? env->GetMethodID(cls, "cameraStartVideo", "()Z") : nullptr;
    if (m) result = env->CallBooleanMethod(activity, m) == JNI_TRUE;
    if (env->ExceptionCheck()) { env->ExceptionClear(); }
    if (cls) env->DeleteLocalRef(cls);
    if (detach) vm->DetachCurrentThread();
    return result;
}

int host_photo_mode(JavaVM* vm, jobject activity) {
    if (!vm || !activity) return 0;
    bool detach = false;
    JNIEnv* env = env_for_vm(vm, &detach);
    if (!env) return 0;
    int result = 0;
    jclass cls = env->GetObjectClass(activity);
    jmethodID m = cls ? env->GetMethodID(cls, "cameraPhotoMode", "()I") : nullptr;
    if (m) result = env->CallIntMethod(activity, m);
    if (env->ExceptionCheck()) { env->ExceptionClear(); }
    if (cls) env->DeleteLocalRef(cls);
    if (detach) vm->DetachCurrentThread();
    return result;
}

bool host_photo_mode_ui(JavaVM* vm, jobject activity) {
    if (!vm || !activity) return true;
    bool detach = false;
    JNIEnv* env = env_for_vm(vm, &detach);
    if (!env) return true;
    bool result = true;
    jclass cls = env->GetObjectClass(activity);
    jmethodID m = cls ? env->GetMethodID(cls, "cameraPhotoModeUiEnabled", "()Z") : nullptr;
    if (m) result = env->CallBooleanMethod(activity, m) == JNI_TRUE;
    if (env->ExceptionCheck()) { env->ExceptionClear(); }
    if (cls) env->DeleteLocalRef(cls);
    if (detach) vm->DetachCurrentThread();
    return result;
}

void host_finish_with_results(JavaVM* vm, jobject activity,
                              const std::vector<std::string>& paths) {
    if (!vm || !activity) return;
    bool detach = false;
    JNIEnv* env = env_for_vm(vm, &detach);
    if (!env) return;
    jclass strCls = env->FindClass("java/lang/String");
    if (!strCls) { if (detach) vm->DetachCurrentThread(); return; }
    jobjectArray arr = env->NewObjectArray(static_cast<jsize>(paths.size()), strCls, nullptr);
    env->DeleteLocalRef(strCls);
    if (!arr) { if (detach) vm->DetachCurrentThread(); return; }
    for (jsize i = 0; i < static_cast<jsize>(paths.size()); ++i) {
        jstring s = env->NewStringUTF(paths[i].c_str());
        env->SetObjectArrayElement(arr, i, s);
        env->DeleteLocalRef(s);
    }
    jclass cls = env->GetObjectClass(activity);
    jmethodID m = cls ? env->GetMethodID(cls, "finishWithResults", "([Ljava/lang/String;)V") : nullptr;
    if (m) env->CallVoidMethod(activity, m, arr);
    if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); }
    if (cls) env->DeleteLocalRef(cls);
    env->DeleteLocalRef(arr);   // strCls was already released above
    if (detach) vm->DetachCurrentThread();
}

// ── Session capture-file registry ────────────────────────────────────────────
void session_files_reset() {
    std::lock_guard<std::mutex> lk(g_session_files_mtx);
    g_session_files.clear();
}

void session_record_file(const std::string& path) {
    if (path.empty()) return;
    std::lock_guard<std::mutex> lk(g_session_files_mtx);
    g_session_files.push_back(path);
}

std::vector<std::string> session_files() {
    std::lock_guard<std::mutex> lk(g_session_files_mtx);
    return g_session_files;
}

} // namespace jni

// ── Native callback invoked from Java (background camera thread) ─────────────
// `image` is kept alive (via a global ref) until the renderer signals done,
// then closed through HdrCameraSession.releaseImage so the camera can reuse the
// buffer slot.
extern "C" JNIEXPORT void JNICALL
Java_io_nava_camera_HdrCameraSession_nativeOnPreviewBuffer(
        JNIEnv* env, jclass, jlong /*native_ctx*/, jobject hardware_buffer, jobject image) {
    if (!jni::g_sink.on_frame || !hardware_buffer || !image) return;
    AHardwareBuffer* hb = AHardwareBuffer_fromHardwareBuffer(env, hardware_buffer);
    if (!hb) return;

    jobject image_ref = env->NewGlobalRef(image);
    auto release = [image_ref]() {
        bool detach = false;
        JNIEnv* e = jni::env_for_thread(&detach);
        if (e) {
            e->CallVoidMethod(jni::g_session, jni::g_release, image_ref);
            if (e->ExceptionCheck()) { e->ExceptionClear(); }
            e->DeleteGlobalRef(image_ref);
            if (detach) jni::g_vm->DetachCurrentThread();
        }
    };
    jni::g_sink.on_frame(hb, std::move(release));
}

// Encoder output format (csd: Annex B VPS/SPS/PPS). Fired once before packets.
extern "C" JNIEXPORT void JNICALL
Java_io_nava_camera_HdrCameraSession_nativeOnVideoFormat(
        JNIEnv* env, jclass, jlong, jbyteArray csd, jint width, jint height) {
    if (!jni::g_record.on_format || !csd) return;
    jsize len = env->GetArrayLength(csd);
    jbyte* buf = env->GetByteArrayElements(csd, nullptr);
    jni::g_record.on_format(reinterpret_cast<const uint8_t*>(buf), len, width, height);
    env->ReleaseByteArrayElements(csd, buf, JNI_ABORT);
}

// One encoded access unit (Annex B), with presentation timestamp in microseconds.
extern "C" JNIEXPORT void JNICALL
Java_io_nava_camera_HdrCameraSession_nativeOnVideoPacket(
        JNIEnv* env, jclass, jlong, jbyteArray data, jint size, jlong pts_us, jboolean keyframe) {
    if (!jni::g_record.on_packet || !data || size <= 0) return;
    jbyte* buf = env->GetByteArrayElements(data, nullptr);
    jni::g_record.on_packet(reinterpret_cast<const uint8_t*>(buf), size,
                            static_cast<int64_t>(pts_us), keyframe == JNI_TRUE);
    env->ReleaseByteArrayElements(data, buf, JNI_ABORT);
}

// One RAW frame of a bracketed still burst. `buf` is a direct ByteBuffer over
// the Image's 16-bit Bayer plane (valid for the duration of this call).
extern "C" JNIEXPORT void JNICALL
Java_io_nava_camera_HdrCameraSession_nativeOnRawFrame(
        JNIEnv* env, jclass, jlong, jstring path, jobject buf,
        jint width, jint height, jint stride, jfloatArray neutral, jfloatArray black, jint white,
        jlong exposure_ns, jint iso, jint index, jint count) {
    if (!jni::g_raw.on_raw || !path || !buf) return;
    auto* data = reinterpret_cast<const uint16_t*>(env->GetDirectBufferAddress(buf));
    if (!data) return;

    const char* cpath = env->GetStringUTFChars(path, nullptr);
    float neutral3[3] = {1, 1, 1}; bool has_neutral = false;
    float black4[4]   = {0, 0, 0, 0}; bool has_black = false;
    if (neutral && env->GetArrayLength(neutral) >= 3) {
        env->GetFloatArrayRegion(neutral, 0, 3, neutral3); has_neutral = true;
    }
    if (black && env->GetArrayLength(black) >= 4) {
        env->GetFloatArrayRegion(black, 0, 4, black4); has_black = true;
    }
    jni::g_raw.on_raw(cpath, data, width, height, stride,
                      has_neutral ? neutral3 : nullptr,
                      has_black ? black4 : nullptr, white,
                      static_cast<int64_t>(exposure_ns), iso, index, count);
    // NOTE: the source DNG is written (and registered) by the recorder, gated by
    // the photo output mode — so it is NOT recorded here (that would report files
    // that the merged/HDR modes never write).
    env->ReleaseStringUTFChars(path, cpath);
}

// One full-resolution YUV_420_888 still (non-RAW devices). The three planes are
// copied into Java byte[]s (the source Image is already closed), so this can run
// off the camera thread; we convert to RGB and write a lossless PNG.
extern "C" JNIEXPORT void JNICALL
Java_io_nava_camera_HdrCameraSession_nativeOnStillFrame(
        JNIEnv* env, jclass, jstring path, jbyteArray y, jbyteArray u, jbyteArray v,
        jint width, jint height, jint y_stride, jint u_stride, jint v_stride,
        jint uv_pixel_stride, jint orientation_deg) {
    if (!path || !y || !u || !v) { LOGE("nativeOnStillFrame: null arg"); return; }
    LOGI("nativeOnStillFrame %dx%d (yStride=%d uvPix=%d orient=%d)",
         width, height, y_stride, uv_pixel_stride, orientation_deg);
    const char* cpath = env->GetStringUTFChars(path, nullptr);
    jbyte* yp = env->GetByteArrayElements(y, nullptr);
    jbyte* up = env->GetByteArrayElements(u, nullptr);
    jbyte* vp = env->GetByteArrayElements(v, nullptr);
    if (cpath && yp && up && vp) {
        cam::write_png_yuv420(cpath,
                              reinterpret_cast<const uint8_t*>(yp),
                              reinterpret_cast<const uint8_t*>(up),
                              reinterpret_cast<const uint8_t*>(vp),
                              width, height, y_stride, u_stride, v_stride,
                              uv_pixel_stride, orientation_deg);
        jni::session_record_file(cpath);   // PNG written synchronously above
    }
    if (vp) env->ReleaseByteArrayElements(v, vp, JNI_ABORT);
    if (up) env->ReleaseByteArrayElements(u, up, JNI_ABORT);
    if (yp) env->ReleaseByteArrayElements(y, yp, JNI_ABORT);
    if (cpath) env->ReleaseStringUTFChars(path, cpath);
}

// One streaming RAW16 video frame (raw video mode, while recording). `buf` is
// a direct ByteBuffer over the Image's Bayer plane — valid only during this
// call; the sink copies synchronously into its staging ring.
extern "C" JNIEXPORT void JNICALL
Java_io_nava_camera_HdrCameraSession_nativeOnRawVideoFrame(
        JNIEnv* env, jclass, jobject buf, jint width, jint height,
        jint stride, jlong ts_ns) {
    if (!jni::g_raw_video.on_frame || !buf) return;
    auto* data = static_cast<const uint8_t*>(env->GetDirectBufferAddress(buf));
    if (!data) return;
    jni::g_raw_video.on_frame(data, width, height, stride, static_cast<int64_t>(ts_ns));
}

// White balance reference for the current clip (AWB-locked neutral point).
extern "C" JNIEXPORT void JNICALL
Java_io_nava_camera_HdrCameraSession_nativeOnRawVideoNeutral(
        JNIEnv* env, jclass, jfloatArray neutral) {
    if (!jni::g_raw_video.on_neutral || !neutral) return;
    if (env->GetArrayLength(neutral) < 3) return;
    float n[3];
    env->GetFloatArrayRegion(neutral, 0, 3, n);
    jni::g_raw_video.on_neutral(n);
}

// USB DAC file descriptor from UsbManager (after permission grant). 0 clears it.
extern "C" JNIEXPORT void JNICALL
Java_io_nava_camera_HdrCameraSession_nativeOnUsbFd(JNIEnv*, jclass, jint fd) {
    jni::g_usb_fd = fd;
}
