/**
 * @file button_control.cpp
 * @brief ButtonView 的实现：按压状态机与配色切换。
 */

#include "evk/ui/view/button_control.h"

#include <utility>

#include "evk/frame_scheduler.h"
#include "evk/ui/pointer_input.h"
#include "evk/ui/render_view.h"
#include "evk/ui/semantics.h"

namespace evk::ui {
namespace {

/**
 * @brief 按钮对应的 View：Down 按下着色、Move 出界取消、Up 界内松手
 *        触发 onPressed、Cancel 复位。
 *
 * pressed 是 View 内部状态，widget 重建（updateButtonView）不打断——
 * 除非被禁用（updateButtonView 会兜底取消进行中的按压）。
 */
class ButtonView final : public View {
public:
    ButtonStyle style;
    std::function<void()> onPressed;
    bool enabled = true;
    bool pressed = false;

    bool acceptsPointerInput() const override { return true; }

    /// 无障碍内省：按钮角色 + tap 动作；禁用态上镜（朗读「已禁用」）。
    void fillSemantics(SemanticsConfig& config) const override {
        config.role = SemanticsRole::kButton;
        config.actions |= kSemanticsActionTap;
        config.disabled = !enabled;
    }

    /// 无障碍 tap 走与触摸 Up 相同的 onPressed 回调（禁用时不响应）。
    bool performSemanticsAction(uint32_t action) override {
        if (action == kSemanticsActionTap && enabled && onPressed) {
            onPressed();
            return true;
        }
        return View::performSemanticsAction(action);
    }

    void updateColor() {
        const uint32_t color = !enabled
            ? style.disabledColor
            : pressed ? style.pressedColor : style.normalColor;
        background = Color::rgba(color);
        hasBackground = true;
        requestRender();
    }

    void handlePointer(const PointerEvent& event) override {
        if (!visible || !enabled) {
            return;
        }
        switch (event.action) {
            case PointerAction::Down:
                pressed = true;
                updateColor();
                break;
            case PointerAction::Move: {
                const bool inside = containsVisiblePoint(event.x, event.y);
                if (inside != pressed) {
                    pressed = inside;
                    updateColor();
                }
                break;
            }
            case PointerAction::Up: {
                const bool clicked = pressed && containsVisiblePoint(event.x, event.y);
                pressed = false;
                updateColor();
                if (clicked && onPressed) {
                    const auto callback = onPressed;
                    callback();
                }
                break;
            }
            case PointerAction::Cancel:
                if (pressed) {
                    pressed = false;
                    updateColor();
                }
                break;
        }
    }
};

ButtonView* asButton(View& view) {
    return dynamic_cast<ButtonView*>(&view);
}

} // namespace

std::unique_ptr<View> createButtonView(
    const ButtonStyle& style,
    std::function<void()> onPressed) {
    auto button = std::make_unique<ButtonView>();
    button->style = style;
    button->onPressed = std::move(onPressed);
    button->updateColor();
    return button;
}

void updateButtonView(
    View& view,
    const ButtonStyle& style,
    std::function<void()> onPressed,
    bool enabled) {
    ButtonView* button = asButton(view);
    if (!button) {
        return;
    }
    if (!enabled && button->enabled) {
        cancelPointerForView(button);
    }
    button->style = style;
    button->onPressed = std::move(onPressed);
    button->enabled = enabled;
    button->pressed = false;
    button->updateColor();
}

} // namespace evk::ui
