#include "evk/ui/controls/button.h"

#include <memory>

#include "evk/render_loop.h"
#include "evk/ui/input.h"
#include "evk/ui/view.h"

namespace {

const char kButtonType = 0;

struct ButtonState {
    esx_button_style style{};
    esx_button_click_func onClick = nullptr;
    void* userData = nullptr;
    bool enabled = true;
    bool pressed = false;
};

std::shared_ptr<ButtonState> buttonState(esx_view button) {
    evk::ui::View* view = esxViewFromHandle(button);
    if (!view || view->controlType != &kButtonType) {
        return nullptr;
    }
    return std::static_pointer_cast<ButtonState>(view->controlState);
}

void updateButtonColor(evk::ui::View* view, const ButtonState& state) {
    const uint32_t color = !state.enabled ? state.style.disabled_color
                           : state.pressed ? state.style.pressed_color
                                           : state.style.normal_color;
    view->background = evk::ui::Color::rgba(color);
    view->hasBackground = true;
    evk::requestRender();
}

void handleButtonPointer(esx_view handle, const evk::ui::PointerEvent& event,
                         void* userData) {
    auto* state = static_cast<ButtonState*>(userData);
    evk::ui::View* view = esxViewFromHandle(handle);
    if (!state || !view || !view->visible || !state->enabled) {
        return;
    }

    switch (event.action) {
        case evk::ui::PointerAction::Down:
            state->pressed = true;
            updateButtonColor(view, *state);
            break;
        case evk::ui::PointerAction::Move: {
            const bool inside = view->containsVisiblePoint(event.x, event.y);
            if (inside != state->pressed) {
                state->pressed = inside;
                updateButtonColor(view, *state);
            }
            break;
        }
        case evk::ui::PointerAction::Up: {
            const bool clicked = state->pressed &&
                                 view->containsVisiblePoint(event.x, event.y);
            state->pressed = false;
            updateButtonColor(view, *state);
            if (clicked && state->onClick) {
                state->onClick(handle, state->userData);
            }
            break;
        }
        case evk::ui::PointerAction::Cancel:
            if (state->pressed) {
                state->pressed = false;
                updateButtonColor(view, *state);
            }
            break;
    }
}

} // namespace

extern "C" {

esx_view esx_button_create(float x, float y, float width, float height,
                           esx_view parent, const esx_button_style* style,
                           esx_button_click_func on_click, void* user_data) {
    const esx_button_style defaultStyle{0x3CB371FF, 0x2E8B57FF, 0x808080FF};
    auto state = std::make_shared<ButtonState>();
    state->style = style ? *style : defaultStyle;
    state->onClick = on_click;
    state->userData = user_data;

    const esx_view handle = esx_create_view(x, y, width, height, parent);
    evk::ui::View* view = esxViewFromHandle(handle);
    if (!view) {
        return 0;
    }
    view->controlType = &kButtonType;
    view->controlState = state;
    view->pointerHandler = handleButtonPointer;
    view->pointerUserData = state.get();
    updateButtonColor(view, *state);
    return handle;
}

void esx_button_set_enabled(esx_view button, int32_t enabled) {
    auto state = buttonState(button);
    evk::ui::View* view = esxViewFromHandle(button);
    if (!state || !view) {
        return;
    }
    const bool nextEnabled = enabled != 0;
    if (!nextEnabled && state->enabled) {
        evk::ui::cancelPointerForView(button);
        view = esxViewFromHandle(button);
        if (!view) {
            return;
        }
    }
    state->enabled = nextEnabled;
    state->pressed = false;
    updateButtonColor(view, *state);
}

void esx_button_set_on_click(esx_view button, esx_button_click_func on_click,
                             void* user_data) {
    auto state = buttonState(button);
    if (!state) {
        return;
    }
    state->onClick = on_click;
    state->userData = user_data;
}

} // extern "C"
