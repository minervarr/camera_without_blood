#pragma once
#include <android/log.h>

void init_logger(const char* file_path);
void file_log(int prio, const char* tag, const char* fmt, ...);

#ifndef LOG_TAG
#define LOG_TAG "App"
#endif

#define LOGI(...) file_log(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) file_log(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGD(...) file_log(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
