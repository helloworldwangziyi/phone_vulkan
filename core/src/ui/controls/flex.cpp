#include "evk/ui/controls/flex.h"

#include "evk/ui/layout/flex_layout.h"
#include "evk/ui/render_view.h"

namespace evk::ui {

/**
 * @brief Flex 的构造：顺手算「固有主轴尺寸」（intrinsic main size）。
 *
 * 全部孩子都有固定主轴尺寸（mainSize≥0）时，把「尺寸 + 前后间距」累加
 * 成自己的固有尺寸，写进对外上报的排布参数（intrinsicData_）——这样
 * Column/Row 套在别的 Flex 里、或作为页面根时，父布局知道它
 * 「最小要多长」。有一个孩子是弹性的（flex>0）就放弃固有尺寸
 * （主尺寸为负 = 交给父布局）。
 */
Flex::Flex(Axis direction, std::vector<std::unique_ptr<Widget>> childWidgets)
    : axis(direction), children_(std::move(childWidgets)) {
    float intrinsicMainSize = 0.0f;
    for (const auto& child : children_) {
        if (!child) {
            continue;
        }
        const FlexParentData childData = child->flexParentData(axis);
        if (childData.mainSize < 0.0f) {
            return;
        }
        intrinsicMainSize += childData.before + childData.mainSize + childData.after;
    }
    intrinsicData_.mainSize = intrinsicMainSize;
}

FlexParentData Flex::flexParentData(Axis parentAxis) const {
    return parentAxis == axis ? intrinsicData_ : FlexParentData{};
}

std::unique_ptr<View> Flex::createRenderObject() const {
    auto view = createFlexView(axis);
    updateRenderObject(*view);
    return view;
}

void Flex::updateRenderObject(View& view) const {
    if (color == 0) {
        view.clearBackground();
    } else {
        view.setBackground(color);
    }
}

void Flex::configureChild(View& parent, const Widget& child, View& childView) const {
    setFlexChild(parent, childView, child.flexParentData(axis));
}

Column::Column(std::vector<std::unique_ptr<Widget>> childWidgets)
    : Flex(Axis::Vertical, std::move(childWidgets)) {}

Row::Row(std::vector<std::unique_ptr<Widget>> childWidgets)
    : Flex(Axis::Horizontal, std::move(childWidgets)) {}

} // namespace evk::ui
