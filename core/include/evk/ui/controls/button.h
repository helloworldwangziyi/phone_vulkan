#pragma once

/**
 * @file button.h
 * @brief 按钮组件（Button）：包 Button 控件（自带 pressed 状态机与禁用态）。
 *
 * 对照 Flutter 的 material/button 系列。View 层实现见
 * controls/button_control.h（createButtonView/updateButtonView）。
 */

#include "evk/ui/controls/button_control.h"
#include "evk/ui/widget_tree.h"

namespace evk::ui {

/// 按钮组件：包 Button 控件（自带 pressed 状态机与禁用态）。
class Button final : public RenderObjectWidget {
public:
    ButtonStyle style;
    std::function<void()> onPressed;
    bool enabled = true;

    Button(ButtonStyle style, std::function<void()> onPressed);
    std::unique_ptr<View> createRenderObject() const override;
    void updateRenderObject(View& view) const override;
};

/// 构造辅助：一行造一个按钮。
std::unique_ptr<Widget> button(
    ButtonStyle style,
    std::function<void()> onPressed);

} // namespace evk::ui
