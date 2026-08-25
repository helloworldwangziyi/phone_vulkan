#include "evk/ui/controls/text.h"

#include <utility>

#include "evk/ui/render_view.h"
#include "evk/ui/text_layout.h"

namespace evk::ui {

namespace {

/**
 * @brief 换行模式 Text 的 View：宽度由父布局写入（交叉轴），高度按行数
 *        自己撑出来。
 *
 * 父布局（FlexView）只负责写 bounds；本视图在 handleBoundsChanged 里按
 * 实际宽度重排，若内容高度与父布局分给的高度不一致，就把正确高度经
 * setFlexChild 回灌给父容器重排——沿用既有的排布参数通道，不是新协议。
 * 父容器重排后本视图以相同宽度再进一次本函数，此时高度已吻合，递归
 * 终止。排版结果在 TextLayout 里按键缓存，重复进入不重排。
 *
 * 限制：换行 Text 要直接放在 Column 里（或显式尺寸/滚动内容里）；不要
 * 套 Expanded/Center/Padding——转接件改写的排布参数会被回灌覆盖。
 */
class WrappedTextView final : public View {
public:
    std::string content;  ///< 文本内容（UTF-8）
    float sizePx = 0.0f;  ///< 字号
    uint32_t color = 0;   ///< 文字颜色（RGBA）
    FontId font = kFontAny; ///< 首选字体
    TextLayout layout;    ///< 排版缓存（键：文本/字号/字体/宽度）

    void handleBoundsChanged() override {
        if (rect.w <= 0.0f || !parent) {
            return;
        }
        layout.layout(content.c_str(), sizePx, font, rect.w);
        const float need = layout.totalHeight();
        if (need == rect.h) {
            return; // 父布局分给的高度已吻合内容高度。
        }
        // 回灌正确高度 → 父容器立即重排 → 本视图以 (同宽, need) 再进一次
        // 本函数，高度吻合后返回，递归到此为止。
        FlexParentData data;
        data.mainSize = need;
        setFlexChild(*parent, *this, data);
    }
};

} // namespace

TextWidget::TextWidget(std::string content, float fontSize, uint32_t color,
                       FontId font, bool softWrap)
    : content_(std::move(content)),
      fontSize_(fontSize),
      color_(color),
      font_(font),
      softWrap_(softWrap) {}

std::unique_ptr<View> TextWidget::createRenderObject() const {
    if (softWrap_) {
        auto view = std::make_unique<WrappedTextView>();
        updateRenderObject(*view);
        return view;
    }
    auto view = std::make_unique<View>();
    updateRenderObject(*view);
    return view;
}

void TextWidget::updateRenderObject(View& view) const {
    // 文本视图无背景无手势，只有一个 painter：内容/字号/颜色变了就整体换闭包。
    view.clearBackground();
    if (softWrap_) {
        auto* self = static_cast<WrappedTextView*>(&view);
        self->content = content_;
        self->sizePx = fontSize_;
        self->color = color_;
        self->font = font_;
        view.painter = [self](PaintContext& paint) {
            // 宽度以视图实际宽度为准（父布局写入）；相同输入命中排版缓存。
            self->layout.layout(self->content.c_str(), self->sizePx, self->font,
                                paint.size().width);
            self->layout.paint(paint, 0.0f, 0.0f, self->color);
        };
        return;
    }
    view.painter = [content = content_, font = font_, size = fontSize_,
                    color = color_](PaintContext& paint) {
        paint.drawText(content.c_str(), font, 0.0f, 0.0f, size, color);
    };
}

bool TextWidget::canUpdate(const Widget& other) const {
    const auto* text = dynamic_cast<const TextWidget*>(&other);
    return text && text->softWrap_ == softWrap_;
}

FlexParentData TextWidget::flexParentData(Axis axis) const {
    // 主轴尺寸 = 测量值（纵向容器报行高、横向容器报行宽）；
    // 交叉轴不约束，沿用容器的对齐/拉伸规则。
    FlexParentData data;
    if (softWrap_ && axis == Axis::Vertical) {
        // 换行模式在 build 期拿不到视图宽度：先按"不软换行"（仅 \n 断行）
        // 估一个高度上报；视图拿到真实宽度后会把正确高度回灌给父容器。
        TextLayout scratch;
        scratch.layout(content_.c_str(), fontSize_, font_, 0.0f);
        data.mainSize = scratch.totalHeight();
        return data;
    }
    float width = 0.0f;
    float height = 0.0f;
    FontEngine::instance().measureText(content_.c_str(), fontSize_, font_,
                                       &width, &height);
    data.mainSize = axis == Axis::Vertical ? height : width;
    return data;
}

std::unique_ptr<Widget> text(std::string content, float fontSize, uint32_t color,
                             FontId font, bool softWrap) {
    return makeWidget<TextWidget>(std::move(content), fontSize, color, font,
                                  softWrap);
}

} // namespace evk::ui
