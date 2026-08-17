#pragma once

/// @file watchlist_page.h
/// 蓝湖图纸「2-1-750-自选-黑」的复刻页：深色自选行情列表。
///
/// 页面结构（对照图纸自上而下）：板块 Tab 条 → 列表表头（名称/最新/涨跌/
/// 涨跌幅/成交）→ 可滚动行情行 → Toast 槽 → 底部四 Tab。
/// 行情行用 painter 逐格绘制（数字列右对齐），后台线程每秒随机游走
/// 价格并 setState——正是「行情通知驱动列表刷新」的演示场景。
#include <memory>

#include "evk/ui/widget_tree.h"

class WatchlistPage final : public evk::ui::StatefulWidget {
public:
    std::unique_ptr<evk::ui::State> createState() const override;
};
