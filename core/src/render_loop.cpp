#include "evk/render_loop.h"

#include <atomic>

#include "evk/ui/animator.h"
#include "evk/ui/event_bus.h"

namespace {

evk::FrameFunc g_frameFunc = nullptr;
std::atomic_bool g_framePending{false};
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
    esxDrainUiTasks();
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
