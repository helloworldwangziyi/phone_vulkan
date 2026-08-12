#pragma once

#include <stdint.h>

// App 自建颜色系统：一套语义化设计 token，深色/浅色双主题。
// 颜色均为 0xRRGGBBAA。控件与页面只引用 token，不写死色值。
struct AppTheme {
    uint32_t windowBackground;  // 窗口/页面底色
    uint32_t surface;           // 面板、卡片
    uint32_t surfaceRaised;     // 更亮一层的表面（次级按钮、返回按钮）
    uint32_t primary;           // 主按钮
    uint32_t primaryPressed;
    uint32_t primaryDisabled;
    uint32_t secondary;         // 次级按钮
    uint32_t secondaryPressed;
    uint32_t accent;            // 强调色
    uint32_t navBar;            // 导航栏背景
    uint32_t navBarLine;        // 导航栏底部分隔线
    uint32_t backArrow;         // 导航栏返回箭头
    uint32_t panelGradient[3];  // 面板渐变三角形三色
    uint32_t panelAccent;       // 点击切换后的强调顶点色
    uint32_t scrollItems[8];    // 列表项色阶（由深到浅）
};

// 当前主题（默认深色）。
const AppTheme& appTheme();

bool appThemeIsDark();

// 深/浅互切。只换 token 值，已创建的视图需由 App 重建或重刷。
void appThemeToggle();
