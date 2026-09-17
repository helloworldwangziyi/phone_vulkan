#include "app_fonts.h"
#include "app_images.h"
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
        EVK_LOGI("app", "mount_skipped reason=already_mounted");
        return;
    }
    EVK_LOGI("app", "mount_started ui_model=flutter_style");
    appFonts::registerFonts(); ///< 先注册字体，首个含文字的 build() 才能排版
    appImages::ensureRegistered(); ///< 程序化位图（径向渐变徽章） ///< 先注册字体，首个含文字的 build() 才能排版
    evk::ui::runApp(
        evk::ui::makeWidget<HomePage>(),
        {
            appCalcHeight(150.0f),
            navigationStyle(appTheme()),
        });
    // 预热字形缓存：各页面已知文案提前光栅化，转场动画不再撞上
    // 「逐字光栅化 + atlas 整页重传」的尖峰（首次进入行情页卡顿的根因）。
    appFonts::prewarm();
}

bool appEvent(evk::EventId id, const void* data) {
    switch (id) {
        case evk::EventId::SurfaceChanged: {
            const auto* size = static_cast<const evk::SurfaceChangedData*>(data);
            appSetScreenSize(
                static_cast<float>(size->width),
                static_cast<float>(size->height));
            evk::ui::setViewportSize(g_screenWidth, g_screenHeight);
            EVK_LOGI("layout", "viewport_updated width={:.0f} height={:.0f}",
                     g_screenWidth, g_screenHeight);
            break;
        }
        case evk::EventId::EngineReady:
            EVK_LOGI("lifecycle", "engine_ready_received");
            createUi();
            break;
        case evk::EventId::SurfaceDestroyed:
            evk::ui::shutdownApp();
            break;
        case evk::EventId::SafeAreaChanged: {
            // 平台壳上报安全区（状态栏/刘海/手势条）：根视图整体内缩避障。
            const auto* insets = static_cast<const evk::SafeAreaData*>(data);
            evk::ui::setSafeAreaInsets(
                insets->top, insets->bottom, insets->left, insets->right);
            EVK_LOGI("layout",
                     "safe_area_updated top={:.0f} bottom={:.0f} left={:.0f} right={:.0f}",
                     insets->top, insets->bottom, insets->left, insets->right);
            break;
        }
        case evk::EventId::BackPressed: {
            // 平台壳上报的系统返回：导航栈能 pop 就消费；
            // 已在栈底返回 false，平台壳自行收尾（Android finish Activity）。
            evk::ui::Navigator* navigator = evk::ui::appNavigator();
            if (navigator && navigator->depth() > 1) {
                navigator->pop(true);
                return true;
            }
            return false;
        }
    }
    return true;
}

struct AppBootstrap {
    AppBootstrap() {
        evk::setEventFunc(appEvent);
    }
};

AppBootstrap g_appBootstrap;

} // namespace
