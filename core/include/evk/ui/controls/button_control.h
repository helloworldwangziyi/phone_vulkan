#pragma once

#include <cstdint>
#include <functional>
#include <memory>

namespace evk::ui {

class View;

struct ButtonStyle {
    uint32_t normalColor = 0x3CB371FF;
    uint32_t pressedColor = 0x2E8B57FF;
    uint32_t disabledColor = 0x808080FF;
};

std::unique_ptr<View> createButtonView(
    const ButtonStyle& style,
    std::function<void()> onPressed);

void updateButtonView(
    View& view,
    const ButtonStyle& style,
    std::function<void()> onPressed,
    bool enabled = true);

} // namespace evk::ui
