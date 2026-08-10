#include "panel_view.h"

#include "app_event.h"
#include "global.h"

namespace {

esx_view g_panel = 0;

// 设计稿布局（1080x2339）：880x600，顶部 200，水平居中。
// x 取实时屏宽居中而不是设计坐标 100，保证任意宽高比下都居中。
void layoutPanel() {
    if (g_panel == 0) {
        return;
    }
    const float w = appCalcWidth(880.0f);
    const float h = appCalcHeight(600.0f);
    esx_view_set_bounds(g_panel, (g_screenWidth - w) * 0.5f, appCalcHeight(200.0f), w, h);
}

// 视图回调：Draw 事件落在 panel 上时重绘自己。
// 坐标为相对 panel 左上角的局部坐标（设计像素，随视图同比缩放）。
void onPanelDraw(evk::EventId /*id*/, esx_view view, const void* /*data*/) {
    esx_draw_triangle(view,
                      appCalcWidth(440.0f), appCalcHeight(60.0f),
                      appCalcWidth(790.0f), appCalcHeight(500.0f),
                      appCalcWidth(90.0f),  appCalcHeight(500.0f),
                      0xFF0000FF, 0x00FF00FF, 0x0000FFFF);
}

} // namespace

esx_view panelViewCreate(esx_view parent) {
    g_panel = esx_create_view(ESX_VIEW_RECT, 0, 0, 0, 0, parent);
    esx_view_set_background(g_panel, 0x1B2A4AFF); // 深蓝不透明
    layoutPanel();
    appRegisterEvent(evk::EventId::Draw, g_panel, onPanelDraw);
    appRegisterEvent(evk::EventId::SurfaceChanged, 0,
                     [](evk::EventId, esx_view, const void*) { layoutPanel(); });
    return g_panel;
}
