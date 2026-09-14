/**
 * @file controls/semantics.cpp
 * @brief Semantics 标注包装的实现：透明容器 View + 标注写入。
 */

#include "evk/ui/controls/semantics.h"

#include <utility>

namespace evk::ui {
namespace {

/**
 * @brief 语义标注对应的 View：自身无视觉，尺寸包孩子（受下行约束钳制），
 *        标注经 View::semantics 字段由语义收集读取。布局同
 *        GestureDetectorView（透明单子容器）。
 */
class SemanticsView final : public View {
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

SemanticsWidget::SemanticsWidget(
    std::string label,
    SemanticsRole role,
    std::unique_ptr<Widget> child)
    : label_(std::move(label)), role_(role) {
    children_.push_back(std::move(child));
}

std::unique_ptr<View> SemanticsWidget::createRenderObject() const {
    auto view = std::make_unique<SemanticsView>();
    updateRenderObject(*view);
    return view;
}

void SemanticsWidget::updateRenderObject(View& view) const {
    if (!view.semantics) {
        view.semantics = std::make_unique<SemanticsConfig>();
    }
    view.semantics->label = label_;
    if (role_ != SemanticsRole::kNone) {
        view.semantics->role = role_;
    }
}

std::unique_ptr<Widget> semantics(
    std::string label,
    SemanticsRole role,
    std::unique_ptr<Widget> child) {
    return makeWidget<SemanticsWidget>(std::move(label), role, std::move(child));
}

std::unique_ptr<Widget> semantics(std::string label, std::unique_ptr<Widget> child) {
    return makeWidget<SemanticsWidget>(
        std::move(label), SemanticsRole::kNone, std::move(child));
}

} // namespace evk::ui
