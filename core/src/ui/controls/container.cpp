#include "evk/ui/controls/container.h"

#include <utility>

#include "evk/ui/render_view.h"

namespace evk::ui {

Container::Container(uint32_t colorValue) : color(colorValue) {}

std::unique_ptr<View> Container::createRenderObject() const {
    auto view = std::make_unique<View>();
    updateRenderObject(*view);
    return view;
}

void Container::updateRenderObject(View& view) const {
    // 圆角/描边装饰：背景不再是整块矩形，改由 painter 按视图实际尺寸绘制；
    // 用户 painter（若给了）在装饰之后执行，两者可以叠加。
    const bool decorated =
        cornerRadius > 0.0f || (borderWidth > 0.0f && borderColor != 0);
    if (decorated) {
        view.clearBackground();
    } else if (color == 0) {
        view.clearBackground();
    } else {
        view.setBackground(color);
    }
    view.onClick = onTap
        ? [callback = onTap](const ClickEvent&) { callback(); }
        : std::function<void(const ClickEvent&)>{};
    if (decorated) {
        view.painter = [fill = color, radius = cornerRadius, border = borderColor,
                        borderWidth = borderWidth, user = painter](PaintContext& paint) {
            const Size size = paint.size();
            const Rect bounds = {0.0f, 0.0f, size.width, size.height};
            if (fill != 0) {
                paint.drawRoundRect(bounds, radius, fill);
            }
            if (borderWidth > 0.0f && border != 0) {
                paint.strokeRoundRect(bounds, radius, borderWidth, border);
            }
            if (user) {
                user(paint);
            }
        };
    } else {
        view.painter = painter;
    }
}

std::unique_ptr<Widget> container(
    uint32_t color,
    std::function<void()> onTap,
    std::function<void(PaintContext&)> painter) {
    auto result = makeWidget<Container>(color);
    auto* config = static_cast<Container*>(result.get());
    config->onTap = std::move(onTap);
    config->painter = std::move(painter);
    return result;
}

} // namespace evk::ui
