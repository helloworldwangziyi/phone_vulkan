#pragma once

/**
 * @file container.h
 * @brief 通用块级组件（Container）：背景 + 可选描边/圆角 + 点击 + 自绘。
 *
 * 对照 Flutter 的 widgets/container.dart（我们的 Container 一个类顶
 * Container + GestureDetector + CustomPaint）。View 层无需独立控件：
 * 直接用 View 自带的背景/painter 机制。
 */

#include "evk/ui/widget_tree.h"

namespace evk::ui {

/**
 * @brief 通用块级组件：背景色 + 可选描边/圆角 + 点击 + 自绘。
 *
 * 传了 onTap 才成为触控目标；painter 在帧构建期间被调用，只能经
 * PaintContext 绘制，不能改视图树。
 */
class Container final : public RenderObjectWidget {
public:
    uint32_t color = 0;
    uint32_t borderColor = 0;  ///< 描边色；非 0 且 borderWidth > 0 时生效
    float borderWidth = 0.0f;  ///< 描边宽度（像素，向内侧）
    float cornerRadius = 0.0f; ///< 圆角半径；> 0 时背景/描边走圆角绘制
    std::function<void()> onTap;
    std::function<void(PaintContext&)> painter;

    explicit Container(uint32_t color = 0);
    std::unique_ptr<View> createRenderObject() const override;
    void updateRenderObject(View& view) const override;
};

/// 构造辅助：一行造一个 Container。
std::unique_ptr<Widget> container(
    uint32_t color = 0,
    std::function<void()> onTap = {},
    std::function<void(PaintContext&)> painter = {});

} // namespace evk::ui
