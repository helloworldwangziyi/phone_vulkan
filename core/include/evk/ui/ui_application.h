#pragma once

#include <memory>

#include "evk/ui/navigation/navigation_stack.h"

namespace evk::ui {

class Widget;

struct AppOptions {
    float navigationBarHeight = 0.0f;
    NavigationStyle navigationStyle;
};

void runApp(std::unique_ptr<Widget> home, AppOptions options = {});
void shutdownApp();
void setViewportSize(float width, float height);

/**
 * @brief 设置安全区内边距（真实像素），根视图据此整体内缩，
 *        避开状态栏/刘海/手势条。由平台壳的 SafeAreaChanged 事件驱动；
 *        与 setViewportSize 的先后顺序无关，两者都会触发重排。
 */
void setSafeAreaInsets(float top, float bottom, float left, float right);

Navigator* appNavigator();

} // namespace evk::ui
