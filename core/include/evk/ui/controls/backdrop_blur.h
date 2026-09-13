#pragma once

/**
 * @file backdrop_blur.h
 * @brief 背景模糊组件（BackdropBlur）：把本组件之下的内容高斯模糊后
 *        合成回本区域（可选圆角），孩子正常画在其上。
 *
 * 对照 Flutter 的 BackdropFilter。实现：paint 时向 Canvas 发一条 0 顶点
 * 的模糊标记批；渲染层遇到标记把已绘场景离屏做可分离高斯（H+V 两趟）
 * 再合成回该区域。有模糊的帧整帧走离屏通道；无模糊帧零开销。
 */

#include "evk/ui/render_view.h"
#include "evk/ui/widget_tree.h"

namespace evk::ui {

/// 背景模糊组件：尺寸包孩子；sigma 为高斯半径（像素，渲染层钳 ≤32）。
class BackdropBlur final : public RenderObjectWidget {
public:
    float sigma = 0.0f;  ///< 模糊半径（像素）
    float radius = 0.0f; ///< 合成区域的圆角半径（0 = 直角）

    BackdropBlur(float sigma, float radius, std::unique_ptr<Widget> child);
    std::unique_ptr<View> createRenderObject() const override;
    void updateRenderObject(View& view) const override;
    std::vector<std::unique_ptr<Widget>>& children() override { return children_; }

private:
    std::vector<std::unique_ptr<Widget>> children_;
};

/// 构造辅助：一行造一个背景模糊。
std::unique_ptr<Widget> backdropBlur(float sigma, float radius,
                                     std::unique_ptr<Widget> child);

} // namespace evk::ui
