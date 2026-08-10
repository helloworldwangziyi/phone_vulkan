#pragma once

// 全局状态与全局事件监听（参考 estarxapp 的 esx_global.c）。
// 全局监听的职责是把平台上报的量存成全局变量（真实像素尺寸、缩放比），
// 以及把原始触摸转发给 core 的点击合成 helper。

// 设计稿尺寸（与 estarxapp 的 DESIGN_WIDTH/DESIGN_HEIGHT 一致）。
// 视图坐标一律以设计像素书写，经 appCalcWidth/appCalcHeight 换算成
// 当前设备的真实像素，保证不同机型布局观感一致。
constexpr float kDesignWidth  = 1080.0f;
constexpr float kDesignHeight = 2339.0f;

// 真实 surface 像素尺寸，SurfaceChanged 全局监听负责更新
//（init 时该事件先于 AppStart 到达，AppStart 里即可使用）。
extern float g_screenWidth;
extern float g_screenHeight;

// 设计像素 → 真实像素。宽高共用同一缩放比（取两者中较小者，保持比例不变形）。
float appCalcWidth(float size);
float appCalcHeight(float size);
