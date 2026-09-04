#pragma once

/**
 * @file basic.h
 * @brief 基础小件合集：EdgeInsets / Expanded / SizedBox / Padding / Center。
 *
 * 对照 Flutter 的 widgets/basic.dart——单个文件装不下框架、但每个类
 * 又只有几十行的小组件集中放这里。Expanded 是 ProxyWidget：把孩子的
 * flexParentData 抄一份、改几笔再上报（flex 系数）；SizedBox/Padding/
 * Center 是 RenderObjectWidget：尺寸、内边距与居中在 View 层的
 * performLayout 里经约束协议自我强制，无需向父容器上报。
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
 * 宽或高传 -1 表示该方向不锁死（有界撑满、无界包孩子）。尺寸在
 * View 层经约束协议自我强制（SizedBoxView）。
 */
class SizedBox final : public RenderObjectWidget {
public:
    SizedBox(float width, float height, std::unique_ptr<Widget> child);

    std::unique_ptr<View> createRenderObject() const override;
    void updateRenderObject(View& view) const override;
    std::vector<std::unique_ptr<Widget>>& children() override { return children_; }

private:
    float width_;
    float height_;
    std::vector<std::unique_ptr<Widget>> children_;
};

/**
 * @brief 内边距盒子：约束扣除 insets 后下行给孩子，自身尺寸 =
 *        孩子 + 内边距。
 */
class Padding final : public RenderObjectWidget {
public:
    Padding(EdgeInsets insets, std::unique_ptr<Widget> child);

    std::unique_ptr<View> createRenderObject() const override;
    void updateRenderObject(View& view) const override;
    std::vector<std::unique_ptr<Widget>>& children() override { return children_; }

private:
    EdgeInsets insets_;
    std::vector<std::unique_ptr<Widget>> children_;
};

/**
 * @brief 居中容器：松约束让孩子按内容自测，自身有界撑满、无界包孩子，
 *        孩子在自身范围内两轴居中。
 *
 * 对应 Flutter 的 Center（RenderPositionedBox）：关键是**松掉**下行的
 * tight 约束，孩子才不会被父级 min 值钳大——例如 tight 宽的 Padding
 * 里的 center(sizedBox(...))，盒子保持自身宽度并居中。
 */
class Center final : public RenderObjectWidget {
public:
    explicit Center(std::unique_ptr<Widget> child);

    std::unique_ptr<View> createRenderObject() const override;
    void updateRenderObject(View& view) const override;
    std::vector<std::unique_ptr<Widget>>& children() override { return children_; }

private:
    std::vector<std::unique_ptr<Widget>> children_;
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
