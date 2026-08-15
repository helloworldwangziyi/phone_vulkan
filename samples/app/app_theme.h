#pragma once

/// @file app_theme.h
#include <stdint.h>

/// ============================================================================
/// App 自建颜色系统：一套语义化设计 token，深色/浅色双主题。
///
/// 约定：
///   - 颜色统一 0xRRGGBBAA（红绿蓝 + 不透明度），与 esx_view_set_background /
///     esx_draw_* 的参数格式一致；
///   - 控件与页面只引用 token（theme.primary、theme.surface……），不写死色值：
///     换肤 = 换 token 值 + 让页面重跑 build()（重读 token），
///     reconcile 就地更新所有颜色。
///
/// 换肤链路（首页演示）：
///   themeButton.onTap
///     → appThemeToggle()                        /// 切 token 表
///     → esx_navigation_set_style(...)           /// 导航栏不属于页面，就地更新
///     → esx_event_emit(kEventThemeChanged)      /// 广播
///     → 各页面 listen 的 handler → setState()   /// 页面重跑 build() 换肤
/// ============================================================================

struct AppTheme {
    uint32_t windowBackground; ///< 窗口/页面底色
    uint32_t surface; ///< 面板、卡片
    uint32_t surfaceRaised; ///< 更亮一层的表面（次级按钮、返回按钮）
    uint32_t primary; ///< 主按钮
    uint32_t primaryPressed;
    uint32_t primaryDisabled;
    uint32_t secondary; ///< 次级按钮
    uint32_t secondaryPressed;
    uint32_t accent; ///< 强调色
    uint32_t navBar; ///< 导航栏背景
    uint32_t navBarLine; ///< 导航栏底部分隔线
    uint32_t backArrow; ///< 导航栏返回箭头
    uint32_t panelGradient[3]; ///< 面板渐变三角形三色
    uint32_t panelAccent; ///< 点击切换后的强调顶点色
    uint32_t scrollItems[8]; ///< 列表项色阶（由深到浅）
};

/// 当前主题（默认深色）。const 引用：换肤后指针不变、值已切换，
/// 页面只需在每次 build() 重新调用即可拿到新 token。
const AppTheme& appTheme();

bool appThemeIsDark();

/// 主题切换事件（事件总线 id）：appThemeToggle 后广播，
/// 页面 listen 该事件并 setState 即完成换肤。id 由 App 自定义（从 1 起），
/// 与 core 的 EventId（平台生命周期）互不冲突——那是另一条通道。
constexpr int32_t kEventThemeChanged = 1;

/// 深/浅互切。只换 token 值；页面在 build() 里读 token，setState 重建即生效。
void appThemeToggle();
