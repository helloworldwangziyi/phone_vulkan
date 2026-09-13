/**
 * @file backdrop_blur.cpp
 * @brief BackdropBlur 的实现：透明包裹 View + backdropBlurSigma 挂载。
 */

#include "evk/ui/controls/backdrop_blur.h"

namespace evk::ui {
namespace {

/// 背景模糊对应的 View：自身无视觉，尺寸包孩子（受下行约束钳制）。
/// 模糊标记批由 View::paint 按 backdropBlurSigma 发出（渲染层接离屏通道）。
class BackdropBlurView final : public View {
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

BackdropBlur::BackdropBlur(float sigmaValue, float radiusValue,
                           std::unique_ptr<Widget> child)
    : sigma(sigmaValue), radius(radiusValue) {
    children_.push_back(std::move(child));
}

std::unique_ptr<View> BackdropBlur::createRenderObject() const {
    auto view = std::make_unique<BackdropBlurView>();
    view->backdropBlurSigma = sigma;
    view->clipRadius = radius;
    return view;
}

void BackdropBlur::updateRenderObject(View& view) const {
    view.backdropBlurSigma = sigma;
    view.clipRadius = radius;
}

std::unique_ptr<Widget> backdropBlur(float sigma, float radius,
                                     std::unique_ptr<Widget> child) {
    return makeWidget<BackdropBlur>(sigma, radius, std::move(child));
}

} // namespace evk::ui
