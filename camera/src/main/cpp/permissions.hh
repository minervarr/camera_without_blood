#pragma once
#include <jni.h>
#include <android_native_app_glue.h>

#include "logger.hh"

// ---------------------------------------------------------------------------
// Runtime permission helpers for a pure-NativeActivity app (no Java UI).
//
// IMPORTANT JNI rules honoured here:
//  * The android_main glue thread is attached to the JVM ONCE and left
//    attached for the lifetime of the thread. Attaching/detaching on every
//    call is unnecessary and detaching a thread that later does more JNI work
//    is a common source of crashes.
//  * Every JNI call that can raise is followed by an exception check. A
//    pending (uncleared) exception makes the *next* JNI call abort the whole
//    process -- which manifests as an instant "black screen".
// ---------------------------------------------------------------------------

namespace perm {

// Attach the calling (glue) thread once and reuse the env. Never detached.
inline JNIEnv* env_for(android_app* app) {
    JNIEnv* env = nullptr;
    JavaVM* vm  = app->activity->vm;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK) {
        return env;
    }
    if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK) {
        LOGE("permissions: AttachCurrentThread failed");
        return nullptr;
    }
    return env;
}

// Clears any pending exception so it cannot poison subsequent JNI calls.
inline bool check_exc(JNIEnv* env, const char* where) {
    if (env->ExceptionCheck()) {
        LOGE("permissions: JNI exception at %s", where);
        env->ExceptionDescribe();
        env->ExceptionClear();
        return true;
    }
    return false;
}

} // namespace perm

// Returns true only if every permission the app needs is already granted.
inline bool has_permissions(android_app* app) {
    JNIEnv* env = perm::env_for(app);
    if (!env) return false;

    jobject activity = app->activity->clazz;
    jclass  cls      = env->GetObjectClass(activity);
    jmethodID check  = env->GetMethodID(cls, "checkSelfPermission",
                                        "(Ljava/lang/String;)I");
    if (perm::check_exc(env, "GetMethodID(checkSelfPermission)") || !check) {
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
        if (perm::check_exc(env, "checkSelfPermission")) { all_granted = false; break; }
        if (r != 0 /* PERMISSION_GRANTED */) { all_granted = false; }
    }

    env->DeleteLocalRef(cls);
    return all_granted;
}

// Fires the system permission dialog. Safe to call repeatedly; it no-ops if
// everything is already granted. The grant result is observed by polling
// has_permissions() once focus returns (see App::handle_cmd).
inline void request_permissions(android_app* app) {
    if (has_permissions(app)) return;

    JNIEnv* env = perm::env_for(app);
    if (!env) return;

    jobject activity = app->activity->clazz;
    jclass  cls      = env->GetObjectClass(activity);

    jmethodID request = env->GetMethodID(cls, "requestPermissions",
                                         "([Ljava/lang/String;I)V");
    if (perm::check_exc(env, "GetMethodID(requestPermissions)") || !request) {
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
    perm::check_exc(env, "requestPermissions");

    env->DeleteLocalRef(p0);
    env->DeleteLocalRef(p1);
    env->DeleteLocalRef(p2);
    env->DeleteLocalRef(arr);
    env->DeleteLocalRef(str_cls);
    env->DeleteLocalRef(cls);
}
