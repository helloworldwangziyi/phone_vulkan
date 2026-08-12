// 跨平台 App 唯一入口。Android/iOS/Harmony 只负责启动各自平台壳，
// 页面、布局和业务回调全部链接这套公共源码。

#include "app_metrics.h"
#include "app_theme.h"
#include "app_ui.h"
#include "detail_page.h"
#include "main_page.h"

#include "evk/esx_view.h"
#include "evk/event.h"
#include "evk/ui/controls/navigation.h"

namespace {

esx_view g_nav = 0;

void onNavPop(esx_view /*nav*/, esx_view page, void* /*userData*/) {
    detailPageOnPopped(page);
}

void appCreateUi() {
    if (g_nav != 0) {
        return;
    }
    const AppTheme& theme = appTheme();
    const esx_navigation_style navStyle{
        theme.navBar, theme.navBarLine, theme.surfaceRaised,
        theme.surface, theme.backArrow,
    };
    g_nav = esx_navigation_create(0, 0, g_screenWidth, g_screenHeight, 0,
                                  appCalcHeight(150), &navStyle);
    esx_set_root_view(g_nav);
    esx_navigation_set_on_pop(g_nav, onNavPop, nullptr);
    esx_navigation_push(g_nav, homePageCreate(g_nav), 0);
}

void appDestroyUi() {
    if (g_nav == 0) {
        return;
    }
    esx_destroy_view(g_nav); // 销毁整棵视图树（页面随之销毁，不走 on_pop）
    g_nav = 0;
    homePageDestroy();
    detailPagesClear();
}

void appEvent(evk::EventId id, const void* data) {
    switch (id) {
        case evk::EventId::SurfaceChanged: {
            const auto* size = static_cast<const evk::SurfaceChangedData*>(data);
            appSetScreenSize(static_cast<float>(size->width),
                             static_cast<float>(size->height));
            if (g_nav != 0) {
                esx_view_set_bounds(g_nav, 0, 0, g_screenWidth, g_screenHeight);
                homePageLayout();
                detailPagesLayout();
            }
            break;
        }
        case evk::EventId::AppStart:
            appCreateUi();
            break;
        case evk::EventId::SurfaceDestroyed:
            break;
    }
}

struct AppBootstrap {
    AppBootstrap() {
        evk::setEventFunc(appEvent);
    }
};

AppBootstrap g_appBootstrap;

} // namespace

void appRebuildUi() {
    appDestroyUi();
    appCreateUi();
}
