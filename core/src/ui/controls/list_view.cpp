#include "evk/ui/controls/list_view.h"

#include "evk/ui/view/scroll_control.h"
#include "evk/ui/render_view.h"

namespace evk::ui {

ListView::ListView(float itemHeight, std::vector<std::unique_ptr<Widget>> items)
    : itemHeight_(itemHeight), children_(std::move(items)) {}

std::unique_ptr<View> ListView::createRenderObject() const {
    return createListView(
        itemHeight_,
        contentWidth,
        itemHeight_ * static_cast<float>(children_.size()),
        onScroll);
}

void ListView::updateRenderObject(View& view) const {
    updateListView(
        view,
        itemHeight_,
        contentWidth,
        itemHeight_ * static_cast<float>(children_.size()),
        onScroll);
}

View* ListView::childParent(View& view) const {
    return scrollContent(view);
}

std::unique_ptr<Widget> listView(
    float itemHeight,
    std::vector<std::unique_ptr<Widget>> items,
    float contentWidth) {
    auto result = makeWidget<ListView>(itemHeight, std::move(items));
    static_cast<ListView*>(result.get())->contentWidth = contentWidth;
    return result;
}

} // namespace evk::ui
