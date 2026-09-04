#include "evk/ui/controls/flex.h"

#include "evk/ui/layout/flex_layout.h"
#include "evk/ui/render_view.h"

namespace evk::ui {

Flex::Flex(Axis direction, std::vector<std::unique_ptr<Widget>> childWidgets)
    : axis(direction), children_(std::move(childWidgets)) {}

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
