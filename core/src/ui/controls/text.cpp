#include "evk/ui/controls/text.h"

#include <utility>

#include "evk/ui/render_view.h"

namespace evk::ui {

TextWidget::TextWidget(std::string content, float fontSize, uint32_t color,
                       FontId font)
    : content_(std::move(content)), fontSize_(fontSize), color_(color), font_(font) {}

std::unique_ptr<View> TextWidget::createRenderObject() const {
    auto view = std::make_unique<View>();
    updateRenderObject(*view);
    return view;
}

void TextWidget::updateRenderObject(View& view) const {
    // 文本视图无背景无手势，只有一个 painter：内容/字号/颜色变了就整体换闭包。
    view.clearBackground();
    view.painter = [content = content_, font = font_, size = fontSize_,
                    color = color_](PaintContext& paint) {
        paint.drawText(content.c_str(), font, 0.0f, 0.0f, size, color);
    };
}

FlexParentData TextWidget::flexParentData(Axis axis) const {
    // 主轴尺寸 = 测量值（纵向容器报行高、横向容器报行宽）；
    // 交叉轴不约束，沿用容器的对齐/拉伸规则。
    FlexParentData data;
    float width = 0.0f;
    float height = 0.0f;
    FontEngine::instance().measureText(content_.c_str(), fontSize_, font_,
                                       &width, &height);
    data.mainSize = axis == Axis::Vertical ? height : width;
    return data;
}

std::unique_ptr<Widget> text(std::string content, float fontSize, uint32_t color,
                             FontId font) {
    return makeWidget<TextWidget>(std::move(content), fontSize, color, font);
}

} // namespace evk::ui
