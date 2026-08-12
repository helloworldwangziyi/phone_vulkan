#include "evk/render_loop.h"

#include <atomic>

#include "evk/ui/animator.h"

namespace {

evk::FrameFunc g_frameFunc = nullptr;
std::atomic_bool g_framePending{false};

} // namespace

namespace evk {

void setFrameFunc(FrameFunc func) {
    g_frameFunc = func;
}

void requestRender() {
    g_framePending.store(true, std::memory_order_release);
}

bool beginFrame(int64_t frameTimeNanos) {
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
