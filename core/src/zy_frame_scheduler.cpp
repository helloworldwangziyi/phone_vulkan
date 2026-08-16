/**
 * @file zy_frame_scheduler.cpp
 * @brief 按需渲染循环实现：dirty 标志 + VSync 驱动的帧调度。
 */
#include "evk/zy_frame_scheduler.h"

#include <atomic>

#include "evk/ui/zy_animation_scheduler.h"
#include "evk/ui/zy_event_bus.h"

namespace {

/// 平台壳注册的帧绘制实现。
evk::FrameFunc g_frameFunc = nullptr;
/// 下一次 VSync 待绘制标志（dirty）。
std::atomic_bool g_framePending{false};
/// 引擎就绪标志（跨线程读写，release/acquire 语义）。
std::atomic_bool g_engineReady{false};

} // namespace

namespace evk {

void setFrameFunc(FrameFunc func) {
    g_frameFunc = func;
}

void setEngineReady(bool ready) {
    g_engineReady.store(ready, std::memory_order_release);
}

bool engineReady() {
    return g_engineReady.load(std::memory_order_acquire);
}

void requestRender() {
    g_framePending.store(true, std::memory_order_release);
}

bool beginFrame(int64_t frameTimeNanos) {
    // 跨线程任务先执行：任务内可能改视图、启动动画、置 dirty，当帧即可体现。
    ui::drainUiTasks();
    // 动画先走：tick 内修改视图会置 dirty，当帧即可绘制。
    ui::tickAnimations(frameTimeNanos);
    if (!g_frameFunc || !g_framePending.exchange(false, std::memory_order_acq_rel)) {
        return false;
    }
    g_frameFunc(frameTimeNanos);
    return true;
}

void cancelPendingFrame() {
    g_framePending.store(false, std::memory_order_release);
}

} // namespace evk
