#pragma once

/**
 * @file clip_rrect.h
 * @brief 圆角裁剪组件（ClipRRect）：把孩子（含其自绘）裁进圆角矩形。
 *
 * 对照 Flutter 的 ClipRRect。实现：包一层透明 View 并设 clipRadius，
 * 绘制时背景/painter/孩子的全部批次带 SDF 圆角遮罩（片元距离场削角，
 * 1px 抗锯齿边缘）；矩形 scissor 照旧逐层收窄。嵌套圆角裁剪只有最
 * 内层生效（矩形裁剪嵌套不限）。
 */

#include "evk/ui/render_view.h"
#include "evk/ui/widget_tree.h"

namespace evk::ui {

/// 圆角裁剪组件：尺寸包孩子，把孩子裁进圆角矩形。
class ClipRRect final : public RenderObjectWidget {
public:
    float radius = 0.0f;

    ClipRRect(float radius, std::unique_ptr<Widget> child);
    std::unique_ptr<View> createRenderObject() const override;
    void updateRenderObject(View& view) const override;
    std::vector<std::unique_ptr<Widget>>& children() override { return children_; }

private:
    std::vector<std::unique_ptr<Widget>> children_;
};

/// 构造辅助：一行造一个圆角裁剪。
std::unique_ptr<Widget> clipRRect(float radius, std::unique_ptr<Widget> child);

} // namespace evk::ui
