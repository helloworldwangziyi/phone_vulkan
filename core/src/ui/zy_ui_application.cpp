#include "evk/ui/zy_ui_application.h"

#include <memory>
#include <utility>

#include "evk/ui/zy_animation_scheduler.h"
#include "evk/ui/zy_event_bus.h"
#include "evk/ui/zy_pointer_input.h"
#include "evk/ui/zy_render_view.h"
#include "evk/ui/zy_widget_tree.h"

namespace {

std::unique_ptr<evk::ui::Navigator> g_navigator;
float g_viewportWidth = 0.0f;
float g_viewportHeight = 0.0f;

} // namespace

namespace evk::ui {

void runApp(std::unique_ptr<Widget> home, AppOptions options) {
    shutdownApp();
    g_navigator = std::make_unique<Navigator>(
        options.navigationBarHeight,
        options.navigationStyle);
    setRootView(&g_navigator->view());
    g_navigator->setBounds(g_viewportWidth, g_viewportHeight);
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
    if (g_navigator) {
        g_navigator->setBounds(width, height);
    }
}

Navigator* appNavigator() {
    return g_navigator.get();
}

} // namespace evk::ui
