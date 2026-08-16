#pragma once
#include <jni.h>
#include <android_native_app_glue.h>

#include "jni_util.hh"  // vce::platform::jni::env_for() + check_exc() (canvas lib)
#include "host.hh"

// ---------------------------------------------------------------------------
// App-specific runtime permission checks for this camera recorder. The generic
// JNI substrate (thread-attach + exception checks) now lives in the canvas
// library as vce::platform::jni; this header only owns the permission *list*
// this app requires (CAMERA / RECORD_AUDIO / WRITE_EXTERNAL_STORAGE).
// ---------------------------------------------------------------------------

// Returns true only if every permission the app needs is already granted.
inline bool has_permissions(android_app* app) {
    JNIEnv* env = vce::platform::jni::env_for(app);
    if (!env) return false;

    jobject activity = app->activity->clazz;
    jclass  cls      = env->GetObjectClass(activity);
    jmethodID check  = env->GetMethodID(cls, "checkSelfPermission",
                                        "(Ljava/lang/String;)I");
    if (vce::platform::jni::check_exc(env, "GetMethodID(checkSelfPermission)") || !check) {
        return false;
    }

    static const char* kPerms[] = {
        "android.permission.CAMERA",
        "android.permission.RECORD_AUDIO",
        "android.permission.WRITE_EXTERNAL_STORAGE",
    };

    bool all_granted = true;
    for (const char* name : kPerms) {
        jstring s = env->NewStringUTF(name);
        jint    r = env->CallIntMethod(activity, check, s);
        env->DeleteLocalRef(s);
        if (vce::platform::jni::check_exc(env, "checkSelfPermission")) { all_granted = false; break; }
        if (r != 0 /* PERMISSION_GRANTED */) { all_granted = false; }
    }

    env->DeleteLocalRef(cls);
    return all_granted;
}

// Host-based overload: extracts android_app* from the Host's nativeApp().
inline bool has_permissions(Host* host) {
    auto* app = static_cast<android_app*>(host->nativeApp());
    return app ? has_permissions(app) : false;
}

// Fires the system permission dialog. Safe to call repeatedly; it no-ops if
// everything is already granted. The grant result is observed by polling
// has_permissions() once focus returns (see App::handle_cmd).
inline void request_permissions(android_app* app) {
    if (has_permissions(app)) return;

    JNIEnv* env = vce::platform::jni::env_for(app);
    if (!env) return;

    jobject activity = app->activity->clazz;
    jclass  cls      = env->GetObjectClass(activity);

    jmethodID request = env->GetMethodID(cls, "requestPermissions",
                                         "([Ljava/lang/String;I)V");
    if (vce::platform::jni::check_exc(env, "GetMethodID(requestPermissions)") || !request) {
        env->DeleteLocalRef(cls);
        return;
    }

    jclass str_cls = env->FindClass("java/lang/String");
    jobjectArray arr = env->NewObjectArray(3, str_cls, nullptr);
    jstring p0 = env->NewStringUTF("android.permission.CAMERA");
    jstring p1 = env->NewStringUTF("android.permission.RECORD_AUDIO");
    jstring p2 = env->NewStringUTF("android.permission.WRITE_EXTERNAL_STORAGE");
    env->SetObjectArrayElement(arr, 0, p0);
    env->SetObjectArrayElement(arr, 1, p1);
    env->SetObjectArrayElement(arr, 2, p2);

    env->CallVoidMethod(activity, request, arr, 123);
    vce::platform::jni::check_exc(env, "requestPermissions");

    env->DeleteLocalRef(p0);
    env->DeleteLocalRef(p1);
    env->DeleteLocalRef(p2);
    env->DeleteLocalRef(arr);
    env->DeleteLocalRef(str_cls);
    env->DeleteLocalRef(cls);
}

// Host-based overload: extracts android_app* from the Host's nativeApp().
inline void request_permissions(Host* host) {
    auto* app = static_cast<android_app*>(host->nativeApp());
    if (app) request_permissions(app);
}
