#pragma once

/**
 * @file text.h
 * @brief 文本组件（TextWidget）：内容、字号、颜色与首选字体；可选定宽换行。
 *
 * 对照 Flutter 的 widgets/text.dart（无富文本的简化版）。默认单行：尺寸由
 * FontEngine 测量得出；softWrap = true 时按视图宽度自动换行（TextLayout），
 * 高度由行数撑出。绘制经 PaintContext::drawText，字形按需进 atlas。
 */

#include "evk/ui/font_engine.h"
#include "evk/ui/widget_tree.h"

namespace evk::ui {

/**
 * @brief 文本 Widget：内容、字号、颜色与首选字体，可选按宽度换行。
 *
 * 尺寸由 FontEngine 测量得出（布局期间调用，不触发光栅化）：
 * 单行模式下纵向容器里占一行高度、横向容器里占实测宽度；
 * softWrap 换行模式下以视图宽度为约束排版，纵向容器里高度 =
 * 行数 × 行高（视图拿到宽度后自行回灌，见 text.cpp 的 WrappedTextView）。
 * 绘制在 painter 里经 PaintContext::drawText 完成，字形按需进 atlas。
 * 首选字体缺字时自动按注册顺序回退（如 Latin 字体 + CJK 字体混排）。
 */
class TextWidget final : public RenderObjectWidget {
public:
    TextWidget(std::string content, float fontSize, uint32_t color,
               FontId font = kFontAny, bool softWrap = false);

    std::unique_ptr<View> createRenderObject() const override;
    void updateRenderObject(View& view) const override;
    FlexParentData flexParentData(Axis axis) const override;
    /// 单行/换行两种模式对应不同的 View 类型，模式切换必须重建子树。
    bool canUpdate(const Widget& other) const override;

private:
    std::string content_; ///< 文本内容（UTF-8）
    float fontSize_;      ///< 字号（像素高度）
    uint32_t color_;      ///< 文字颜色（RGBA）
    FontId font_;         ///< 首选字体；kFontAny = 按注册顺序回退
    bool softWrap_;       ///< true = 按视图宽度自动换行（多行）
};

/// 构造辅助：一行造一个文本；softWrap = true 时按视图宽度自动换行。
std::unique_ptr<Widget> text(std::string content, float fontSize, uint32_t color,
                             FontId font = kFontAny, bool softWrap = false);

} // namespace evk::ui
