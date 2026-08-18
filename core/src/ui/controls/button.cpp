#include "evk/ui/controls/button.h"

#include <utility>

namespace evk::ui {

Button::Button(ButtonStyle value, std::function<void()> callback)
    : style(value), onPressed(std::move(callback)) {}

std::unique_ptr<View> Button::createRenderObject() const {
    return createButtonView(style, onPressed);
}

void Button::updateRenderObject(View& view) const {
    updateButtonView(view, style, onPressed, enabled);
}

std::unique_ptr<Widget> button(
    ButtonStyle style,
    std::function<void()> onPressed) {
    return makeWidget<Button>(style, std::move(onPressed));
}

} // namespace evk::ui
