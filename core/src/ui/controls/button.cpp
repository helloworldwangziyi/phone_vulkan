#include "evk/ui/controls/button.h"

#include <memory>
#include <utility>

#include "evk/ui/semantics.h"

namespace evk::ui {

Button::Button(ButtonStyle value, std::function<void()> callback)
    : style(value), onPressed(std::move(callback)) {}

std::unique_ptr<View> Button::createRenderObject() const {
    std::unique_ptr<View> view = createButtonView(style, onPressed);
    updateRenderObject(*view);  // 首挂也要写语义标注（update 路径才走得到）
    return view;
}

void Button::updateRenderObject(View& view) const {
    updateButtonView(view, style, onPressed, enabled);
    if (!semanticsLabel.empty()) {
        if (!view.semantics) {
            view.semantics = std::make_unique<SemanticsConfig>();
        }
        view.semantics->label = semanticsLabel;
    }
}

std::unique_ptr<Widget> button(
    ButtonStyle style,
    std::function<void()> onPressed) {
    return makeWidget<Button>(style, std::move(onPressed));
}

} // namespace evk::ui
