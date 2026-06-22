#include "app.hh"
#include <android_native_app_glue.h>
#include "logger.hh"
#include <string>

void android_main(android_app* state) {
    if (state->activity->externalDataPath) {
        std::string log_path = std::string(state->activity->externalDataPath) + "/app.log";
        init_logger(log_path.c_str());
    } else {
        init_logger("/sdcard/Download/app.log");
    }
    LOGI("App started, logger initialized");
    App app(state);
    app.run();
}
