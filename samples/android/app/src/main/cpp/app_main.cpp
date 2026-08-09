// Sample App 层：演示 esx 视图 ABI 的用法。
//
// 视图树：
//   root (GROUP，铺满 surface)
//   ├── panel  (RECT, 深蓝背景, 100,200 880x600)，Draw 回调里画 RGB 渐变三角形
//   └── button (RECT, 绿/橙背景, 340,1000 400x160)，点击切换颜色并重绘
//
// 事件回调在 .so 加载时即注册（静态初始化），保证 AppStart 不丢失。

#include "evk/esx_view.h"
#include "evk/event.h"
#include "evk/log.h"
#include "evk/render_loop.h"

namespace {

esx_view g_root = 0;
esx_view g_panel = 0;
esx_view g_button = 0;

bool g_buttonOrange = false;
constexpr uint32_t kButtonGreen = 0x3CB371FF;
constexpr uint32_t kButtonOrange = 0xE07020FF;

void appEvent(evk::EventId id, const void* data) {
    switch (id) {
        case evk::EventId::AppStart: {
            g_root = esx_create_view(ESX_VIEW_GROUP, 0, 0, 0, 0, 0);
            esx_set_root_view(g_root);

            g_panel = esx_create_view(ESX_VIEW_RECT, 100, 200, 880, 600, g_root);
            esx_view_set_background(g_panel, 0x1B2A4AFF); // 深蓝不透明

            g_button = esx_create_view(ESX_VIEW_RECT, 340, 1000, 400, 160, g_root);
            esx_view_set_background(g_button, kButtonGreen); // 绿色
            EVK_LOGI("view tree created: root={} panel={} button={}", g_root, g_panel, g_button);
            break;
        }
        case evk::EventId::SurfaceChanged: {
            const auto* d = static_cast<const evk::SurfaceChangedData*>(data);
            esx_view_set_bounds(g_root, 0, 0,
                                static_cast<float>(d->width), static_cast<float>(d->height));
            break;
        }
        case evk::EventId::Draw: {
            const auto* d = static_cast<const evk::DrawData*>(data);
            if (d->view == g_panel) {
                // 红绿蓝渐变三角形，坐标相对 panel 左上角。
                esx_draw_triangle(g_panel,
                                  440, 60, 790, 500, 90, 500,
                                  0xFF0000FF, 0x00FF00FF, 0x0000FFFF);
            }
            break;
        }
        case evk::EventId::UiClick: {
            const auto* d = static_cast<const evk::UiClickData*>(data);
            if (d->view == g_button) {
                g_buttonOrange = !g_buttonOrange;
                uint32_t color = g_buttonOrange ? kButtonOrange : kButtonGreen;
                esx_view_set_background(g_button, color);
                EVK_LOGI("UiClick on button at ({:.1f},{:.1f}), background -> #{:08X}",
                         d->x, d->y, color);
                evk::requestRender();
            }
            break;
        }
        default:
            break;
    }
}

// 静态初始化自注册：.so load 时即注册事件回调，保证 AppStart 不丢失。
struct EventAutoRegister {
    EventAutoRegister() {
        evk::setEventFunc(appEvent);
    }
};
EventAutoRegister g_autoRegister;

} // namespace
