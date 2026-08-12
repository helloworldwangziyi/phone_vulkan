#pragma once

#include "evk/esx_view.h"

// 首页（导航栈根页面）：渐变面板 + 详情页/主题按钮 + ScrollView 演示。
// 返回 parent=0 创建的页面视图，由 Navigation 接管布局与生命周期。
esx_view homePageCreate(esx_view nav);

// SurfaceChanged 后按真实像素重排页面内容。
void homePageLayout();

// 页面视图由外部（Navigation/App）统一销毁，这里只清 App 侧记录。
void homePageDestroy();
