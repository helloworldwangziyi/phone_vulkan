// ============================================================================
// ui/animator：VSync 逐帧动画驱动。
//
// 驱动链路：平台 VSync → beginFrame → tickAnimations（先于 dirty 检查）。
// tick 内修改视图（esx_view_set_bounds 等）会 requestRender() 置 dirty，
// 当帧即被绘制；动画未结束则每帧都有变更，无需平台侧额外调度即自持续。
//
// 执行模型：tick 前先把动画列表整体搬走（swap），tick 期间新注册的动画
// （如 fling 结束转 spring）留到下一帧执行，避免边遍历边修改容器。
//
// 清理契约：tick 返回 true 或 stopAllAnimations 时调用 cleanup 释放
// userData；tick 内必须用 esxViewFromHandle 重解析句柄、用 weak_ptr
// 持有控件状态，失效即返回 true 退场（控件无需析构钩子）。
// ============================================================================

#include "evk/ui/animator.h"

#include <vector>

#include "evk/render_loop.h"

namespace {

struct AnimationEntry {
    evk::ui::AnimationTick tick = nullptr;
    void* userData = nullptr;
    evk::ui::AnimationCleanup cleanup = nullptr;
};

std::vector<AnimationEntry> g_animations;

void finishAnimation(AnimationEntry& entry) {
    if (entry.cleanup) {
        entry.cleanup(entry.userData);
    }
}

} // namespace

namespace evk::ui {

void startAnimation(AnimationTick tick, void* userData, AnimationCleanup cleanup) {
    if (!tick) {
        return;
    }
    g_animations.push_back({tick, userData, cleanup});
    requestRender();
}

void tickAnimations(int64_t frameTimeNanos) {
    if (g_animations.empty()) {
        return;
    }
    // tick 内可能注册新动画（如 fling 结束转 spring），先搬走当前列表，
    // 新动画留到下一帧才执行。
    std::vector<AnimationEntry> running;
    running.swap(g_animations);
    for (auto& entry : running) {
        if (entry.tick(frameTimeNanos, entry.userData)) {
            finishAnimation(entry);
        } else {
            g_animations.push_back(entry);
        }
    }
}

void stopAllAnimations() {
    for (auto& entry : g_animations) {
        finishAnimation(entry);
    }
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
