#include "evk/ui/ui_application.h"

#include <memory>
#include <utility>

#include "evk/ui/animation_scheduler.h"
#include "evk/ui/event_bus.h"
#include "evk/ui/pointer_input.h"
#include "evk/ui/render_view.h"
#include "evk/ui/widget_tree.h"

namespace {

std::unique_ptr<evk::ui::Navigator> g_navigator;
float g_viewportWidth = 0.0f;
float g_viewportHeight = 0.0f;
float g_safeTop = 0.0f;
float g_safeBottom = 0.0f;
float g_safeLeft = 0.0f;
float g_safeRight = 0.0f;

// 根视图 = 视口扣除安全区后的矩形；视口或安全区任一变化都要重排。
void applyLayout() {
    if (!g_navigator) {
        return;
    }
    const float width = g_viewportWidth - g_safeLeft - g_safeRight;
    const float height = g_viewportHeight - g_safeTop - g_safeBottom;
    g_navigator->view().setBounds(
        g_safeLeft, g_safeTop,
        width > 0.0f ? width : 0.0f,
        height > 0.0f ? height : 0.0f);
}

} // namespace

namespace evk::ui {

void runApp(std::unique_ptr<Widget> home, AppOptions options) {
    shutdownApp();
    g_navigator = std::make_unique<Navigator>(
        options.navigationBarHeight,
        options.navigationStyle);
    setRootView(&g_navigator->view());
    applyLayout();
    g_navigator->push(std::move(home), false);
}

void shutdownApp() {
    cancelAllPointerEvents();
    stopAllAnimations();
    setRootView(nullptr);
    if (g_navigator) {
        g_navigator->clear();
        g_navigator.reset();
    }
    EventBus::instance().clear();
}

void setViewportSize(float width, float height) {
    g_viewportWidth = width;
    g_viewportHeight = height;
    applyLayout();
}

void setSafeAreaInsets(float top, float bottom, float left, float right) {
    g_safeTop = top;
    g_safeBottom = bottom;
    g_safeLeft = left;
    g_safeRight = right;
    applyLayout();
}

Navigator* appNavigator() {
    return g_navigator.get();
}

} // namespace evk::ui
