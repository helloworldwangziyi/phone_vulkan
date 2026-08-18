#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

namespace evk::ui {

class BuildContext;
class Element;
class View;
class Widget;

/// 导航栏配色（RGBA 字节序与 Color::rgba 一致）。
struct NavigationStyle {
    uint32_t barColor = 0x111827FF;
    uint32_t barLineColor = 0x374151FF;
    uint32_t backButtonColor = 0x111827FF;
    uint32_t backButtonPressedColor = 0x1F2937FF;
    uint32_t backArrowColor = 0xFFFFFFFF;
};

/**
 * @brief 页面导航栈：push/pop 转场、路由生命周期下发。
 *
 * 页面以 Widget 形式推入，Navigator 负责 mount、转场动画与 RouteEvent
 * 四段旅程（WillEnter/DidEnter/WillLeave/DidLeave，见 widget_tree.h）：
 * Will* 返回 false 可否决本次导航，每个 Will 严格配对一次 Did。
 * 返回入口有两个：导航栏返回键，以及平台壳上报的 EventId::BackPressed
 * （Android 返回键/手势、iOS 边缘手势，由 App 层转成 pop）。
 * 状态机与实现细节见 navigation_stack.cpp 的文件头注释。
 */
class Navigator {
public:
    Navigator(float navigationBarHeight, NavigationStyle style = {});
    ~Navigator();

    Navigator(const Navigator&) = delete;
    Navigator& operator=(const Navigator&) = delete;

    static Navigator& of(BuildContext& context);

    bool push(std::unique_ptr<Widget> page, bool animated = true);
    bool pop(bool animated = true);

    size_t depth() const;
    View* topView() const;
    View& view() const;
    void setStyle(const NavigationStyle& style);
    void setBounds(float width, float height);
    void clear();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace evk::ui
