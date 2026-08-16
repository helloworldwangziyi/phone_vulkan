#include "evk/render_platform.h"

#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <jni.h>

#include "evk/log.h"

namespace evk {

class AndroidPlatform : public IPlatform {
public:
    AndroidPlatform(JNIEnv* env, jobject surface)
        : env_(env), surface_(surface) {
        // 析构时要 DeleteGlobalRef，需要 JavaVM 反查/附着当前线程拿 env。
        env->GetJavaVM(&jvm_);
    }

    ~AndroidPlatform() {
        if (window_) {
            ANativeWindow_release(window_);
        }
        // surface_ 是全局引用（见 evkCreateAndroidPlatform），析构时必须释放。
        if (surface_ && jvm_) {
            JNIEnv* env = nullptr;
            bool attached = false;
            if (jvm_->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK &&
                jvm_->AttachCurrentThread(&env, nullptr) == JNI_OK) {
                attached = true;
            }
            if (env) {
                env->DeleteGlobalRef(surface_);
            }
            if (attached) {
                jvm_->DetachCurrentThread();
            }
            surface_ = nullptr;
        }
    }

    const char* getSurfaceExtensionName() const override {
        return VK_KHR_ANDROID_SURFACE_EXTENSION_NAME;
    }

    bool createVulkanSurface(VkInstance instance, VkSurfaceKHR* surface) override {
        if (!surface_) {
            EVK_LOGE("Android surface is null");
            return false;
        }

        window_ = ANativeWindow_fromSurface(env_, surface_);
        if (!window_) {
            EVK_LOGE("ANativeWindow_fromSurface failed");
            return false;
        }

        VkAndroidSurfaceCreateInfoKHR createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
        createInfo.window = window_;

        if (vkCreateAndroidSurfaceKHR(instance, &createInfo, nullptr, surface) != VK_SUCCESS) {
            EVK_LOGE("vkCreateAndroidSurfaceKHR failed");
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

private:
    JNIEnv* env_ = nullptr;
    jobject surface_ = nullptr; // 全局引用，析构时 DeleteGlobalRef
    JavaVM* jvm_ = nullptr;
    ANativeWindow* window_ = nullptr;
};

IPlatform* createAndroidPlatform(JNIEnv* env, jobject surface) {
    return new AndroidPlatform(env, surface);
}

void destroyAndroidPlatform(IPlatform* platform) {
    delete platform;
}

} // namespace evk

extern "C" {
// Provide these factory functions with C linkage so the JNI bridge does not
// need to know the class layout.
evk::IPlatform* evkCreateAndroidPlatform(JNIEnv* env, jobject surface) {
    // Java 传来的 surface 是 JNI 局部引用，nativeInit 返回后即失效；
    // 跨调用持有必须先提升为全局引用（析构时 DeleteGlobalRef，见 ~AndroidPlatform）。
    jobject globalSurface = surface ? env->NewGlobalRef(surface) : nullptr;
    return evk::createAndroidPlatform(env, globalSurface);
}

void evkDestroyAndroidPlatform(evk::IPlatform* platform) {
    evk::destroyAndroidPlatform(platform);
}
} // extern "C"
