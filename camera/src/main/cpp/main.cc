#include "app.hh"
#include <android_native_app_glue.h>
#include "android_host.hh"
#include "logger.hh"
#include <memory>
#include <string>

void android_main(android_app* state) {
    if (state->activity->externalDataPath) {
        std::string log_path = std::string(state->activity->externalDataPath) + "/app.log";
        init_logger(log_path.c_str());
    } else {
        init_logger("/sdcard/Download/app.log");
    }
    LOGI("App started, logger initialized");

    // no launch extra, no fallback, no all-files-access prompt (camera uses
    // getExternalFilesDir, which is always granted).
    auto host = std::make_unique<AndroidHost>(state, nullptr, nullptr, false);
    App app(std::move(host));
    app.run();
}
