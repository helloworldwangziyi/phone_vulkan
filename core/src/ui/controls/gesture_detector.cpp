/**
 * @file gesture_detector.cpp
 * @brief GestureDetector 的实现：透明包裹 View + 回调挂载。
 */

#include "evk/ui/controls/gesture_detector.h"

#include <utility>

namespace evk::ui {
namespace {

/**
 * @brief 手势探测对应的 View：自身无视觉，尺寸包孩子（受下行约束钳制），
 *        回调字段由 widget 侧刷新。识别全部交给输入管线（onClick 即
 *        tap 路径；onScale 非空即 acceptsScaleInput）。
 */
class GestureDetectorView final : public View {
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

GestureDetectorView* asDetector(View& view) {
    return dynamic_cast<GestureDetectorView*>(&view);
}

} // namespace

GestureDetector::GestureDetector(
    std::unique_ptr<Widget> child,
    GestureCallbacks value)
    : callbacks(std::move(value)) {
    children_.push_back(std::move(child));
}

std::unique_ptr<View> GestureDetector::createRenderObject() const {
    auto view = std::make_unique<GestureDetectorView>();
    view->onClick = callbacks.onTap;
    view->onDoubleTap = callbacks.onDoubleTap;
    view->onLongPress = callbacks.onLongPress;
    view->onScale = callbacks.onScale;
    return view;
}

void GestureDetector::updateRenderObject(View& view) const {
    if (auto* detector = asDetector(view)) {
        detector->onClick = callbacks.onTap;
        detector->onDoubleTap = callbacks.onDoubleTap;
        detector->onLongPress = callbacks.onLongPress;
        detector->onScale = callbacks.onScale;
    }
}

std::unique_ptr<Widget> gestureDetector(
    std::unique_ptr<Widget> child,
    GestureCallbacks callbacks) {
    return makeWidget<GestureDetector>(std::move(child), std::move(callbacks));
}

} // namespace evk::ui
