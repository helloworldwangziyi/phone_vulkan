/// @file app_images.cpp
#include "app_images.h"

#include <cmath>
#include <vector>

namespace {

constexpr uint32_t kBadgeSize = 128;
evk::ui::TextureId gBadge = evk::ui::kInvalidTexture;

float smoothstep(float e0, float e1, float x) {
    if (x <= e0) {
        return 0.0f;
    }
    if (x >= e1) {
        return 1.0f;
    }
    const float t = (x - e0) / (e1 - e0);
    return t * t * (3.0f - 2.0f * t);
}

/// 生成 128x128 径向渐变徽章：中心青色 → 边缘靛蓝，圆形软 alpha 边。
std::vector<uint32_t> makeBadgePixels() {
    std::vector<uint32_t> pixels(static_cast<size_t>(kBadgeSize) * kBadgeSize);
    const float center = (kBadgeSize - 1) * 0.5f;
    for (uint32_t y = 0; y < kBadgeSize; ++y) {
        for (uint32_t x = 0; x < kBadgeSize; ++x) {
            const float dx = static_cast<float>(x) - center;
            const float dy = static_cast<float>(y) - center;
            const float dist = std::sqrt(dx * dx + dy * dy) / center; ///< 0~1+
            // 颜色插值：青 (0x22D3EE) → 靛蓝 (0x6366F1)。
            const float t = dist < 1.0f ? dist : 1.0f;
            const float r = 0x22 / 255.0f + (0x63 - 0x22) / 255.0f * t;
            const float g = 0xD3 / 255.0f + (0x36 - 0xD3) / 255.0f * t;
            const float b = 0xEE / 255.0f + (0xF1 - 0xEE) / 255.0f * t;
            // 圆外 alpha 软衰减（0.85~1.0 过 1.0 截止）。
            float a = 1.0f - smoothstep(0.85f, 1.0f, dist);
            const auto toByte = [](float v) {
                const int i = static_cast<int>(v * 255.0f + 0.5f);
                return static_cast<uint32_t>(i < 0 ? 0 : (i > 255 ? 255 : i));
            };
            pixels[static_cast<size_t>(y) * kBadgeSize + x] =
                (toByte(r) << 24) | (toByte(g) << 16) | (toByte(b) << 8) | toByte(a);
        }
    }
    return pixels;
}

} // namespace

namespace appImages {

evk::ui::TextureId badge() {
    return gBadge;
}

void ensureRegistered() {
    if (gBadge != evk::ui::kInvalidTexture) {
        return;
    }
    const std::vector<uint32_t> pixels = makeBadgePixels();
    gBadge = evk::ui::TextureStore::instance().addTexture(kBadgeSize, kBadgeSize,
                                                          pixels.data());
}

} // namespace appImages
