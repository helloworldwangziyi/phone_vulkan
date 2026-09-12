#pragma once

/**
 * @file gesture_detector.h
 * @brief 手势探测组件（GestureDetector）：给孩子挂 tap / double tap /
 *        long press / scale 回调。
 *
 * 对照 Flutter 的 GestureDetector。识别与仲裁在输入管线
 * （core/src/ui/pointer_input.cpp）：本组件只是把回调挂到包裹 View 上。
 * 注意语义与 Flutter 一致：
 * - 设了 onDoubleTap 时，onTap 延迟 300ms 结算（等待第二击）；
 * - onLongPress 触发只废 tap 系，之后的移动仍可滚动外层；
 * - onScale 集齐双指即胜，双指的 tap/pan 认领作废（外层滚动收 Cancel）。
 */

#include "evk/ui/render_view.h"
#include "evk/ui/widget_tree.h"

namespace evk::ui {

/// GestureDetector 的手势回调集：按需设置，未设的手势不参与识别。
struct GestureCallbacks {
    std::function<void(const ClickEvent&)> onTap;
    std::function<void(const ClickEvent&)> onDoubleTap;
    std::function<void(const ClickEvent&)> onLongPress;
    std::function<void(const ScaleEvent&)> onScale;
};

/// 手势探测组件：包一层透明 View，把手势回调挂到孩子身上。
class GestureDetector final : public RenderObjectWidget {
public:
    GestureCallbacks callbacks;

    GestureDetector(std::unique_ptr<Widget> child, GestureCallbacks callbacks);
    std::unique_ptr<View> createRenderObject() const override;
    void updateRenderObject(View& view) const override;
    std::vector<std::unique_ptr<Widget>>& children() override { return children_; }

private:
    std::vector<std::unique_ptr<Widget>> children_;
};

/// 构造辅助：一行造一个手势探测器。
std::unique_ptr<Widget> gestureDetector(
    std::unique_ptr<Widget> child,
    GestureCallbacks callbacks);

} // namespace evk::ui
