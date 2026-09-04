#include "evk/ui/controls/basic.h"

#include <algorithm>
#include <utility>

#include "evk/ui/render_view.h"

namespace evk::ui {
namespace {

/**
 * @brief SizedBox 对应的 View：锁死显式给定的轴（tight 约束），
 *        未锁轴有界撑满、无界包孩子。
 *
 * 尺寸在约束协议内自我强制（不再经 flexParentData 上报）：锁死的轴给
 * 孩子 tight 约束；自身回报 = 锁死值 / 约束 max / 孩子尺寸，最后被
 * 下行约束钳制（父 tight 优先，与 Flutter 一致）。
 */
class SizedBoxView final : public View {
public:
    float width = -1.0f;
    float height = -1.0f;

    Size performLayout(const BoxConstraints& constraints) override {
        BoxConstraints childConstraints = constraints;
        if (width >= 0.0f) {
            childConstraints.minWidth = childConstraints.maxWidth = width;
        }
        if (height >= 0.0f) {
            childConstraints.minHeight = childConstraints.maxHeight = height;
        }
        Size childSize{0.0f, 0.0f};
        if (!children.empty()) {
            childSize = children.front()->layout(childConstraints);
            children.front()->setPosition(0.0f, 0.0f);
        }
        return {
            width >= 0.0f ? width
                          : (constraints.isWidthBounded() ? constraints.maxWidth
                                                          : childSize.width),
            height >= 0.0f ? height
                           : (constraints.isHeightBounded() ? constraints.maxHeight
                                                            : childSize.height),
        };
    }
};

/**
 * @brief Padding 对应的 View：约束 deflate 后下行给孩子，自身回报
 *        孩子尺寸 + 内边距（受下行约束钳制）。
 */
class PaddingView final : public View {
public:
    EdgeInsets insets;

    Size performLayout(const BoxConstraints& constraints) override {
        const float horizontal = insets.left + insets.right;
        const float vertical = insets.top + insets.bottom;
        Size childSize{0.0f, 0.0f};
        if (!children.empty()) {
            childSize =
                children.front()->layout(constraints.deflate(horizontal, vertical));
            children.front()->setPosition(insets.left, insets.top);
        }
        return {childSize.width + horizontal, childSize.height + vertical};
    }
};

/**
 * @brief 居中容器对应的 View：松约束让孩子自测，自身有界撑满、
 *        无界包孩子，孩子在自身范围内两轴居中。
 *
 * 对照 Flutter 的 RenderPositionedBox（Center 的渲染对象）：下行的
 * tight 约束在此**松掉**（min 清零），孩子按内容定尺寸后居中摆放——
 * 不再经 flexParentData 把居中意图上传给 Flex 父容器（那条路在非
 * Flex 父级如 Padding 下会断：tight min 会把孩子钳大，内容贴左）。
 */
class CenterView final : public View {
public:
    Size performLayout(const BoxConstraints& constraints) override {
        Size childSize{0.0f, 0.0f};
        if (!children.empty()) {
            const BoxConstraints loose{
                0.0f, constraints.maxWidth, 0.0f, constraints.maxHeight};
            childSize = children.front()->layout(loose);
        }
        const float width = constraints.isWidthBounded() ? constraints.maxWidth
                                                         : childSize.width;
        const float height = constraints.isHeightBounded() ? constraints.maxHeight
                                                           : childSize.height;
        if (!children.empty()) {
            children.front()->setPosition((width - childSize.width) * 0.5f,
                                          (height - childSize.height) * 0.5f);
        }
        return {width, height};
    }
};

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
 * @brief Expanded 的「参数改写」模式：构造时把孩子的 flexParentData
 *        抄一份存着，被问时改几笔再上报。
 *
 * 转接件自己不产 View，却要参与父 Flex 的排布——靠的就是把孩子的
 * flexParentData 往上传时做修改：Expanded 把 mainSize 置 -1、flex 置
 * 自己的系数（吃剩余空间）。注意按轴向存两份（horizontal/vertical）：
 * 同一节点在两个方向的父容器里排布参数可以完全不同。
 *
 * （SizedBox/Padding/Center 不在此列：它们的尺寸/内边距/居中行为在
 * View 层经约束协议自我强制，无需向父 Flex 上报。）
 */
FlexParentData Expanded::flexParentData(Axis axis) const {
    FlexParentData data = axis == Axis::Vertical ? verticalData_ : horizontalData_;
    data.mainSize = -1.0f;
    data.flex = std::max(0.0f, flex_);
    return data;
}

SizedBox::SizedBox(float width, float height, std::unique_ptr<Widget> child)
    : width_(width), height_(height) {
    children_.push_back(std::move(child));
}

std::unique_ptr<View> SizedBox::createRenderObject() const {
    auto view = std::make_unique<SizedBoxView>();
    view->width = width_;
    view->height = height_;
    return view;
}

void SizedBox::updateRenderObject(View& view) const {
    if (auto* sizedBoxView = dynamic_cast<SizedBoxView*>(&view)) {
        sizedBoxView->width = width_;
        sizedBoxView->height = height_;
    }
}

Padding::Padding(EdgeInsets insets, std::unique_ptr<Widget> child)
    : insets_(insets) {
    children_.push_back(std::move(child));
}

std::unique_ptr<View> Padding::createRenderObject() const {
    auto view = std::make_unique<PaddingView>();
    view->insets = insets_;
    return view;
}

void Padding::updateRenderObject(View& view) const {
    if (auto* paddingView = dynamic_cast<PaddingView*>(&view)) {
        paddingView->insets = insets_;
    }
}

Center::Center(std::unique_ptr<Widget> child) {
    children_.push_back(std::move(child));
}

std::unique_ptr<View> Center::createRenderObject() const {
    return std::make_unique<CenterView>();
}

void Center::updateRenderObject(View&) const {}

std::unique_ptr<Widget> expanded(std::unique_ptr<Widget> child, float flex) {
    return makeWidget<Expanded>(std::move(child), flex);
}

std::unique_ptr<Widget> sizedBox(
    float width,
    float height,
    std::unique_ptr<Widget> child) {
    return makeWidget<SizedBox>(width, height, std::move(child));
}

std::unique_ptr<Widget> padding(
    EdgeInsets insets,
    std::unique_ptr<Widget> child) {
    return makeWidget<Padding>(insets, std::move(child));
}

std::unique_ptr<Widget> center(std::unique_ptr<Widget> child) {
    return makeWidget<Center>(std::move(child));
}

} // namespace evk::ui
