#pragma once

#include <memory>

#include "evk/ui/navigation/zy_navigation_stack.h"

namespace evk::ui {

class Widget;

struct AppOptions {
    float navigationBarHeight = 0.0f;
    NavigationStyle navigationStyle;
};

void runApp(std::unique_ptr<Widget> home, AppOptions options = {});
void shutdownApp();
void setViewportSize(float width, float height);
Navigator* appNavigator();

} // namespace evk::ui
