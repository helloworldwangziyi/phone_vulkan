#include "evk/ui/controls/scroll_view.h"

#include <utility>

#include "evk/ui/view/scroll_control.h"
#include "evk/ui/render_view.h"

namespace evk::ui {

ScrollViewWidget::ScrollViewWidget(
    std::unique_ptr<Widget> child,
    float height,
    std::function<void(float, float)> callback)
    : contentHeight(height), onScroll(std::move(callback)) {
    children_.push_back(std::move(child));
}

std::unique_ptr<View> ScrollViewWidget::createRenderObject() const {
    return createScrollView(contentWidth, contentHeight, onScroll);
}

void ScrollViewWidget::updateRenderObject(View& view) const {
    updateScrollView(view, contentWidth, contentHeight, onScroll);
}

View* ScrollViewWidget::childParent(View& view) const {
    return scrollContent(view);
}

std::unique_ptr<Widget> scrollView(
    std::unique_ptr<Widget> child,
    float contentHeight,
    std::function<void(float, float)> onScroll) {
    return makeWidget<ScrollViewWidget>(
        std::move(child), contentHeight, std::move(onScroll));
}

} // namespace evk::ui
