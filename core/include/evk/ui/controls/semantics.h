#pragma once

/**
 * @file controls/semantics.h
 * @brief 语义标注包装组件（Semantics）：给孩子子树挂无障碍标注。
 *
 * 对照 Flutter 的 widgets/semantics.dart（精简版：单孩子、label/role 两
 * 个维度）。用途：painter 自绘的内容（画进顶点流的文字/图形）对语义树
 * 不可见，用它补标注；或给可交互容器补朗读标签。
 *
 * 视图层是透明容器（尺寸包孩子，不产生绘制），标注经 View::semantics
 * 字段生效；角色/内容由控件内省（fillSemantics）回填的节点不受影响
 * 优先级更高的是手工标注。
 */

#include <memory>
#include <string>

#include "evk/ui/semantics.h"
#include "evk/ui/widget_tree.h"

namespace evk::ui {

/**
 * @brief 语义标注包装：给孩子子树挂 label/role，自身透明。
 */
class SemanticsWidget final : public RenderObjectWidget {
public:
    SemanticsWidget(std::string label, SemanticsRole role,
                    std::unique_ptr<Widget> child);

    std::unique_ptr<View> createRenderObject() const override;
    void updateRenderObject(View& view) const override;
    std::vector<std::unique_ptr<Widget>>& children() override { return children_; }

private:
    std::string label_;
    SemanticsRole role_;
    std::vector<std::unique_ptr<Widget>> children_;  ///< 恒为 1 个孩子
};

/// 构造辅助：语义标注包装（role 省略时为 kNone，由内容/动作推导）。
std::unique_ptr<Widget> semantics(
    std::string label, SemanticsRole role, std::unique_ptr<Widget> child);
std::unique_ptr<Widget> semantics(std::string label, std::unique_ptr<Widget> child);

} // namespace evk::ui
