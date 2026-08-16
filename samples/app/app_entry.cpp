#include "app_fonts.h"
#include "screen_metrics.h"
#include "app_theme.h"
#include "home_page.h"

#include "evk/app_lifecycle.h"
#include "evk/log.h"
#include "evk/ui/ui_application.h"
#include "evk/ui/widget_tree.h"

namespace {

evk::ui::NavigationStyle navigationStyle(const AppTheme& theme) {
    return {
        theme.navBar,
        theme.navBarLine,
        theme.surfaceRaised,
        theme.surface,
        theme.backArrow,
    };
}

void createUi() {
    if (evk::ui::appNavigator()) {
        EVK_LOGI("UI already mounted; duplicate EngineReady ignored");
        return;
    }
    EVK_LOGI("mounting Flutter-style UI root");
    appFonts::registerFonts(); ///< 先注册字体，首个含文字的 build() 才能排版
    evk::ui::runApp(
        evk::ui::makeWidget<HomePage>(),
        {
            appCalcHeight(150.0f),
            navigationStyle(appTheme()),
        });
}

void appEvent(evk::EventId id, const void* data) {
    switch (id) {
        case evk::EventId::SurfaceChanged: {
            const auto* size = static_cast<const evk::SurfaceChangedData*>(data);
            appSetScreenSize(
                static_cast<float>(size->width),
                static_cast<float>(size->height));
            evk::ui::setViewportSize(g_screenWidth, g_screenHeight);
            EVK_LOGI("UI viewport set to {:.0f}x{:.0f}",
                     g_screenWidth, g_screenHeight);
            break;
        }
        case evk::EventId::EngineReady:
            EVK_LOGI("EngineReady received by App");
            createUi();
            break;
        case evk::EventId::SurfaceDestroyed:
            evk::ui::shutdownApp();
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
