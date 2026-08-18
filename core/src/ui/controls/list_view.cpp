#include "evk/ui/controls/list_view.h"

#include "evk/ui/controls/scroll_control.h"
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

void ListView::configureChild(
    View& parent, const Widget&, View& childView) const {
    // 行位置 = 在 content 里的下标 × 行高（与 ListContentView::layoutRows
    // 一致；此处负责「刚挂载的行」的首次落位，增删后的整体重排由
    // content 层的 handleBoundsChanged/handleChildRemoved 触发）。
    View* content = scrollContent(parent);
    if (!content) {
        return;
    }
    for (size_t i = 0; i < content->children.size(); ++i) {
        if (content->children[i].get() == &childView) {
            childView.setBounds(
                0.0f,
                itemHeight_ * static_cast<float>(i),
                content->rect.w,
                itemHeight_);
            return;
        }
    }
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
