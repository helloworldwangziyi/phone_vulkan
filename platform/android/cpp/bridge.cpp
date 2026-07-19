#include <jni.h>
#include <android/log.h>

#include "evk/renderer.h"

// Factory functions exported from platform_android.cpp with C linkage.
extern "C" evk::IPlatform* evkCreateAndroidPlatform(JNIEnv* env, jobject surface);
extern "C" void evkDestroyAndroidPlatform(evk::IPlatform* platform);

#define LOG_TAG "estarx-vulkan"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

static evk::Renderer* g_renderer = nullptr;
static evk::IPlatform* g_platform = nullptr;

extern "C" JNIEXPORT void JNICALL
Java_com_estarx_vulkan_NativeBridge_nativeInit(JNIEnv* env, jclass /*clazz*/, jobject surface) {
    if (g_renderer) {
        return;
    }

    g_platform = evkCreateAndroidPlatform(env, surface);
    if (!g_platform) {
        LOGE("failed to create Android platform");
        return;
    }

    g_renderer = new evk::Renderer(g_platform);
    if (!g_renderer->initialize()) {
        LOGE("failed to initialize Vulkan renderer");
        delete g_renderer;
        g_renderer = nullptr;
        evkDestroyAndroidPlatform(g_platform);
        g_platform = nullptr;
        return;
    }

    LOGI("Vulkan renderer initialized");
}

extern "C" JNIEXPORT void JNICALL
Java_com_estarx_vulkan_NativeBridge_nativeResize(JNIEnv* /*env*/, jclass /*clazz*/, jint width, jint height) {
    if (g_renderer) {
        g_renderer->setSize(static_cast<uint32_t>(width), static_cast<uint32_t>(height));
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_estarx_vulkan_NativeBridge_nativeRender(JNIEnv* /*env*/, jclass /*clazz*/) {
    if (g_renderer) {
        g_renderer->render();
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_estarx_vulkan_NativeBridge_nativeDestroy(JNIEnv* /*env*/, jclass /*clazz*/) {
    if (g_renderer) {
        delete g_renderer;
        g_renderer = nullptr;
    }
    if (g_platform) {
        evkDestroyAndroidPlatform(g_platform);
        g_platform = nullptr;
    }
    LOGI("Vulkan renderer destroyed");
}
