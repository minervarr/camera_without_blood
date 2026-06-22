#pragma once
#include <jni.h>
#include <android_native_app_glue.h>

#include "permissions.hh"  // reuse perm::env_for() + perm::check_exc()

#include "logger.hh"

// ---------------------------------------------------------------------------
// Fullscreen / immersive helper for a pure-NativeActivity app (no Java UI).
//
// Hides the status bar and navigation bar by calling
//   getWindow().getDecorView().setSystemUiVisibility(flags)
// via JNI. Min SDK is 26, so we use the legacy setSystemUiVisibility flags
// (WindowInsetsController is API 30+).
//
// Behavior: non-sticky immersive (SYSTEM_UI_FLAG_IMMERSIVE) — both bars hidden;
// an edge swipe brings them back until re-hidden. The system clears these flags
// on focus changes (e.g. the permission dialog), so this must be re-applied when
// focus returns (see App::handle_cmd APP_CMD_GAINED_FOCUS).
//
// Follows the same JNI rules as permissions.hh: every call that can raise is
// followed by an exception check, and local refs are released.
// ---------------------------------------------------------------------------

// Combined View.SYSTEM_UI_FLAG_* values (stable public API constants):
//   LAYOUT_STABLE          0x100
//   LAYOUT_HIDE_NAVIGATION 0x200
// We only hide the status bar (FULLSCREEN) and leave the navigation bar visible.
// This ensures the window bounds are sized to stop exactly at the navigation bar,
// preventing our UI from overlapping underneath it.
static constexpr jint kImmersiveFlags = 0x100 | 0x4;

inline void enable_immersive(android_app* app) {
    JNIEnv* env = perm::env_for(app);
    if (!env) return;

    jobject activity = app->activity->clazz;
    jclass  act_cls  = env->GetObjectClass(activity);

    jmethodID get_window = env->GetMethodID(act_cls, "getWindow",
                                            "()Landroid/view/Window;");
    if (perm::check_exc(env, "GetMethodID(getWindow)") || !get_window) {
        env->DeleteLocalRef(act_cls);
        return;
    }
    jobject window = env->CallObjectMethod(activity, get_window);
    if (perm::check_exc(env, "getWindow") || !window) {
        env->DeleteLocalRef(act_cls);
        return;
    }

    jclass    win_cls    = env->GetObjectClass(window);
    jmethodID get_decor  = env->GetMethodID(win_cls, "getDecorView",
                                            "()Landroid/view/View;");
    if (perm::check_exc(env, "GetMethodID(getDecorView)") || !get_decor) {
        env->DeleteLocalRef(win_cls);
        env->DeleteLocalRef(window);
        env->DeleteLocalRef(act_cls);
        return;
    }
    jobject decor = env->CallObjectMethod(window, get_decor);
    if (perm::check_exc(env, "getDecorView") || !decor) {
        env->DeleteLocalRef(win_cls);
        env->DeleteLocalRef(window);
        env->DeleteLocalRef(act_cls);
        return;
    }

    jclass    view_cls = env->GetObjectClass(decor);
    jmethodID set_vis  = env->GetMethodID(view_cls, "setSystemUiVisibility", "(I)V");
    if (perm::check_exc(env, "GetMethodID(setSystemUiVisibility)") || !set_vis) {
        env->DeleteLocalRef(view_cls);
        env->DeleteLocalRef(decor);
        env->DeleteLocalRef(win_cls);
        env->DeleteLocalRef(window);
        env->DeleteLocalRef(act_cls);
        return;
    }

    env->CallVoidMethod(decor, set_vis, kImmersiveFlags);
    perm::check_exc(env, "setSystemUiVisibility");

    env->DeleteLocalRef(view_cls);
    env->DeleteLocalRef(decor);
    env->DeleteLocalRef(win_cls);
    env->DeleteLocalRef(window);
    env->DeleteLocalRef(act_cls);
}
