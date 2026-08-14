// ============================================================================
// iOS 桥接实现：壳层 C 调用 → core。与 Android bridge.cpp 同构：
// 生命周期一致（evkIosInit 建 / evkIosDestroy 拆，幂等），事件经
// evk::dispatchEvent 回流给 App 注册的回调。
//
// 与 JNI 层的差别只有两点：
//   - 日志走 core 默认 stdout（Xcode 控制台可见）；Android 注入了 logcat sink。
//   - 没有 JNIEnv/全局引用问题，CAMetalLayer 由壳层持有。
// ============================================================================

#include "bridge_ios.h"

#include "evk/esx_view.h"
#include "evk/event.h"
#include "evk/log.h"
#include "evk/render_loop.h"
#include "evk/renderer.h"
#include "evk/ui/animator.h"
#include "evk/ui/canvas.h"
#include "evk/ui/input.h"

// platform_ios.mm 以 C 链接导出的工厂函数。
extern "C" evk::IPlatform* evkCreateIosPlatform(const void* layer);
extern "C" void evkDestroyIosPlatform(evk::IPlatform* platform);

// 全局渲染器与平台适配器，生命周期与壳层视图一致。
static evk::Renderer* g_renderer = nullptr;
static evk::IPlatform* g_platform = nullptr;
static bool g_appStarted = false;

// 画一帧：构建视图树内容（内部执行 View draw callback）后交给渲染器。
// 同时注册为 core 的 FrameFunc，App 调 evk::requestRender() 会走到这里。
static void renderFrame(int64_t /*frameTimeNanos*/) {
    static evk::ui::Canvas canvas;
    esxBuildFrame(canvas);
    if (g_renderer) {
        g_renderer->render(canvas);
    }
}

// 壳层：视图与 Metal 层就绪（可能多次：退后台重建）。幂等。
void evkIosInit(const void* layer) {
    if (g_renderer) {
        return;
    }
    g_platform = evkCreateIosPlatform(layer);
    if (!g_platform) {
        EVK_LOGE("failed to create iOS platform");
        return;
    }
    g_renderer = new evk::Renderer(g_platform);
    if (!g_renderer->initialize()) {
        // 失败时按创建的反序清理干净，保证下次能干净重试。
        EVK_LOGE("failed to initialize Vulkan renderer");
        delete g_renderer;
        g_renderer = nullptr;
        evkDestroyIosPlatform(g_platform);
        g_platform = nullptr;
        return;
    }

    EVK_LOGI("Vulkan renderer initialized (MoltenVK)");
    evk::setFrameFunc(&renderFrame);
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
    if (g_renderer) {
        g_renderer->setSize(static_cast<uint32_t>(width),
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

void evkIosDestroy(void) {
    evk::ui::cancelAllPointerEvents();
    evk::ui::stopAllAnimations();
    evk::dispatchEvent(evk::EventId::SurfaceDestroyed, nullptr);
    evk::cancelPendingFrame();
    evk::setFrameFunc(nullptr);
    evk::setEngineReady(false);
    g_appStarted = false; // 下次 evkIosInit 走完整 EngineReady 重建流程
    if (g_renderer) {
        delete g_renderer;
        g_renderer = nullptr;
    }
    if (g_platform) {
        evkDestroyIosPlatform(g_platform);
        g_platform = nullptr;
    }
    EVK_LOGI("Vulkan renderer destroyed");
}
