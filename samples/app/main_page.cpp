// 首页（导航栈根页面）：渐变面板 + "打开详情页"/"切换主题"按钮 + ScrollView 演示。
// 页面是 parent=0 创建的视图，布局与生命周期由 Navigation 接管；
// 本模块只负责页面内容：创建（homePageCreate）、旋转重排（homePageLayout）、
// 整树重建（切换主题）时清理 App 侧句柄记录（homePageDestroy）。

#include "main_page.h"

#include "app_metrics.h"
#include "app_theme.h"
#include "app_ui.h"
#include "detail_page.h"
#include "evk/esx_view.h"
#include "evk/log.h"
#include "evk/render_loop.h"
#include "evk/ui/controls/button.h"
#include "evk/ui/controls/navigation.h"
#include "evk/ui/controls/scroll_view.h"

namespace {

esx_view g_nav = 0;
esx_view g_page = 0;
esx_view g_panel = 0;
esx_view g_detailButton = 0;
esx_view g_themeButton = 0;
esx_view g_scrollView = 0;
esx_view g_scrollItems[8]{};
bool g_panelAccent = false;

void drawPanel(esx_view view, void* /*userData*/) {
    const AppTheme& theme = appTheme();
    const uint32_t left = g_panelAccent ? theme.panelAccent : theme.panelGradient[0];
    esx_draw_triangle(view,
                      appCalcWidth(440.0f), appCalcHeight(50.0f),
                      appCalcWidth(790.0f), appCalcHeight(460.0f),
                      appCalcWidth(90.0f), appCalcHeight(460.0f),
                      left, theme.panelGradient[1], theme.panelGradient[2]);
}

void onPanelClick(esx_view /*view*/, const esx_view_click_event* /*event*/,
                  void* /*userData*/) {
    g_panelAccent = !g_panelAccent;
    EVK_LOGI("panel clicked, accent={}", g_panelAccent);
    evk::requestRender();
}

void onDetailButtonClick(esx_view /*button*/, void* /*userData*/) {
    EVK_LOGI("push detail page");
    esx_navigation_push(g_nav, detailPageCreate(g_nav), 1);
}

void onThemeButtonClick(esx_view /*button*/, void* /*userData*/) {
    EVK_LOGI("toggle theme, dark={}", !appThemeIsDark());
    appThemeToggle();
    appRebuildUi();
}

void onScroll(esx_view scrollView, float offsetX, float offsetY, void* /*userData*/) {
    EVK_LOGI("scroll view {} offset=({:.1f}, {:.1f})", scrollView, offsetX, offsetY);
}

} // namespace

esx_view homePageCreate(esx_view nav) {
    if (g_page != 0) {
        return g_page;
    }
    g_nav = nav;
    const AppTheme& theme = appTheme();

    g_page = esx_create_view(0, 0, 0, 0, 0);
    esx_view_set_background(g_page, theme.windowBackground);

    g_panel = esx_create_view(0, 0, 0, 0, g_page);
    esx_view_set_background(g_panel, theme.surface);
    esx_view_set_draw_callback(g_panel, drawPanel, nullptr);
    esx_view_set_click_callback(g_panel, onPanelClick, nullptr);

    const esx_button_style primaryStyle{theme.primary, theme.primaryPressed,
                                        theme.primaryDisabled};
    g_detailButton = esx_button_create(0, 0, 0, 0, g_page, &primaryStyle,
                                       onDetailButtonClick, nullptr);

    const esx_button_style secondaryStyle{theme.secondary, theme.secondaryPressed,
                                          theme.primaryDisabled};
    g_themeButton = esx_button_create(0, 0, 0, 0, g_page, &secondaryStyle,
                                      onThemeButtonClick, nullptr);

    const float scrollWidth = appCalcWidth(880.0f);
    g_scrollView = esx_scroll_view_create(0, 0, scrollWidth, appCalcHeight(560.0f),
                                          scrollWidth, appCalcHeight(1250.0f), g_page);
    esx_scroll_view_set_on_scroll(g_scrollView, onScroll, nullptr);
    const esx_view content = esx_scroll_view_get_content(g_scrollView);
    for (int i = 0; i < 8; ++i) {
        g_scrollItems[i] = esx_create_view(0, 0, 0, 0, content);
        esx_view_set_background(g_scrollItems[i], theme.scrollItems[i]);
    }

    homePageLayout();
    EVK_LOGI("home page created: page={} panel={} scroll={}",
             g_page, g_panel, g_scrollView);
    return g_page;
}

void homePageLayout() {
    if (g_page == 0) {
        return;
    }

    const float panelWidth = appCalcWidth(880.0f);
    esx_view_set_bounds(g_panel, (g_screenWidth - panelWidth) * 0.5f,
                        appCalcHeight(60.0f), panelWidth, appCalcHeight(540.0f));

    const float buttonWidth = appCalcWidth(400.0f);
    const float buttonX = (g_screenWidth - buttonWidth) * 0.5f;
    esx_view_set_bounds(g_detailButton, buttonX, appCalcHeight(660.0f),
                        buttonWidth, appCalcHeight(140.0f));
    esx_view_set_bounds(g_themeButton, buttonX, appCalcHeight(840.0f),
                        buttonWidth, appCalcHeight(140.0f));

    const float scrollWidth = appCalcWidth(880.0f);
    esx_view_set_bounds(g_scrollView, (g_screenWidth - scrollWidth) * 0.5f,
                        appCalcHeight(1040.0f), scrollWidth, appCalcHeight(560.0f));
    esx_scroll_view_set_content_size(g_scrollView, scrollWidth, appCalcHeight(1250.0f));
    for (int i = 0; i < 8; ++i) {
        esx_view_set_bounds(g_scrollItems[i],
                            appCalcWidth(24.0f),
                            appCalcHeight(24.0f + i * 152.0f),
                            appCalcWidth(832.0f),
                            appCalcHeight(122.0f));
    }
}

void homePageDestroy() {
    g_nav = 0;
    g_page = 0;
    g_panel = 0;
    g_detailButton = 0;
    g_themeButton = 0;
    g_scrollView = 0;
    for (auto& item : g_scrollItems) {
        item = 0;
    }
    g_panelAccent = false;
}
