#pragma once

/**
 * @file basic.h
 * @brief 基础小件合集：EdgeInsets / Expanded / SizedBox / Padding / Center。
 *
 * 对照 Flutter 的 widgets/basic.dart——单个文件装不下框架、但每个类
 * 又只有几十行的小组件集中放这里。共同模式：构造时把孩子的
 * flexParentData 抄一份存着，被父 Flex 询问时改几笔再上报（Expanded
 * 改 flex、Center 改对齐、SizedBox 固定尺寸、Padding 折算内边距）。
 */

#include "evk/ui/widget_tree.h"

namespace evk::ui {

/// 四边内边距（Padding 的参数）。
struct EdgeInsets {
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;

    static EdgeInsets all(float value);
    static EdgeInsets symmetric(float horizontal, float vertical);
    static EdgeInsets only(float left, float top, float right, float bottom);
};

/**
 * @brief 转接组件：让孩子在 Column/Row 里按 flex 系数瓜分剩余主轴空间。
 *
 * 对应 Flutter 的 Expanded。多个 Expanded 按 flex 比例分配。
 */
class Expanded final : public ProxyWidget {
public:
    Expanded(std::unique_ptr<Widget> child, float flex = 1.0f);
    FlexParentData flexParentData(Axis axis) const override;

private:
    float flex_;
    FlexParentData horizontalData_;
    FlexParentData verticalData_;
};

/**
 * @brief 固定尺寸盒子：给孩子一个明确的宽高。
 *
 * 宽或高传 -1 表示该方向不约束（如 SizedBox(-1, 540, child) 只定高度）。
 * 对应的 SizedBoxView 会在自身尺寸变化时把孩子塞满。
 */
class SizedBox final : public RenderObjectWidget {
public:
    SizedBox(float width, float height, std::unique_ptr<Widget> child);

    FlexParentData flexParentData(Axis axis) const override;
    std::unique_ptr<View> createRenderObject() const override;
    void updateRenderObject(View& view) const override;
    std::vector<std::unique_ptr<Widget>>& children() override { return children_; }

private:
    float width_;
    float height_;
    FlexParentData horizontalData_;
    FlexParentData verticalData_;
    std::vector<std::unique_ptr<Widget>> children_;
};

/**
 * @brief 内边距盒子：孩子的 View 缩进 insets 后占据剩余空间。
 *
 * 对 Column/Row 父容器上报的排布尺寸 = 孩子尺寸 + 内边距。
 */
class Padding final : public RenderObjectWidget {
public:
    Padding(EdgeInsets insets, std::unique_ptr<Widget> child);

    FlexParentData flexParentData(Axis axis) const override;
    std::unique_ptr<View> createRenderObject() const override;
    void updateRenderObject(View& view) const override;
    std::vector<std::unique_ptr<Widget>>& children() override { return children_; }

private:
    EdgeInsets insets_;
    FlexParentData horizontalData_;
    FlexParentData verticalData_;
    std::vector<std::unique_ptr<Widget>> children_;
};

/**
 * @brief 居中转接组件：把孩子的交叉轴对齐改成居中。
 *
 * 对应 Flutter 的 Center（本质是 Align）。注意它不产生 View：需要
 * 背景色时外面再包一层 Container。
 */
class Center final : public ProxyWidget {
public:
    explicit Center(std::unique_ptr<Widget> child);
    FlexParentData flexParentData(Axis axis) const override;

private:
    FlexParentData horizontalData_;
    FlexParentData verticalData_;
};

/// 构造辅助：按 flex 系数瓜分剩余主轴空间。
std::unique_ptr<Widget> expanded(
    std::unique_ptr<Widget> child,
    float flex = 1.0f);
/// 构造辅助：固定尺寸盒子（宽/高传 -1 = 该方向不约束）。
std::unique_ptr<Widget> sizedBox(
    float width,
    float height,
    std::unique_ptr<Widget> child);
/// 构造辅助：内边距盒子。
std::unique_ptr<Widget> padding(
    EdgeInsets insets,
    std::unique_ptr<Widget> child);
/// 构造辅助：交叉轴居中。
std::unique_ptr<Widget> center(std::unique_ptr<Widget> child);

} // namespace evk::ui
