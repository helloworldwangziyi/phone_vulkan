/**
 * @file button.cpp
 * @brief Button：带点击处理的 View。
 *
 * 继承 View 并重写输入钩子：
 * - acceptsPointerInput → true，声明自己想成为原始 Pointer 事件目标；
 * - handlePointer       → 按下/移出/抬起的状态机，Up 且仍在按钮内时回调 onClick。
 *
 * 状态（按下、禁用）变化直接改写自身 background 并请求重绘。
 */

#include "evk/ui/controls/button.h"

#include <memory>

#include "evk/render_loop.h"
#include "evk/ui/input.h"
#include "evk/ui/view.h"

namespace {

/**
 * @brief Button 控件实现（继承 View，自带点击判定状态机）。
 */
class Button : public evk::ui::View {
public:
    esx_button_style style{}; ///< 三种状态颜色（normal/pressed/disabled）
    esx_button_click_func onClick = nullptr; ///< 语义化点击（按下+抬起都在按钮内才触发）
    void* userData = nullptr; ///< onClick 的 user_data
    bool enabled = true; ///< false 时不响应输入并显示 disabled 色
    bool pressed = false; ///< 按下态（决定背景色；Up 时参与点击判定）

    /**
     * @brief 声明"想成为原始 Pointer 事件目标"：输入层 Down 时沿父链认领最近目标，
     * 因此点在按钮上时 Down/Move/Up 全套原始事件都归 Button，
     * 点击由下面的状态机自己判定（不经过输入层的点击合成路径——
     * 输入层 Up 分支里的 !acceptsPointerInput() 条件即为此留的通道）。
     */
    bool acceptsPointerInput() const override { return true; }

    /// 按 enabled/pressed 选择当前背景色并请求重绘。
    void updateColor() {
        const uint32_t color = !enabled ? style.disabled_color
                               : pressed ? style.pressed_color
                                         : style.normal_color;
        background = evk::ui::Color::rgba(color);
        hasBackground = true;
        evk::requestRender();
    }

    /**
     * @brief 原始指针状态机（点击由控件自己判定，不依赖输入层的点击合成）。
     *
     * - Down   → 进入 pressed（换 pressed 色并请求重绘）；
     * - Move   → 手指移出按钮即退出 pressed（视觉跟随，松手不触发点击），
     *   移回则恢复——containsVisiblePoint 要求整条父链都可见，
     *   所以被上层盖住/按钮隐藏时也会自动退出按下态；
     * - Up     → 只有"抬起时仍 pressed 且手指还在按钮内"才算点击
     *   （= 按下后没滑出去），回调 onClick（→ trampoline → onTap）；
     * - Cancel → 复位 pressed 不回调（手势被判给滑动/视图销毁/禁用等）。
     *
     * 输入层的 12px 触控阈值在控件上游：超阈值时 Button 会收到 Cancel，
     * 点击与滑动因此互斥。
     */
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
                // 手指滑出/滑回按钮时切换按下态；inside 与 pressed 相等则无变化，
                // 避免每次 Move 都重绘。
                const bool inside = containsVisiblePoint(event.x, event.y);
                if (inside != pressed) {
                    pressed = inside;
                    updateColor();
                }
                break;
            }
            case evk::ui::PointerAction::Up: {
                // 点击成立条件：抬起时仍处于按下态 + 手指仍在按钮可见区域内。
                const bool clicked = pressed && containsVisiblePoint(event.x, event.y);
                pressed = false;
                updateColor();
                if (clicked && onClick) {
                    onClick(handle, userData);
                }
                break;
            }
            case evk::ui::PointerAction::Cancel:
                // 手势被取消（滑动判定/多指/视图隐藏销毁/禁用）：只复位不回调。
                if (pressed) {
                    pressed = false;
                    updateColor();
                }
                break;
        }
    }
};

/// 句柄 → Button；句柄无效或视图不是 Button 时返回 nullptr。
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

void esx_button_set_style(esx_view button, const esx_button_style* style) {
    Button* self = buttonFromHandle(button);
    if (!self || !style) {
        return;
    }
    self->style = *style;
    self->updateColor();
}

} // extern "C"
