#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

namespace evk::ui {

class BuildContext;
class Element;
class View;
class Widget;

struct NavigationStyle {
    uint32_t barColor = 0x111827FF;
    uint32_t barLineColor = 0x374151FF;
    uint32_t backButtonColor = 0x111827FF;
    uint32_t backButtonPressedColor = 0x1F2937FF;
    uint32_t backArrowColor = 0xFFFFFFFF;
};

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
