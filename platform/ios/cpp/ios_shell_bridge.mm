// ============================================================================
// iOS 桥接实现：壳层 C 调用 → core。与 Android android_jni_bridge.cpp 同构：
// 生命周期一致（evkIosInit 建 / evkIosDestroy 拆，幂等），事件经
// evk::dispatchEvent 回流给 App 注册的回调。
//
// 与 JNI 层的差别只有两点：
//   - 日志走 core 默认 stdout（Xcode 控制台可见）；Android 注入了 logcat sink。
//   - 没有 JNIEnv/全局引用问题，CAMetalLayer 由壳层持有。
// ============================================================================

#include "ios_shell_bridge.h"

#import <Foundation/Foundation.h>

#include "evk/app_lifecycle.h"
#include "evk/kv_store.h"
#include "evk/log.h"
#include "evk/frame_scheduler.h"
#include "evk/compositor.h"
// 完整类型：g_platform->getSurfaceSize 与 compositor->renderer()->setSize 要用。
#include "evk/render_platform.h"
#include "evk/vulkan_renderer.h"
#include "evk/ui/animation_scheduler.h"
#include "evk/ui/ui_application.h"
#include "evk/ui/pointer_input.h"

// ios_vulkan_platform.mm 以 C 链接导出的工厂函数。
extern "C" evk::IPlatform* evkCreateIosPlatform(const void* layer);
extern "C" void evkDestroyIosPlatform(evk::IPlatform* platform);

// 全局 Compositor（帧编排器，内部持有渲染器）与平台适配器，
// 生命周期与壳层视图一致。
static evk::Compositor* g_compositor = nullptr;
static evk::IPlatform* g_platform = nullptr;
static bool g_appStarted = false;

// 壳层：视图与 Metal 层就绪（可能多次：退后台重建）。幂等。
void evkIosInit(const void* layer) {
    if (g_compositor) {
        return;
    }
    // 引擎就绪前先注入私有存储目录：core 的 KeyValueStore 初始化需要
    // 平台路径。Documents 目录随 App 沙盒持久保存；initialize 幂等，
    // 视图重建重复调用安全。
    @autoreleasepool {
        NSArray<NSString*>* paths = NSSearchPathForDirectoriesInDomains(
            NSDocumentDirectory, NSUserDomainMask, YES);
        if (paths.count > 0) {
            evk::KeyValueStore::initialize(paths.firstObject.UTF8String);
        } else {
            EVK_LOGE("failed to locate Documents directory");
        }
    }
    g_platform = evkCreateIosPlatform(layer);
    if (!g_platform) {
        EVK_LOGE("failed to create iOS platform");
        return;
    }
    g_compositor = new evk::Compositor(g_platform);
    if (!g_compositor->initialize()) {
        // 失败时按创建的反序清理干净，保证下次能干净重试。
        EVK_LOGE("failed to initialize Vulkan renderer");
        delete g_compositor;
        g_compositor = nullptr;
        evkDestroyIosPlatform(g_platform);
        g_platform = nullptr;
        return;
    }

    EVK_LOGI("Vulkan renderer initialized (MoltenVK)");
    // 帧编排（buildFrame + 首帧日志 + render）内聚在 evk::Compositor。
    evk::setFrameFunc([](int64_t) { if (g_compositor) g_compositor->renderFrame(); });
    // 渲染器初始化完成：core 进入就绪状态，之后 App 才能安全创建视图。
    evk::setEngineReady(true);
    // EngineReady 之前先报一次 SurfaceChanged：此时 drawableSize 已是真实像素，
    // App 建视图树时即可按真实像素布局；之后的尺寸变化仍由 evkIosResize 上报。
    uint32_t surfaceWidth = 0;
    uint32_t surfaceHeight = 0;
    g_platform->getSurfaceSize(&surfaceWidth, &surfaceHeight);
    evk::SurfaceChangedData initSize{static_cast<int32_t>(surfaceWidth),
                                     static_cast<int32_t>(surfaceHeight)};
    evk::dispatchEvent(evk::EventId::SurfaceChanged, &initSize);
    if (!g_appStarted) {
        g_appStarted = true;
        evk::dispatchEvent(evk::EventId::EngineReady, nullptr);
    }
    evk::requestRender();
}

void evkIosResize(int32_t width, int32_t height) {
    evk::SurfaceChangedData data{width, height};
    evk::dispatchEvent(evk::EventId::SurfaceChanged, &data);
    if (g_compositor) {
        g_compositor->renderer()->setSize(static_cast<uint32_t>(width),
                                          static_cast<uint32_t>(height));
    }
    evk::requestRender();
}

// CADisplayLink 的 VSync 回调。core 仅在 dirty 时真正构建和提交帧。
void evkIosBeginFrame(int64_t frameTimeNanos) {
    evk::beginFrame(frameTimeNanos);
}

void evkIosTouch(int32_t action, int32_t pointerId, float x, float y,
                 int64_t eventTimeNanos) {
    evk::ui::PointerAction pointerAction;
    switch (action) {
        case 0: pointerAction = evk::ui::PointerAction::Down; break;
        case 1: pointerAction = evk::ui::PointerAction::Up; break;
        case 2: pointerAction = evk::ui::PointerAction::Move; break;
        case 3: pointerAction = evk::ui::PointerAction::Cancel; break;
        default: return;
    }
    const evk::ui::PointerEvent event{pointerAction, pointerId, x, y,
                                      eventTimeNanos};
    evk::ui::dispatchPointerEvent(event);
}

// 壳层左边缘手势识别器触发。与 Android nativeOnBackPressed 同语义：
// 消费结果仅作回报，iOS 栈底没有"退出 App"的收尾动作。
int32_t evkIosBackPressed(void) {
    return evk::dispatchEvent(evk::EventId::BackPressed, nullptr) ? 1 : 0;
}

// 壳层 viewSafeAreaInsetsDidChange 触发；单位已换算成像素（点 × scale）。
void evkIosSafeArea(float top, float bottom, float left, float right) {
    evk::SafeAreaData data{top, bottom, left, right};
    evk::dispatchEvent(evk::EventId::SafeAreaChanged, &data);
}

void evkIosDestroy(void) {
    evk::ui::cancelAllPointerEvents();
    evk::ui::stopAllAnimations();
    evk::dispatchEvent(evk::EventId::SurfaceDestroyed, nullptr);
    evk::cancelPendingFrame();
    evk::setFrameFunc(nullptr);
    evk::setEngineReady(false);
    g_appStarted = false; // 下次 evkIosInit 走完整 EngineReady 重建流程
    if (g_compositor) {
        delete g_compositor;
        g_compositor = nullptr;
    }
    if (g_platform) {
        evkDestroyIosPlatform(g_platform);
        g_platform = nullptr;
    }
    EVK_LOGI("Vulkan renderer destroyed");
}
