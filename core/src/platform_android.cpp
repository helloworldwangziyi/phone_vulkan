#include "evk/platform.h"

#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <jni.h>

namespace evk {

class AndroidPlatform : public IPlatform {
public:
    AndroidPlatform(JNIEnv* env, jobject surface)
        : env_(env), surface_(surface) {}

    ~AndroidPlatform() {
        if (window_) {
            ANativeWindow_release(window_);
        }
    }

    bool createVulkanSurface(VkInstance instance, VkSurfaceKHR* surface) override {
        if (!surface_) {
            logMessage(this, LogLevel::Error, "evk", "Android surface is null");
            return false;
        }

        window_ = ANativeWindow_fromSurface(env_, surface_);
        if (!window_) {
            logMessage(this, LogLevel::Error, "evk", "ANativeWindow_fromSurface failed");
            return false;
        }

        VkAndroidSurfaceCreateInfoKHR createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
        createInfo.window = window_;

        if (vkCreateAndroidSurfaceKHR(instance, &createInfo, nullptr, surface) != VK_SUCCESS) {
            logMessage(this, LogLevel::Error, "evk", "vkCreateAndroidSurfaceKHR failed");
            return false;
        }
        return true;
    }

    void getSurfaceSize(uint32_t* width, uint32_t* height) override {
        if (window_) {
            *width = static_cast<uint32_t>(ANativeWindow_getWidth(window_));
            *height = static_cast<uint32_t>(ANativeWindow_getHeight(window_));
        } else {
            *width = 0;
            *height = 0;
        }
    }

    void log(LogLevel level, const char* tag, const char* fmt, va_list args) override {
        android_LogPriority priority = ANDROID_LOG_INFO;
        switch (level) {
            case LogLevel::Verbose: priority = ANDROID_LOG_VERBOSE; break;
            case LogLevel::Debug:   priority = ANDROID_LOG_DEBUG;   break;
            case LogLevel::Info:    priority = ANDROID_LOG_INFO;    break;
            case LogLevel::Warn:    priority = ANDROID_LOG_WARN;    break;
            case LogLevel::Error:   priority = ANDROID_LOG_ERROR;   break;
        }
        __android_log_vprint(priority, tag, fmt, args);
    }

private:
    JNIEnv* env_ = nullptr;
    jobject surface_ = nullptr;
    ANativeWindow* window_ = nullptr;
};

IPlatform* createAndroidPlatform(JNIEnv* env, jobject surface) {
    return new AndroidPlatform(env, surface);
}

void destroyAndroidPlatform(IPlatform* platform) {
    delete platform;
}

void logMessage(IPlatform* platform, LogLevel level, const char* tag, const char* fmt, ...) {
    if (!platform) return;
    va_list args;
    va_start(args, fmt);
    platform->log(level, tag, fmt, args);
    va_end(args);
}

} // namespace evk

extern "C" {
// Provide these factory functions with C linkage so the JNI bridge does not
// need to know the class layout.
evk::IPlatform* evkCreateAndroidPlatform(JNIEnv* env, jobject surface) {
    return evk::createAndroidPlatform(env, surface);
}

void evkDestroyAndroidPlatform(evk::IPlatform* platform) {
    evk::destroyAndroidPlatform(platform);
}
} // extern "C"
