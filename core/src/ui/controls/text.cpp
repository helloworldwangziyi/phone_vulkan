#include "evk/ui/controls/text.h"

#include <algorithm>
#include <utility>

#include "evk/ui/render_view.h"
#include "evk/ui/text_layout.h"

namespace evk::ui {

namespace {

/**
 * @brief 单行文本的 View：布局期经 FontEngine 测量自报尺寸。
 *
 * 不再需要 Widget 构建期预量（旧 flexParentData 通道删除）：父布局
 * 下行约束（Column 里主轴无界、交叉轴 tight 拉伸；Row 里相反），
 * performLayout 量出文本宽高后 constrain 进区间回报。
 */
class TextView final : public View {
public:
    std::string content;  ///< 文本内容（UTF-8）
    float sizePx = 0.0f;  ///< 字号
    FontId font = kFontAny; ///< 首选字体

    Size performLayout(const BoxConstraints& constraints) override {
        float width = 0.0f;
        float height = 0.0f;
        FontEngine::instance().measureText(content.c_str(), sizePx, font,
                                           &width, &height);
        return constraints.constrain({width, height});
    }
};

/**
 * @brief 换行模式 Text 的 View：宽度取约束上限，高度按行数自报。
 *
 * performLayout 里以约束 maxWidth 排版（TextLayout 按键缓存，同参
 * 重入不重排），高度 = 行数 × 行高上行给父布局——旧的 setFlexChild
 * 回灌 hack 由约束协议自然取代。宽度无界（如 Row 主轴）时不软换行
 * （仅 \n 断行），报最大行宽 × 总高。
 */
class WrappedTextView final : public View {
public:
    std::string content;  ///< 文本内容（UTF-8）
    float sizePx = 0.0f;  ///< 字号
    uint32_t color = 0;   ///< 文字颜色（RGBA）
    FontId font = kFontAny; ///< 首选字体
    float lineHeightScale = 1.0f;         ///< 行距倍数
    TextAlign align = TextAlign::kLeft;   ///< 水平对齐
    int maxLines = 0;                     ///< 行数上限（0 = 不限）
    TextLayout layout;    ///< 排版缓存（键：文本/字号/字体/宽度/行距/对齐/行数）

    Size performLayout(const BoxConstraints& constraints) override {
        if (constraints.isWidthBounded()) {
            layout.layout(content.c_str(), sizePx, font, constraints.maxWidth,
                          lineHeightScale, align, maxLines);
            return constraints.constrain(
                {constraints.maxWidth, layout.totalHeight()});
        }
        layout.layout(content.c_str(), sizePx, font, 0.0f, lineHeightScale,
                      align, maxLines);
        float widest = 0.0f;
        for (const auto& line : layout.lines()) {
            widest = std::max(widest, line.width);
        }
        return constraints.constrain({widest, layout.totalHeight()});
    }
};

} // namespace

TextWidget::TextWidget(std::string content, float fontSize, uint32_t color,
                       FontId font, bool softWrap, float lineHeightScale,
                       TextAlign align, int maxLines)
    : content_(std::move(content)),
      fontSize_(fontSize),
      color_(color),
      font_(font),
      softWrap_(softWrap),
      lineHeightScale_(lineHeightScale),
      align_(align),
      maxLines_(maxLines) {}

std::unique_ptr<View> TextWidget::createRenderObject() const {
    if (softWrap_) {
        auto view = std::make_unique<WrappedTextView>();
        updateRenderObject(*view);
        return view;
    }
    auto view = std::make_unique<TextView>();
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
        self->lineHeightScale = lineHeightScale_;
        self->align = align_;
        self->maxLines = maxLines_;
        view.painter = [self](PaintContext& paint) {
            // 排版已在布局期按约束宽度完成；相同输入命中缓存直接复用。
            self->layout.layout(self->content.c_str(), self->sizePx, self->font,
                                paint.size().width, self->lineHeightScale,
                                self->align, self->maxLines);
            self->layout.paint(paint, 0.0f, 0.0f, self->color);
        };
        return;
    }
    auto* self = static_cast<TextView*>(&view);
    self->content = content_;
    self->sizePx = fontSize_;
    self->font = font_;
    view.painter = [content = content_, font = font_, size = fontSize_,
                    color = color_](PaintContext& paint) {
        paint.drawText(content.c_str(), font, 0.0f, 0.0f, size, color);
    };
}

bool TextWidget::canUpdate(const Widget& other) const {
    const auto* text = dynamic_cast<const TextWidget*>(&other);
    return text && text->softWrap_ == softWrap_;
}

std::unique_ptr<Widget> text(std::string content, float fontSize, uint32_t color,
                             FontId font, bool softWrap, float lineHeightScale,
                             TextAlign align, int maxLines) {
    return makeWidget<TextWidget>(std::move(content), fontSize, color, font,
                                  softWrap, lineHeightScale, align, maxLines);
}

} // namespace evk::ui
