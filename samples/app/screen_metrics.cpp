/// @file screen_metrics.cpp
#include "screen_metrics.h"

#include <algorithm>

#include "evk/log.h"

/// ============================================================================
/// 设计像素换算实现。核心就是一个缩放比例：
///
///   scaleRatio = min(真实宽/设计宽, 真实高/设计高)
///
/// 布局尺寸 = 设计稿尺寸 × scaleRatio。横竖屏/折叠屏展开时
/// SurfaceChanged → appSetScreenSize 重算比例 → 根视图改 bounds →
/// Flex 级联重排，App 页面代码一行都不用改。
/// ============================================================================

float g_screenWidth = 0.0f;
float g_screenHeight = 0.0f;

namespace {

float g_scaleRatio = 0.0f;

} ///< namespace

void appSetScreenSize(float width, float height) {
    g_screenWidth = width;
    g_screenHeight = height;
    /// 取宽高两个比例的较小者：保证设计稿内容在任何比例屏幕上完整可见，
    /// 极端比例（如折叠屏展开的方屏）下内容等比缩小而不是被裁切。
    g_scaleRatio = std::min(width / kDesignWidth, height / kDesignHeight);
    EVK_LOGI("screen size: {:.0f}x{:.0f}, design scale ratio={:.4f}",
             width, height, g_scaleRatio);
}

float appCalcWidth(float designPixels) {
    return g_scaleRatio * designPixels;
}

float appCalcHeight(float designPixels) {
    return g_scaleRatio * designPixels;
}
