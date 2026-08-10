// iOS 实现层：EVKRenderView 与 core 之间的翻译层，逐条对应 Android 的
// platform/android/cpp/bridge.cpp（那边有完整的薄壳设计约定，不重复展开）。
//
// 生命周期与 Android 一致：渲染器/事件回调随 surface 生命周期建拆
// （evkIOSInit 建 / evkIOSDestroy 拆），App 侧经 evk::setEventFunc
// 注册的 EventFunc 收 AppStart / SurfaceChanged / Touch / UiClick / Draw。

#import "bridge.h"

#import "../EVKMetalRenderer.h"

#include "evk/esx_view.h"
#include "evk/event.h"
#include "evk/log.h"
#include "evk/render_loop.h"
#include "evk/ui/canvas.h"

// 全局 Metal 渲染器，生命周期与视图一致。
static EVKMetalRenderer* g_renderer = nil;

// 画一帧：构建视图树内容（内部逐视图发 Draw 事件）后交给渲染器。
// 同时注册为 core 的 FrameFunc，App 调 evk::requestRender() 会走到这里。
static void renderFrame() {
    static evk::ui::Canvas canvas;
    esxBuildFrame(canvas);
    [g_renderer render:canvas];
}

void evkIOSInit(CAMetalLayer* layer) {
    evk::log::init("estarx-ios");
    if (g_renderer) {
        return;
    }

    g_renderer = [[EVKMetalRenderer alloc] initWithLayer:layer];
    if (!g_renderer) {
        EVK_LOGE("failed to initialize Metal renderer");
        return;
    }

    EVK_LOGI("Metal renderer initialized");
    // 平台壳的"画一帧"实现注册给 core，App 的 requestRender() 由此触发。
    evk::setFrameFunc(&renderFrame);
    // App 的第一个事件：业务层（EventFunc）从这里开始接管。
    evk::dispatchEvent(evk::EventId::AppStart, nullptr);
}

void evkIOSResize(int32_t width, int32_t height) {
    evk::SurfaceChangedData data{ width, height };
    evk::dispatchEvent(evk::EventId::SurfaceChanged, &data);
    // Metal 渲染器每帧直接读 layer.drawableSize，无需额外通知。
}

void evkIOSRender(void) {
    renderFrame();
}

void evkIOSTouch(int32_t action, float x, float y) {
    EVK_LOGI("touch event: action={} x={:.1f} y={:.1f}", action, x, y);
    evk::TouchData data{ action, x, y };  // 栈上结构体，仅在 dispatch 期间有效
    evk::dispatchEvent(evk::EventId::Touch, &data);
    // 同时走视图树的命中测试/点击分发（内部可能再发 UiClick 事件）。
    esxDispatchTouch(action, x, y);
}

void evkIOSDestroy(void) {
    evk::dispatchEvent(evk::EventId::SurfaceDestroyed, nullptr);
    g_renderer = nil; // ARC 释放整个 Metal 栈
    EVK_LOGI("Metal renderer destroyed");
}
