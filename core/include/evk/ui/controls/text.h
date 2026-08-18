#pragma once

/**
 * @file text.h
 * @brief 单行文本组件（TextWidget）：内容、字号、颜色与首选字体。
 *
 * 对照 Flutter 的 widgets/text.dart（单行、无富文本的简化版）。尺寸由
 * FontEngine 测量得出；绘制经 PaintContext::drawText，字形按需进 atlas。
 */

#include "evk/ui/font_engine.h"
#include "evk/ui/widget_tree.h"

namespace evk::ui {

/**
 * @brief 单行文本 Widget：内容、字号、颜色与首选字体。
 *
 * 尺寸由 FontEngine 测量得出（布局期间调用，不触发光栅化）：
 * 纵向容器里占一行高度、横向容器里占实测宽度；绘制在 painter 里经
 * PaintContext::drawText 完成，字形按需进 atlas。首选字体缺字时
 * 自动按注册顺序回退（如 Latin 字体 + CJK 字体混排一行）。
 */
class TextWidget final : public RenderObjectWidget {
public:
    TextWidget(std::string content, float fontSize, uint32_t color,
               FontId font = kFontAny);

    std::unique_ptr<View> createRenderObject() const override;
    void updateRenderObject(View& view) const override;
    FlexParentData flexParentData(Axis axis) const override;

private:
    std::string content_; ///< 文本内容（UTF-8）
    float fontSize_;      ///< 字号（像素高度）
    uint32_t color_;      ///< 文字颜色（RGBA）
    FontId font_;         ///< 首选字体；kFontAny = 按注册顺序回退
};

/// 构造辅助：一行造一个文本。
std::unique_ptr<Widget> text(std::string content, float fontSize, uint32_t color,
                             FontId font = kFontAny);

} // namespace evk::ui
