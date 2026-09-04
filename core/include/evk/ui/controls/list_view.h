#pragma once

/**
 * @file list_view.h
 * @brief 定高行列表组件（ListView）：双轴可滚，行高固定免测量。
 *
 * 对照 Flutter 的 ListView(itemExtent: ...)（Flutter 里 ListView 与
 * ScrollView 同文件，我们这里单列）。View 层复用滚动控件：
 * content 层按固定行高堆叠（见 view/scroll_control.h）。
 */

#include "evk/ui/widget_tree.h"

namespace evk::ui {

/**
 * @brief 定高行列表：双轴可滚（上下翻行；contentWidth 加宽后左右看更多列；
 *        单段手势按主方向锁轴，横竖互斥）。
 *
 * 对照 Flutter 的 ListView(itemExtent: ...)：行高固定免测量，第 i 行位置
 * = (0, i × itemHeight)。v1 为全量构建（对应 Flutter 的
 * ListView(children: [...]) 默认构造器，无懒加载/回收），行数大时请
 * 控制规模。滚动状态（偏移、惯性）存在 View 上，重建后保留；删除一行
 * 后其余行自动上移。
 */
class ListView final : public RenderObjectWidget {
public:
    float contentWidth = -1.0f;  ///< 内容宽度；-1 = 视口宽（横向不可滚）
    std::function<void(float, float)> onScroll;

    ListView(float itemHeight, std::vector<std::unique_ptr<Widget>> items);

    std::unique_ptr<View> createRenderObject() const override;
    void updateRenderObject(View& view) const override;
    std::vector<std::unique_ptr<Widget>>& children() override { return children_; }
    View* childParent(View& view) const override;

private:
    float itemHeight_;
    std::vector<std::unique_ptr<Widget>> children_;
};

/// 构造辅助：一行造一个列表。contentWidth 传 -1 = 视口宽（横向不可滚）。
std::unique_ptr<Widget> listView(
    float itemHeight,
    std::vector<std::unique_ptr<Widget>> items,
    float contentWidth = -1.0f);

} // namespace evk::ui
