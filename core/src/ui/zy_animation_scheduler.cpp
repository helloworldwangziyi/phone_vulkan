#include "evk/ui/zy_animation_scheduler.h"

#include <utility>
#include <vector>

#include "evk/zy_frame_scheduler.h"

namespace {

std::vector<evk::ui::AnimationTick> g_animations;

} // namespace

namespace evk::ui {

void startAnimation(AnimationTick tick) {
    if (!tick) {
        return;
    }
    g_animations.push_back(std::move(tick));
    requestRender();
}

void tickAnimations(int64_t frameTimeNanos) {
    std::vector<AnimationTick> running;
    running.swap(g_animations);
    for (auto& tick : running) {
        if (!tick(frameTimeNanos)) {
            g_animations.push_back(std::move(tick));
        }
    }
}

void stopAllAnimations() {
    g_animations.clear();
}

float easeOutCubic(float t) {
    const float u = 1.0f - t;
    return 1.0f - u * u * u;
}

float easeInOutCubic(float t) {
    if (t < 0.5f) {
        return 4.0f * t * t * t;
    }
    const float u = -2.0f * t + 2.0f;
    return 1.0f - u * u * u / 2.0f;
}

} // namespace evk::ui
