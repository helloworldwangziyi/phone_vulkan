#pragma once

/// @file screen_metrics.h
/// ============================================================================
/// 设计像素换算：App 用「设计稿像素」写布局，运行期换算成「真实像素」。
///
/// 为什么需要：屏幕分辨率千差万别（1080p/2K/折叠屏展开……），如果直接写死
/// 像素，同一份布局在不同设备上大小不一。约定设计稿 1080×2339，所有
/// 布局尺寸写 appCalcWidth/Height(设计值)，运行期按屏幕等比缩放。
///
/// 换算规则：scaleRatio = min(屏宽/1080, 屏高/2339)（取较小者保证完整可见）。
/// 缩放比例由 SurfaceChanged 事件驱动更新（appSetScreenSize），
/// 之后 Flex 容器级联重排，页面无需响应 resize。
/// ============================================================================

constexpr float kDesignWidth = 1080.0f; ///< 设计稿宽（px）
constexpr float kDesignHeight = 2339.0f; ///< 设计稿高（px）

/// 当前 surface 真实像素尺寸（SurfaceChanged 时更新）。
extern float g_screenWidth;
extern float g_screenHeight;

/// 平台壳上报 surface 尺寸时调用：更新基准并重算缩放比例。
void appSetScreenSize(float width, float height);

/// 设计稿像素 → 真实像素。所有布局/绘制尺寸都应经过这两个函数。
float appCalcWidth(float designPixels);
float appCalcHeight(float designPixels);
