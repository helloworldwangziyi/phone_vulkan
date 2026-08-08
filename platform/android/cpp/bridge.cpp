#include <jni.h>

#include "evk/event.h"
#include "evk/log.h"
#include "evk/renderer.h"

// Factory functions exported from platform_android.cpp with C linkage.
extern "C" evk::IPlatform* evkCreateAndroidPlatform(JNIEnv* env, jobject surface);
extern "C" void evkDestroyAndroidPlatform(evk::IPlatform* platform);

static evk::Renderer* g_renderer = nullptr;
static evk::IPlatform* g_platform = nullptr;

extern "C" JNIEXPORT void JNICALL
Java_com_estarx_vulkan_NativeBridge_nativeInit(JNIEnv* env, jclass /*clazz*/, jobject surface) {
    evk::log::init("estarx-vulkan");
    if (g_renderer) {
        return;
    }

    g_platform = evkCreateAndroidPlatform(env, surface);
    if (!g_platform) {
        EVK_LOGE("failed to create Android platform");
        return;
    }

    g_renderer = new evk::Renderer(g_platform);
    if (!g_renderer->initialize()) {
        EVK_LOGE("failed to initialize Vulkan renderer");
        delete g_renderer;
        g_renderer = nullptr;
        evkDestroyAndroidPlatform(g_platform);
        g_platform = nullptr;
        return;
    }

    EVK_LOGI("Vulkan renderer initialized");
    evk::dispatchEvent(evk::EventId::AppStart, nullptr);
}

extern "C" JNIEXPORT void JNICALL
Java_com_estarx_vulkan_NativeBridge_nativeResize(JNIEnv* /*env*/, jclass /*clazz*/, jint width, jint height) {
    evk::SurfaceChangedData data{ width, height };
    evk::dispatchEvent(evk::EventId::SurfaceChanged, &data);
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
Java_com_estarx_vulkan_NativeBridge_nativeOnTouch(JNIEnv* /*env*/, jclass /*clazz*/,
                                                  jint action, jfloat x, jfloat y) {
    EVK_LOGI("touch event: action={} x={:.1f} y={:.1f}", action, x, y);
    evk::TouchData data{ action, x, y };
    evk::dispatchEvent(evk::EventId::Touch, &data);
}

extern "C" JNIEXPORT void JNICALL
Java_com_estarx_vulkan_NativeBridge_nativeDestroy(JNIEnv* /*env*/, jclass /*clazz*/) {
    evk::dispatchEvent(evk::EventId::SurfaceDestroyed, nullptr);
    if (g_renderer) {
        delete g_renderer;
        g_renderer = nullptr;
    }
    if (g_platform) {
        evkDestroyAndroidPlatform(g_platform);
        g_platform = nullptr;
    }
    EVK_LOGI("Vulkan renderer destroyed");
}
