/**
 * @file clip_rrect.cpp
 * @brief ClipRRect 的实现：透明包裹 View + clipRadius 挂载。
 */

#include "evk/ui/controls/clip_rrect.h"

namespace evk::ui {
namespace {

/// 圆角裁剪对应的 View：自身无视觉，尺寸包孩子（受下行约束钳制）。
class ClipRRectView final : public View {
public:
    Size performLayout(const BoxConstraints& constraints) override {
        if (children.empty()) {
            return constraints.biggest();
        }
        const Size size = children.front()->layout(constraints);
        children.front()->setPosition(0.0f, 0.0f);
        return size;
    }
};

} // namespace

ClipRRect::ClipRRect(float radiusValue, std::unique_ptr<Widget> child)
    : radius(radiusValue) {
    children_.push_back(std::move(child));
}

std::unique_ptr<View> ClipRRect::createRenderObject() const {
    auto view = std::make_unique<ClipRRectView>();
    view->clipRadius = radius;
    return view;
}

void ClipRRect::updateRenderObject(View& view) const {
    view.clipRadius = radius;
}

std::unique_ptr<Widget> clipRRect(float radius, std::unique_ptr<Widget> child) {
    return makeWidget<ClipRRect>(radius, std::move(child));
}

} // namespace evk::ui
