#pragma once

/**
 * @file scroll_control.h
 * @brief 滚动容器控件（View 层）：拖动跟手、橡皮筋越界、fling 惯性与回弹。
 *
 * 结构：ScrollView（视口，位置固定）内挂一层 content View，App 的子树
 * 挂在 content 下；滚动 = 把 content 平移到 (-offsetX, -offsetY)，视口外
 * 的部分由 View::paint 逐层收敛的 clip 天然裁掉，无需特判。横向与纵向
 * 都可滚动（content 可比视口更宽、更高）；一段拖动手势按主方向锁定
 * 单轴——横竖互斥，不斜向跟手。
 *
 * 滚动偏移是 View 的内部状态而非 Widget 配置，widget 重建后保留——
 * updateScrollView 只在内容尺寸变化时才收编越界，避免无条件钳制打断
 * 进行中的橡皮筋与 fling。
 */

#include <functional>
#include <memory>

namespace evk::ui {

class View;

/**
 * @brief 造一个滚动视口（仅首次 mount 时调用一次）。
 *
 * @param contentWidth  内容宽度；<= 视口宽时横向不可滚
 * @param contentHeight 内容高度；<= 视口高时纵向不可滚
 * @param onScroll      显示偏移变化回调（橡皮筋越界时是阻尼后的值）
 */
std::unique_ptr<View> createScrollView(
    float contentWidth,
    float contentHeight,
    std::function<void(float offsetX, float offsetY)> onScroll = {});

/// 取视口内 content 挂载点（ScrollViewWidget 的 childParent 靠它重定向）。
View* scrollContent(View& scrollView);

/// 同类型重建时应用新参数；仅内容尺寸变化时才收编越界偏移。
void updateScrollView(
    View& scrollView,
    float contentWidth,
    float contentHeight,
    std::function<void(float offsetX, float offsetY)> onScroll = {});

/**
 * @brief 造一个列表滚动视口：content 按固定行高纵向堆叠全部孩子。
 *
 * 行高固定（对照 Flutter ListView 的 itemExtent）：第 i 行位置
 * = (0, i × itemExtent)，免测量。contentWidth 可大于视口宽——横向与
 * 纵向同时可滚（行情表格：上下翻行、左右看更多列）。v1 为全量构建，
 * 无懒加载。
 */
std::unique_ptr<View> createListView(
    float itemExtent,
    float contentWidth,
    float contentHeight,
    std::function<void(float offsetX, float offsetY)> onScroll = {});

/// 同类型重建时应用列表参数（行高 / 内容尺寸 / 回调）。
void updateListView(
    View& listView,
    float itemExtent,
    float contentWidth,
    float contentHeight,
    std::function<void(float offsetX, float offsetY)> onScroll = {});

/// 代码设置滚动偏移（钳到界内并触发 onScroll）。
void setScrollOffset(View& scrollView, float x, float y);
/// 读当前显示偏移。
void getScrollOffset(const View& scrollView, float* x, float* y);

} // namespace evk::ui
