#include "evk/platform.h"

#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <jni.h>

#include <cstdio>

#include "evk/log.h"

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
        // renderer 传进来的是 printf 风格格式串，先格式化成文本再走 spdlog，
        // 避免 % 占位符与 fmt 的花括号语法冲突。
        char buffer[1024];
        vsnprintf(buffer, sizeof(buffer), fmt, args);
        switch (level) {
            case LogLevel::Verbose: SPDLOG_TRACE("[{}] {}", tag, buffer);    break;
            case LogLevel::Debug:   SPDLOG_DEBUG("[{}] {}", tag, buffer);    break;
            case LogLevel::Info:    SPDLOG_INFO("[{}] {}", tag, buffer);     break;
            case LogLevel::Warn:    SPDLOG_WARN("[{}] {}", tag, buffer);     break;
            case LogLevel::Error:   SPDLOG_ERROR("[{}] {}", tag, buffer);    break;
        }
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
