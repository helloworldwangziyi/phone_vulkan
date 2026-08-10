#include "global.h"

#include "app_event.h"
#include "evk/esx_view.h"
#include "evk/log.h"

float g_screenWidth = 0.0f;
float g_screenHeight = 0.0f;

namespace {

// 设计像素 → 真实像素的缩放比（对应 estarxapp 的 g_window_width_ratio）。
float g_scaleRatio = 0.0f;

// 全局监听：SurfaceChanged → 保存真实像素尺寸并重算缩放比。
// 注册于静态期，早于 AppStart 中各视图的自注册，
// 因此旋转等尺寸变化时，这里永远先于视图的重新布局执行。
void onSurfaceChanged(evk::EventId /*id*/, esx_view /*view*/, const void* data) {
    const auto* d = static_cast<const evk::SurfaceChangedData*>(data);
    g_screenWidth = static_cast<float>(d->width);
    g_screenHeight = static_cast<float>(d->height);
    const float widthRatio = g_screenWidth / kDesignWidth;
    const float heightRatio = g_screenHeight / kDesignHeight;
    g_scaleRatio = widthRatio < heightRatio ? widthRatio : heightRatio;
    EVK_LOGI("screen size: {:.0f}x{:.0f}, design scale ratio={:.4f}",
             g_screenWidth, g_screenHeight, g_scaleRatio);
}

// 全局监听：原始触摸透传给 core 的点击合成 helper（命中后由 core 发 UiClick，
// 再走各视图的点击回调）。action 原样透传 Android MotionEvent 常量。
void onTouch(evk::EventId /*id*/, esx_view /*view*/, const void* data) {
    const auto* d = static_cast<const evk::TouchData*>(data);
    EVK_LOGI("raw touch action={} at ({:.1f},{:.1f})", d->action, d->x, d->y);
    esxDispatchTouch(d->action, d->x, d->y);
}

// 静态自注册：.so 加载时即挂上全局监听，保证 init 的首次 SurfaceChanged 不丢。
struct GlobalEventAutoRegister {
    GlobalEventAutoRegister() {
        appRegisterEvent(evk::EventId::SurfaceChanged, 0, onSurfaceChanged);
        appRegisterEvent(evk::EventId::Touch, 0, onTouch);
    }
};
GlobalEventAutoRegister g_globalEventAutoRegister;

} // namespace

float appCalcWidth(float size) {
    return g_scaleRatio * size;
}

float appCalcHeight(float size) {
    return g_scaleRatio * size;
}
