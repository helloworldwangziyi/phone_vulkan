#include "button_view.h"

#include "app_event.h"
#include "global.h"
#include "evk/log.h"
#include "evk/render_loop.h"

namespace {

esx_view g_button = 0;

bool g_buttonOrange = false;
constexpr uint32_t kButtonGreen = 0x3CB371FF;
constexpr uint32_t kButtonOrange = 0xE07020FF;

// 设计稿布局（1080x2339）：400x160，顶部 1000，水平居中。
void layoutButton() {
    if (g_button == 0) {
        return;
    }
    const float w = appCalcWidth(400.0f);
    const float h = appCalcHeight(160.0f);
    esx_view_set_bounds(g_button, (g_screenWidth - w) * 0.5f, appCalcHeight(1000.0f), w, h);
}

// 视图回调：UiClick 落在 button 上时切换背景色并请求重绘。
void onButtonClick(evk::EventId /*id*/, esx_view view, const void* data) {
    const auto* d = static_cast<const evk::UiClickData*>(data);
    g_buttonOrange = !g_buttonOrange;
    const uint32_t color = g_buttonOrange ? kButtonOrange : kButtonGreen;
    esx_view_set_background(view, color);
    EVK_LOGI("UiClick on button at ({:.1f},{:.1f}), background -> #{:08X}",
             d->x, d->y, color);
    evk::requestRender();
}

} // namespace

esx_view buttonViewCreate(esx_view parent) {
    g_button = esx_create_view(ESX_VIEW_RECT, 0, 0, 0, 0, parent);
    esx_view_set_background(g_button, kButtonGreen); // 绿色
    layoutButton();
    appRegisterEvent(evk::EventId::UiClick, g_button, onButtonClick);
    appRegisterEvent(evk::EventId::SurfaceChanged, 0,
                     [](evk::EventId, esx_view, const void*) { layoutButton(); });
    return g_button;
}
