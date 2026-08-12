#include "app_metrics.h"

#include <algorithm>

#include "evk/log.h"

float g_screenWidth = 0.0f;
float g_screenHeight = 0.0f;

namespace {

float g_scaleRatio = 0.0f;

} // namespace

void appSetScreenSize(float width, float height) {
    g_screenWidth = width;
    g_screenHeight = height;
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
