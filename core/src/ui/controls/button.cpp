// ============================================================================
// Button：带点击处理的 View。
//
// 继承 View 并重写输入钩子：
//   acceptsPointerInput → true，声明自己想成为原始 Pointer 事件目标；
//   handlePointer       → 按下/移出/抬起的状态机，Up 且仍在按钮内时回调 onClick。
//
// 状态（按下、禁用）变化直接改写自身 background 并请求重绘。
// ============================================================================

#include "evk/ui/controls/button.h"

#include <memory>

#include "evk/render_loop.h"
#include "evk/ui/input.h"
#include "evk/ui/view.h"

namespace {

class Button : public evk::ui::View {
public:
    esx_button_style style{};
    esx_button_click_func onClick = nullptr;
    void* userData = nullptr;
    bool enabled = true;
    bool pressed = false;

    bool acceptsPointerInput() const override { return true; }

    // 按 enabled/pressed 选择当前背景色并请求重绘。
    void updateColor() {
        const uint32_t color = !enabled ? style.disabled_color
                               : pressed ? style.pressed_color
                                         : style.normal_color;
        background = evk::ui::Color::rgba(color);
        hasBackground = true;
        evk::requestRender();
    }

    void handlePointer(const evk::ui::PointerEvent& event) override {
        if (!visible || !enabled) {
            return;
        }

        switch (event.action) {
            case evk::ui::PointerAction::Down:
                pressed = true;
                updateColor();
                break;
            case evk::ui::PointerAction::Move: {
                const bool inside = containsVisiblePoint(event.x, event.y);
                if (inside != pressed) {
                    pressed = inside;
                    updateColor();
                }
                break;
            }
            case evk::ui::PointerAction::Up: {
                const bool clicked = pressed && containsVisiblePoint(event.x, event.y);
                pressed = false;
                updateColor();
                if (clicked && onClick) {
                    onClick(handle, userData);
                }
                break;
            }
            case evk::ui::PointerAction::Cancel:
                if (pressed) {
                    pressed = false;
                    updateColor();
                }
                break;
        }
    }
};

// 句柄 → Button；句柄无效或视图不是 Button 时返回 nullptr。
Button* buttonFromHandle(esx_view button) {
    return dynamic_cast<Button*>(esxViewFromHandle(button));
}

} // namespace

extern "C" {

esx_view esx_button_create(float x, float y, float width, float height,
                           esx_view parent, const esx_button_style* style,
                           esx_button_click_func on_click, void* user_data) {
    const esx_button_style defaultStyle{0x3CB371FF, 0x2E8B57FF, 0x808080FF};
    auto button = std::make_unique<Button>();
    button->style = style ? *style : defaultStyle;
    button->onClick = on_click;
    button->userData = user_data;

    Button* raw = button.get();
    const esx_view handle = esxAdoptViewNode(std::move(button), x, y, width, height, parent);
    if (handle == 0) {
        return 0;
    }
    raw->updateColor();
    return handle;
}

void esx_button_set_enabled(esx_view button, int32_t enabled) {
    Button* self = buttonFromHandle(button);
    if (!self) {
        return;
    }
    const bool nextEnabled = enabled != 0;
    if (!nextEnabled && self->enabled) {
        // 禁用先取消进行中的手势（Cancel 回调会让 pressed 复位），
        // 回调可能间接销毁视图，之后重新解析句柄。
        evk::ui::cancelPointerForView(button);
        self = buttonFromHandle(button);
        if (!self) {
            return;
        }
    }
    self->enabled = nextEnabled;
    self->pressed = false;
    self->updateColor();
}

void esx_button_set_on_click(esx_view button, esx_button_click_func on_click,
                             void* user_data) {
    Button* self = buttonFromHandle(button);
    if (!self) {
        return;
    }
    self->onClick = on_click;
    self->userData = user_data;
}

} // extern "C"
