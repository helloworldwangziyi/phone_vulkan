#pragma once

/**
 * @file flex.h
 * @brief 线性容器组件（Flex / Column / Row）与 column/row 构造辅助。
 *
 * 对照 Flutter 的 widgets/flex.dart + basic.dart 里的 Row/Column。
 * View 层（FlexView 排布算法）见 layout/flex_layout.h。
 */

#include "evk/ui/widget_tree.h"

namespace evk::ui {

/**
 * @brief 线性容器（Column/Row 的公共基类）。
 *
 * 对应的 View 是 FlexView：主轴空间先扣固定项与间距，剩余按 flex 系数
 * 分给弹性项（Expanded）。每个孩子的排布参数来自其 Widget 的
 * flexParentData()——所以 App 不写布局函数：尺寸变化时 FlexView 经
 * handleBoundsChanged 自动级联重排。
 */
class Flex : public RenderObjectWidget {
public:
    Axis axis;
    uint32_t color = 0;

    Flex(Axis axis, std::vector<std::unique_ptr<Widget>> children);
    FlexParentData flexParentData(Axis parentAxis) const override;
    std::unique_ptr<View> createRenderObject() const override;
    void updateRenderObject(View& view) const override;
    std::vector<std::unique_ptr<Widget>>& children() override { return children_; }
    void configureChild(View& parent, const Widget& child, View& childView) const override;

private:
    FlexParentData intrinsicData_;
    std::vector<std::unique_ptr<Widget>> children_;
};

/// 纵向 Flex：孩子自上而下排布。
class Column final : public Flex {
public:
    explicit Column(std::vector<std::unique_ptr<Widget>> children);
};

/// 横向 Flex：孩子自左而右排布。
class Row final : public Flex {
public:
    explicit Row(std::vector<std::unique_ptr<Widget>> children);
};

/// 构造辅助：纵向 Flex 描述。
template <typename... Widgets>
std::unique_ptr<Widget> column(Widgets&&... widgets) {
    return makeWidget<Column>(widgetList(std::forward<Widgets>(widgets)...));
}

inline std::unique_ptr<Widget> column(std::vector<std::unique_ptr<Widget>> widgets) {
    return makeWidget<Column>(std::move(widgets));
}

/// 构造辅助：横向 Flex 描述。
template <typename... Widgets>
std::unique_ptr<Widget> row(Widgets&&... widgets) {
    return makeWidget<Row>(widgetList(std::forward<Widgets>(widgets)...));
}

inline std::unique_ptr<Widget> row(std::vector<std::unique_ptr<Widget>> widgets) {
    return makeWidget<Row>(std::move(widgets));
}

} // namespace evk::ui
