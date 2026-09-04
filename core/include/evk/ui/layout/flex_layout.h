#pragma once

/**
 * @file flex_layout.h
 * @brief 线性布局控件（View 层）：Column/Row 对应的 FlexView 与排布参数。
 *
 * 排布规则（对照 Flutter 的 RenderFlex，走约束协议）：非弹性孩子收
 * 无界主轴约束自报尺寸（显式 mainSize 是 tight 覆盖）；主轴有界时剩余
 * 空间按 flex 系数 tight 分给弹性项（Expanded）；交叉轴按 crossSize /
 * crossAlignment 给 tight 或松散约束，孩子尺寸上行后算偏移。参数来自
 * 孩子 Widget 的 flexParentData()——App 不写布局函数。
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
 * 父容器不是 Flex 时不会被读取。mainSize/crossSize 是显式尺寸覆盖
 * （tight 约束）；缺省（< 0）则孩子经约束协议自测尺寸。
 */
struct FlexParentData {
    float mainSize = -1.0f;   ///< 主轴显式尺寸；< 0 = 自测（或交给 flex）
    float flex = 0.0f;        ///< 弹性系数：> 0 时按比例瓜分剩余主轴空间
    float crossSize = -1.0f;  ///< 交叉轴显式尺寸；< 0 时按对齐规则取
    CrossAxisAlignment crossAlignment = CrossAxisAlignment::Stretch;
    float before = 0.0f;      ///< 主轴前间距
    float after = 0.0f;       ///< 主轴后间距
    float crossMargin = 0.0f; ///< 交叉轴两侧留白
};

/// 造一个 FlexView（仅首次 mount 时调用一次）。
std::unique_ptr<View> createFlexView(Axis axis);

/// 把孩子的排布参数写进 FlexView 并标注重排（RenderObjectWidget 的
/// configureChild 里调用）。
void setFlexChild(View& flex, View& child, const FlexParentData& data);

} // namespace evk::ui
