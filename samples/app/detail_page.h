#pragma once

#include "evk/esx_view.h"

// 详情页：演示导航返回（导航栏返回按钮 / 左缘滑动返回）。
// 可同时存在多个实例（详情页可再 push 详情页）。
esx_view detailPageCreate(esx_view nav);

// SurfaceChanged 时重排所有存活实例。
void detailPagesLayout();

// Navigation 的 on_pop 回调：页面销毁前清理对应实例记录。
void detailPageOnPopped(esx_view page);

// 整树重建（如切换主题）时清空全部实例记录。
void detailPagesClear();
