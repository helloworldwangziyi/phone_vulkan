/**
 * @file animator.cpp
 * @brief ui/animator：VSync 逐帧动画驱动。
 *
 * 驱动链路：平台 VSync → beginFrame → tickAnimations（先于 dirty 检查）。
 * tick 内修改视图（esx_view_set_bounds 等）会 requestRender() 置 dirty，
 * 当帧即被绘制；动画未结束则每帧都有变更，无需平台侧额外调度即自持续。
 *
 * 执行模型：tick 前先把动画列表整体搬走（swap），tick 期间新注册的动画
 * （如 fling 结束转 spring）留到下一帧执行，避免边遍历边修改容器。
 *
 * 清理契约：tick 返回 true 或 stopAllAnimations 时调用 cleanup 释放
 * userData；tick 内必须用 esxViewFromHandle 重解析句柄、用 weak_ptr
 * 持有控件状态，失效即返回 true 退场（控件无需析构钩子）。
 */

#include "evk/ui/animator.h"

#include <vector>

#include "evk/render_loop.h"

namespace {

/**
 * @brief 一个注册中的动画：函数指针对 + 用户数据。
 *
 * userData 是跨 C ABI 的 void*（如 ScrollAnimation/NavTransition 堆对象），
 * 所有权归动画条目：tick 返回 true 或 stopAllAnimations 时经 cleanup 释放。
 */
struct AnimationEntry {
    evk::ui::AnimationTick tick = nullptr; ///< 每帧回调；返回 true = 结束
    void* userData = nullptr; ///< tick/cleanup 共享的上下文
    evk::ui::AnimationCleanup cleanup = nullptr; ///< 结束时释放 userData（可空）
};

/// 动画注册表（vector：数量少，线性遍历足够）。
std::vector<AnimationEntry> g_animations;

/// 动画收尾：调用 cleanup 释放 userData（tick 返回 true 与 stopAllAnimations 共用）。
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

/**
 * @brief 缓动函数（t ∈ [0,1]，输出 ∈ [0,1]）：三次缓出，先快后慢——
 * 用于转场/回弹，到达终点前减速，视觉柔和。
 */
float easeOutCubic(float t) {
    const float u = 1.0f - t;
    return 1.0f - u * u * u;
}

/**
 * @brief 三次缓入缓出：两端慢中间快（0~0.5 与 0.5~1 两段对称）。
 */
float easeInOutCubic(float t) {
    if (t < 0.5f) {
        return 4.0f * t * t * t;
    }
    const float u = -2.0f * t + 2.0f;
    return 1.0f - u * u * u / 2.0f;
}

} // namespace evk::ui
