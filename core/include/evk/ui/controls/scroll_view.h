#pragma once

/**
 * @file scroll_view.h
 * @brief 可滚动容器组件（ScrollViewWidget）：拖动 / 惯性 / 回弹。
 *
 * 对照 Flutter 的 widgets/scroll_view.dart。View 层实现见
 * view/scroll_control.h——视口内挂 content 层，滚动 = 平移 content。
 */

#include "evk/ui/widget_tree.h"

namespace evk::ui {

/**
 * @brief 可滚动容器（拖动 / 惯性 / 回弹）。
 *
 * 单子组件：子 View 挂到滚动 content 实体下（childParent 重定向），
 * 由 content 层经约束协议 tight 塞满。contentHeight 由 App 显式给出
 * （v1 无测量、无懒构建）；滚动偏移是控件内部状态，重建后保留。
 */
class ScrollViewWidget final : public RenderObjectWidget {
public:
    float contentWidth = 0.0f;
    float contentHeight = 0.0f;
    std::function<void(float, float)> onScroll;

    ScrollViewWidget(
        std::unique_ptr<Widget> child,
        float contentHeight,
        std::function<void(float, float)> onScroll = {});

    std::unique_ptr<View> createRenderObject() const override;
    void updateRenderObject(View& view) const override;
    std::vector<std::unique_ptr<Widget>>& children() override { return children_; }
    View* childParent(View& view) const override;

private:
    std::vector<std::unique_ptr<Widget>> children_;
};

/// 构造辅助：一行造一个滚动容器。
std::unique_ptr<Widget> scrollView(
    std::unique_ptr<Widget> child,
    float contentHeight,
    std::function<void(float, float)> onScroll = {});

} // namespace evk::ui
