#include "archive.hh"
#include <sys/stat.h>
#include <errno.h>

namespace archive {

static bool create_dir_recursive(const std::string& path) {
    size_t pos = 0;
    do {
        pos = path.find_first_of('/', pos + 1);
        std::string subdir = path.substr(0, pos);
        if (subdir.empty() || subdir == "/") continue;

        struct stat st;
        if (stat(subdir.c_str(), &st) != 0) {
            if (mkdir(subdir.c_str(), 0777) != 0 && errno != EEXIST) {
                return false;
            }
        }
    } while (pos != std::string::npos);
    return true;
}

std::string get_documents_path(JavaVM* vm, jobject activity,
                               const std::string& subfolder) {
    if (!vm || !activity) return "";

    JNIEnv* env = nullptr;
    vm->AttachCurrentThread(&env, nullptr);
    if (!env) return "";

    jclass envClass = env->FindClass("android/os/Environment");
    if (!envClass) return "";

    jfieldID docsField = env->GetStaticFieldID(envClass, "DIRECTORY_DOCUMENTS", "Ljava/lang/String;");
    jstring docsStr = (jstring)env->GetStaticObjectField(envClass, docsField);

    jmethodID getExtStoragePubDir = env->GetStaticMethodID(envClass,
        "getExternalStoragePublicDirectory", "(Ljava/lang/String;)Ljava/io/File;");

    jobject fileObj = env->CallStaticObjectMethod(envClass, getExtStoragePubDir, docsStr);

    jclass fileClass = env->FindClass("java/io/File");
    jmethodID getPath = env->GetMethodID(fileClass, "getAbsolutePath", "()Ljava/lang/String;");

    jstring pathStr = (jstring)env->CallObjectMethod(fileObj, getPath);

    const char* pathChars = env->GetStringUTFChars(pathStr, nullptr);
    std::string path(pathChars);
    env->ReleaseStringUTFChars(pathStr, pathChars);

    vm->DetachCurrentThread();

    if (!subfolder.empty()) {
        path += "/" + subfolder;
    }

    create_dir_recursive(path);
    return path;
}

bool ensure_dir(const std::string& path) {
    return create_dir_recursive(path);
}

} // namespace archive
