#pragma once

/**
 * @file flex_layout.h
 * @brief 线性布局控件（View 层）：Column/Row 对应的 FlexView 与排布参数。
 *
 * 排布规则（对照 Flutter 的 RenderFlex）：主轴空间先扣固定项
 * （mainSize >= 0）与前/后间距，剩余按 flex 系数分给弹性项（Expanded）；
 * 交叉轴按 crossSize 与 crossAlignment 摆放，默认拉伸铺满。参数全部来自
 * 孩子 Widget 的 flexParentData()——尺寸变化时 handleBoundsChanged 级联
 * 重排，App 不写布局函数。
 */

#include <cstdint>
#include <memory>

namespace evk::ui {

class View;

/// 主轴方向（Vertical = Column，Horizontal = Row）。
enum class Axis {
    Horizontal,
    Vertical,
};

/// 交叉轴对齐（Stretch = 铺满剩余交叉空间）。
enum class CrossAxisAlignment {
    Stretch,
    Start,
    Center,
    End,
};

/**
 * @brief 孩子向 Flex 父容器声明的排布参数（Flutter 的 parent data）。
 *
 * 由 Widget::flexParentData() 上报，经 setFlexChild 落到 FlexView；
 * 父容器不是 Flex 时不会被读取。
 */
struct FlexParentData {
    float mainSize = -1.0f;   ///< 主轴固定尺寸；< 0 = 不固定（交给 flex）
    float flex = 0.0f;        ///< 弹性系数：> 0 时按比例瓜分剩余主轴空间
    float crossSize = -1.0f;  ///< 交叉轴尺寸；< 0 时按对齐规则取（默认铺满）
    CrossAxisAlignment crossAlignment = CrossAxisAlignment::Stretch;
    float before = 0.0f;      ///< 主轴前间距
    float after = 0.0f;       ///< 主轴后间距
    float crossMargin = 0.0f; ///< 交叉轴两侧留白
};

/// 造一个 FlexView（仅首次 mount 时调用一次）。
std::unique_ptr<View> createFlexView(Axis axis);

/// 把孩子的排布参数写进 FlexView 并立即重排（RenderObjectWidget 的
/// configureChild 里调用）。
void setFlexChild(View& flex, View& child, const FlexParentData& data);

} // namespace evk::ui
