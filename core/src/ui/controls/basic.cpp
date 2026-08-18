#include "evk/ui/controls/basic.h"

#include <algorithm>
#include <utility>

#include "evk/ui/render_view.h"

namespace evk::ui {
namespace {

/**
 * @brief SizedBox 对应的 View：自己的 bounds 被（Flex 等）写入后，把
 *        孩子塞满。
 *
 * 注意这是布局链的典型形态：父布局只管写「这个节点多大」，节点内部
 * 如何分配由它自己的 handleBoundsChanged 决定——尺寸变化沿 View 树
 * 级联，App 全程不写布局函数。
 */
class SizedBoxView final : public View {
public:
    void handleBoundsChanged() override {
        if (!children.empty()) {
            children.front()->setBounds(0.0f, 0.0f, rect.w, rect.h);
        }
    }
};

/**
 * @brief Padding 对应的 View：孩子缩进 insets 后占据剩余空间。
 */
class PaddingView final : public View {
public:
    EdgeInsets insets;

    void handleBoundsChanged() override {
        if (children.empty()) {
            return;
        }
        children.front()->setBounds(
            insets.left,
            insets.top,
            std::max(0.0f, rect.w - insets.left - insets.right),
            std::max(0.0f, rect.h - insets.top - insets.bottom));
    }
};

/// 读孩子对外上报的排布参数（无孩子 = 默认值）。
FlexParentData childDataOf(const std::unique_ptr<Widget>& child, Axis axis) {
    return child ? child->flexParentData(axis) : FlexParentData{};
}

} // namespace

EdgeInsets EdgeInsets::all(float value) {
    return {value, value, value, value};
}

EdgeInsets EdgeInsets::symmetric(float horizontal, float vertical) {
    return {horizontal, vertical, horizontal, vertical};
}

EdgeInsets EdgeInsets::only(float left, float top, float right, float bottom) {
    return {left, top, right, bottom};
}

Expanded::Expanded(std::unique_ptr<Widget> child, float flex)
    : ProxyWidget(std::move(child)), flex_(flex) {
    horizontalData_ = child_ ? child_->flexParentData(Axis::Horizontal)
                             : FlexParentData{};
    verticalData_ = child_ ? child_->flexParentData(Axis::Vertical)
                           : FlexParentData{};
}

/**
 * @brief Expanded/Center/SizedBox/Padding 共用的「参数改写」模式：
 *        构造时把孩子的 flexParentData 抄一份存着，被问时改几笔再上报。
 *
 * 这些转接件自己不产 View，却要参与父 Flex 的排布——靠的就是把孩子的
 * flexParentData 往上传时做修改：
 *   - Expanded：把 mainSize 置 -1、flex 置自己的系数（吃剩余空间）；
 *   - Center  ：把 crossAlignment 改成居中；
 *   - SizedBox：把主/交叉尺寸改成自己的固定值（变相固定孩子）；
 *   - Padding ：把内边距折算进孩子上报的尺寸。
 * 注意按轴向存两份（horizontal/vertical）：同一节点在两个方向的父容器里
 * 排布参数可以完全不同。
 */
FlexParentData Expanded::flexParentData(Axis axis) const {
    FlexParentData data = axis == Axis::Vertical ? verticalData_ : horizontalData_;
    data.mainSize = -1.0f;
    data.flex = std::max(0.0f, flex_);
    return data;
}

SizedBox::SizedBox(float width, float height, std::unique_ptr<Widget> child)
    : width_(width),
      height_(height),
      horizontalData_(childDataOf(child, Axis::Horizontal)),
      verticalData_(childDataOf(child, Axis::Vertical)) {
    children_.push_back(std::move(child));
}

FlexParentData SizedBox::flexParentData(Axis axis) const {
    FlexParentData data = axis == Axis::Vertical ? verticalData_ : horizontalData_;
    data.mainSize = axis == Axis::Vertical ? height_ : width_;
    data.crossSize = axis == Axis::Vertical ? width_ : height_;
    data.flex = 0.0f;
    return data;
}

std::unique_ptr<View> SizedBox::createRenderObject() const {
    return std::make_unique<SizedBoxView>();
}

void SizedBox::updateRenderObject(View&) const {}

Padding::Padding(EdgeInsets insets, std::unique_ptr<Widget> child)
    : insets_(insets),
      horizontalData_(childDataOf(child, Axis::Horizontal)),
      verticalData_(childDataOf(child, Axis::Vertical)) {
    children_.push_back(std::move(child));
}

FlexParentData Padding::flexParentData(Axis axis) const {
    FlexParentData data = axis == Axis::Vertical ? verticalData_ : horizontalData_;
    const float mainPadding = axis == Axis::Vertical
        ? insets_.top + insets_.bottom
        : insets_.left + insets_.right;
    const float crossPadding = axis == Axis::Vertical
        ? insets_.left + insets_.right
        : insets_.top + insets_.bottom;
    if (data.mainSize >= 0.0f) {
        data.mainSize += mainPadding;
    }
    if (data.crossSize >= 0.0f) {
        data.crossSize += crossPadding;
    }
    return data;
}

std::unique_ptr<View> Padding::createRenderObject() const {
    auto view = std::make_unique<PaddingView>();
    view->insets = insets_;
    return view;
}

void Padding::updateRenderObject(View& view) const {
    if (auto* paddingView = dynamic_cast<PaddingView*>(&view)) {
        paddingView->insets = insets_;
        paddingView->handleBoundsChanged();
    }
}

Center::Center(std::unique_ptr<Widget> child)
    : ProxyWidget(std::move(child)) {
    horizontalData_ = child_ ? child_->flexParentData(Axis::Horizontal)
                             : FlexParentData{};
    verticalData_ = child_ ? child_->flexParentData(Axis::Vertical)
                           : FlexParentData{};
}

FlexParentData Center::flexParentData(Axis axis) const {
    FlexParentData data = axis == Axis::Vertical ? verticalData_ : horizontalData_;
    data.crossAlignment = CrossAxisAlignment::Center;
    return data;
}

std::unique_ptr<Widget> expanded(std::unique_ptr<Widget> child, float flex) {
    return makeWidget<Expanded>(std::move(child), flex);
}

std::unique_ptr<Widget> sizedBox(
    float width, float height, std::unique_ptr<Widget> child) {
    return makeWidget<SizedBox>(width, height, std::move(child));
}

std::unique_ptr<Widget> padding(
    EdgeInsets insets, std::unique_ptr<Widget> child) {
    return makeWidget<Padding>(insets, std::move(child));
}

std::unique_ptr<Widget> center(std::unique_ptr<Widget> child) {
    return makeWidget<Center>(std::move(child));
}

} // namespace evk::ui
