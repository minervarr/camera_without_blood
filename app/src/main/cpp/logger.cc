#include "logger.hh"
#include <cstdio>
#include <cstdarg>

static FILE* g_log_file = nullptr;

void init_logger(const char* file_path) {
    g_log_file = fopen(file_path, "w");
    if (!g_log_file) {
        __android_log_print(ANDROID_LOG_ERROR, "Logger", "Failed to open log file %s", file_path);
    } else {
        __android_log_print(ANDROID_LOG_INFO, "Logger", "Logging to %s", file_path);
    }
}

void file_log(int prio, const char* tag, const char* fmt, ...) {
    va_list args;
    
    va_start(args, fmt);
    __android_log_vprint(prio, tag, fmt, args);
    va_end(args);

    if (g_log_file) {
        fprintf(g_log_file, "[%s] ", tag);
        va_start(args, fmt);
        vfprintf(g_log_file, fmt, args);
        va_end(args);
        fprintf(g_log_file, "\n");
        fflush(g_log_file);
    }
}
