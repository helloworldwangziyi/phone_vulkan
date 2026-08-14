// 跨平台 App 唯一入口。Android/iOS/Harmony 只负责启动各自平台壳，
// 页面、布局和业务回调全部链接这套公共源码。
//
// 启动顺序：平台壳完成渲染器初始化后上报 EngineReady，这里才创建视图树
// （未就绪创建视图会被 core 告警）；SurfaceChanged 只调一次 set_bounds，
// 页面内容重排由 Flex 容器自动级联完成。

#include "app_metrics.h"
#include "app_theme.h"
#include "detail_page.h"
#include "main_page.h"

#include "evk/esx_view.h"
#include "evk/event.h"
#include "evk/ui/controls/navigation.h"
#include "evk/ui/widget.h"

namespace {

esx_view g_nav = 0;

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
    evk::ui::pushPage(g_nav, std::make_unique<HomePage>(), false);
}

void appDestroyUi() {
    if (g_nav == 0) {
        return;
    }
    evk::ui::teardownAllComponents(); // 先清页面回调槽/事件监听
    esx_destroy_view(g_nav);          // 再销毁整棵视图树
    g_nav = 0;
}

void appEvent(evk::EventId id, const void* data) {
    switch (id) {
        case evk::EventId::SurfaceChanged: {
            const auto* size = static_cast<const evk::SurfaceChangedData*>(data);
            appSetScreenSize(static_cast<float>(size->width),
                             static_cast<float>(size->height));
            if (g_nav != 0) {
                esx_view_set_bounds(g_nav, 0, 0, g_screenWidth, g_screenHeight);
            }
            break;
        }
        case evk::EventId::EngineReady:
            appCreateUi();
            break;
        case evk::EventId::SurfaceDestroyed:
            appDestroyUi();
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
