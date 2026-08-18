#pragma once

/**
 * @file button_control.h
 * @brief 按钮控件（View 层）：自带 pressed 按压状态机与禁用态。
 *
 * 本文件是 View 层实现（常驻节点，持有 pressed/enabled 运行时状态）；
 * Widget 层的 evk::ui::Button 只是它的配置描述（controls/button.h）。
 * create/update 走 RenderObject 协议：首次 mount 造 View，同类型重建时
 * 把新参数应用到已存在的 View 上（进行中的按压状态保留）。
 */

#include <cstdint>
#include <functional>
#include <memory>

namespace evk::ui {

class View;

/// 按钮配色（0xRRGGBBAA）：常态 / 按下 / 禁用。
struct ButtonStyle {
    uint32_t normalColor = 0x3CB371FF;
    uint32_t pressedColor = 0x2E8B57FF;
    uint32_t disabledColor = 0x808080FF;
};

/// 造一个按钮 View（仅首次 mount 时调用一次）。
std::unique_ptr<View> createButtonView(
    const ButtonStyle& style,
    std::function<void()> onPressed);

/// 同类型重建时应用新参数；enabled 变 false 会取消进行中的按压。
void updateButtonView(
    View& view,
    const ButtonStyle& style,
    std::function<void()> onPressed,
    bool enabled = true);

} // namespace evk::ui
