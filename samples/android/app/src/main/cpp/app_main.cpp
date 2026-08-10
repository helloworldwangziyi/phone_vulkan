// Sample App 入口：把事件分发器挂到 core，AppStart 时建视图树。
//
// 事件处理的两种方式（参考 estarxapp 的 app 侧结构）：
//   1. global 全局监听（global.cpp，静态期自注册）：给全局变量赋值
//      （真实像素尺寸、缩放比），并转发原始触摸到 core 的点击合成 helper；
//   2. 视图回调（panel_view.cpp / button_view.cpp，建视图时自注册）：
//      事件落在视图上时触发，如 Draw 事件触发视图重绘自己。
//
// 视图树：
//   root (GROUP，铺满 surface)
//   ├── panel  (RECT, 深蓝背景)，Draw 回调里画 RGB 渐变三角形
//   └── button (RECT, 绿/橙背景)，点击切换颜色并重绘

#include "app_event.h"
#include "button_view.h"
#include "global.h"
#include "panel_view.h"

#include "evk/esx_view.h"
#include "evk/event.h"
#include "evk/log.h"

namespace {

esx_view g_root = 0;

void layoutRoot() {
    if (g_root != 0) {
        esx_view_set_bounds(g_root, 0, 0, g_screenWidth, g_screenHeight);
    }
}

void onAppStart(evk::EventId /*id*/, esx_view /*view*/, const void* /*data*/) {
    g_root = esx_create_view(ESX_VIEW_GROUP, 0, 0, 0, 0, 0);
    esx_set_root_view(g_root);
    layoutRoot();

    const esx_view panel = panelViewCreate(g_root);
    const esx_view button = buttonViewCreate(g_root);
    EVK_LOGI("view tree created: root={} panel={} button={}", g_root, panel, button);
}

// 静态初始化自注册：.so load 时即把分发器挂到 core 并注册入口监听，
// 保证 AppStart 不丢失。
struct EventAutoRegister {
    EventAutoRegister() {
        evk::setEventFunc(appEventEntry);
        appRegisterEvent(evk::EventId::AppStart, 0, onAppStart);
        appRegisterEvent(evk::EventId::SurfaceChanged, 0,
                         [](evk::EventId, esx_view, const void*) { layoutRoot(); });
    }
};
EventAutoRegister g_autoRegister;

} // namespace
