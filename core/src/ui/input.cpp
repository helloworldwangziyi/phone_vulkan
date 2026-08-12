#include "evk/ui/input.h"

#include <chrono>
#include <cmath>

#include "evk/esx_view.h"
#include "evk/ui/view.h"

namespace {

constexpr float kTouchSlop = 12.0f;

// ---- 滑动速度估计 ----
// 维护最近约 100ms 的 (t, x, y) 样本（定长小数组，满了挤掉最老的），
// 速度 = 窗口首尾样本位移 / 时间差。窗口取 100ms：太短会被单帧抖动带偏，
// 太长会把甩动前的慢速段平均进来。
// 若抬起时刻距最后一个样本已超过一个窗口，视为"手指停住后才抬起"，速度归零
// （否则慢滑停住再抬手也会被误判成甩动）。
constexpr int64_t kVelocityWindowNanos = 100'000'000;
constexpr size_t kMaxMoveSamples = 8;

struct MoveSample {
    int64_t t = 0;
    float x = 0.0f;
    float y = 0.0f;
};

struct PointerState {
    bool active = false;
    bool dragging = false;
    bool clickCancelled = false;
    int32_t pointerId = 0;
    esx_view target = 0;
    esx_view panTarget = 0;
    float downX = 0.0f;
    float downY = 0.0f;
    float lastX = 0.0f;
    float lastY = 0.0f;
    MoveSample samples[kMaxMoveSamples];
    size_t sampleCount = 0;
};

PointerState g_pointer;

int64_t eventTimeNanos(const evk::ui::PointerEvent& event) {
    if (event.timestampNanos != 0) {
        return event.timestampNanos;
    }
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void pushSample(PointerState& state, int64_t t, float x, float y) {
    if (state.sampleCount == kMaxMoveSamples) {
        for (size_t i = 1; i < state.sampleCount; ++i) {
            state.samples[i - 1] = state.samples[i];
        }
        state.sampleCount -= 1;
    }
    state.samples[state.sampleCount++] = {t, x, y};
    // 丢弃窗口之前的样本（至少保留 1 个）。
    while (state.sampleCount > 1 && t - state.samples[0].t > kVelocityWindowNanos) {
        for (size_t i = 1; i < state.sampleCount; ++i) {
            state.samples[i - 1] = state.samples[i];
        }
        state.sampleCount -= 1;
    }
}

// 由窗口内首尾样本估计速度（px/s）；样本不足或手指已停住时为 0。
void velocityOf(const PointerState& state, int64_t eventNanos,
                float* velocityX, float* velocityY) {
    *velocityX = 0.0f;
    *velocityY = 0.0f;
    if (state.sampleCount < 2) {
        return;
    }
    const MoveSample& last = state.samples[state.sampleCount - 1];
    if (eventNanos - last.t > kVelocityWindowNanos) {
        return; // 最后一个样本已是 100ms 前：抬起前手指已停住
    }
    const MoveSample& first = state.samples[0];
    const float dt = static_cast<float>(last.t - first.t) * 1e-9f;
    if (dt <= 0.0f) {
        return;
    }
    *velocityX = (last.x - first.x) / dt;
    *velocityY = (last.y - first.y) / dt;
}

evk::ui::View* nearestInputTarget(evk::ui::View* hit) {
    for (evk::ui::View* view = hit; view; view = view->parent) {
        if (view->acceptsPointerInput() || view->clickFunc) {
            return view;
        }
    }
    return nullptr;
}

evk::ui::View* nearestPanTarget(evk::ui::View* hit) {
    for (evk::ui::View* view = hit; view; view = view->parent) {
        if (view->acceptsPanInput() || view->panFunc) {
            return view;
        }
    }
    return nullptr;
}

void sendPointer(esx_view handle, const evk::ui::PointerEvent& event) {
    // 控件通过 handlePointer 虚函数接收；普通视图默认空实现，无需判断。
    if (evk::ui::View* view = esxViewFromHandle(handle)) {
        view->handlePointer(event);
    }
}

void sendPan(esx_view handle, esx_view_pan_state state,
             const evk::ui::PointerEvent& event, float dx, float dy,
             float downX, float downY, float velocityX, float velocityY) {
    evk::ui::View* view = esxViewFromHandle(handle);
    if (!view || (!view->acceptsPanInput() && !view->panFunc)) {
        return;
    }
    const esx_view_pan_event pan{
        state,
        event.x - view->actualX,
        event.y - view->actualY,
        dx,
        dy,
        event.x - downX,
        event.y - downY,
        velocityX,
        velocityY,
    };
    // App 绑定的 pan 回调优先；SDK 控件走 handlePan 虚函数。
    if (view->panFunc) {
        view->panFunc(handle, &pan, view->panUserData);
    } else {
        view->handlePan(pan);
    }
}

void resetPointer() {
    g_pointer = PointerState{};
}

bool viewIsInSubtree(evk::ui::View* candidate, evk::ui::View* root) {
    for (evk::ui::View* current = candidate; current; current = current->parent) {
        if (current == root) {
            return true;
        }
    }
    return false;
}

bool pointerReferencesSubtree(evk::ui::View* root) {
    return g_pointer.active && root &&
           (viewIsInSubtree(esxViewFromHandle(g_pointer.target), root) ||
            viewIsInSubtree(esxViewFromHandle(g_pointer.panTarget), root));
}

void finishPointer(const evk::ui::PointerEvent& event, esx_view_pan_state panState,
                   bool notifyTarget) {
    if (!g_pointer.active) {
        return;
    }

    const PointerState finished = g_pointer;
    resetPointer();

    if (notifyTarget) {
        sendPointer(finished.target, event);
    }
    if (finished.dragging) {
        float velocityX = 0.0f;
        float velocityY = 0.0f;
        velocityOf(finished, eventTimeNanos(event), &velocityX, &velocityY);
        sendPan(finished.panTarget, panState, event,
                event.x - finished.lastX, event.y - finished.lastY,
                finished.downX, finished.downY, velocityX, velocityY);
    }
}

} // namespace

namespace evk::ui {

void dispatchPointerEvent(const PointerEvent& event) {
    if (event.action == PointerAction::Down) {
        if (g_pointer.active) {
            const PointerEvent cancel{PointerAction::Cancel, g_pointer.pointerId,
                                      g_pointer.lastX, g_pointer.lastY};
            finishPointer(cancel, ESX_VIEW_PAN_CANCEL, true);
        }

        View* root = esxRootView();
        if (!root) {
            return;
        }
        root->updateActuals();
        View* hit = root->hitTest(event.x, event.y);
        if (!hit) {
            return;
        }

        View* target = nearestInputTarget(hit);
        View* panTarget = nearestPanTarget(hit);
        if (!target && !panTarget) {
            return;
        }

        g_pointer.active = true;
        g_pointer.pointerId = event.pointerId;
        g_pointer.target = target ? target->handle : 0;
        g_pointer.panTarget = panTarget ? panTarget->handle : 0;
        g_pointer.downX = g_pointer.lastX = event.x;
        g_pointer.downY = g_pointer.lastY = event.y;
        pushSample(g_pointer, eventTimeNanos(event), event.x, event.y);
        sendPointer(g_pointer.target, event);
        return;
    }

    if (!g_pointer.active || event.pointerId != g_pointer.pointerId) {
        return;
    }

    if (View* root = esxRootView()) {
        root->updateActuals();
    }

    const float dx = event.x - g_pointer.lastX;
    const float dy = event.y - g_pointer.lastY;
    const float tx = event.x - g_pointer.downX;
    const float ty = event.y - g_pointer.downY;

    if (event.action == PointerAction::Move) {
        pushSample(g_pointer, eventTimeNanos(event), event.x, event.y);
        float velocityX = 0.0f;
        float velocityY = 0.0f;
        velocityOf(g_pointer, eventTimeNanos(event), &velocityX, &velocityY);
        if (!g_pointer.dragging && !g_pointer.clickCancelled &&
            std::sqrt(tx * tx + ty * ty) > kTouchSlop) {
            g_pointer.clickCancelled = true;
            if (g_pointer.target != 0) {
                PointerEvent cancel = event;
                cancel.action = PointerAction::Cancel;
                sendPointer(g_pointer.target, cancel);
                g_pointer.target = 0;
            }
            if (g_pointer.panTarget != 0) {
                g_pointer.dragging = true;
                sendPan(g_pointer.panTarget, ESX_VIEW_PAN_BEGIN, event, tx, ty,
                        g_pointer.downX, g_pointer.downY, velocityX, velocityY);
            }
        } else if (g_pointer.dragging) {
            sendPan(g_pointer.panTarget, ESX_VIEW_PAN_UPDATE, event, dx, dy,
                    g_pointer.downX, g_pointer.downY, velocityX, velocityY);
        } else if (!g_pointer.clickCancelled) {
            sendPointer(g_pointer.target, event);
        }
        g_pointer.lastX = event.x;
        g_pointer.lastY = event.y;
        return;
    }

    if (event.action == PointerAction::Up) {
        if (g_pointer.dragging) {
            finishPointer(event, ESX_VIEW_PAN_END, false);
        } else if (!g_pointer.clickCancelled) {
            const PointerState finished = g_pointer;
            resetPointer();
            sendPointer(finished.target, event);
            View* target = esxViewFromHandle(finished.target);
            if (target && !target->acceptsPointerInput() && target->clickFunc &&
                target->containsVisiblePoint(event.x, event.y)) {
                const esx_view_click_event click{event.x - target->actualX,
                                                 event.y - target->actualY};
                target->clickFunc(target->handle, &click, target->clickUserData);
            }
        } else {
            resetPointer();
        }
        return;
    }

    if (event.action == PointerAction::Cancel) {
        const PointerEvent cancel{PointerAction::Cancel, g_pointer.pointerId,
                                  g_pointer.lastX, g_pointer.lastY};
        finishPointer(cancel, ESX_VIEW_PAN_CANCEL, true);
    }
}

void discardPointerForView(esx_view view) {
    if (pointerReferencesSubtree(esxViewFromHandle(view))) {
        resetPointer();
    }
}

void cancelPointerForView(esx_view view) {
    if (!pointerReferencesSubtree(esxViewFromHandle(view))) {
        return;
    }
    const PointerEvent cancel{PointerAction::Cancel, g_pointer.pointerId,
                              g_pointer.lastX, g_pointer.lastY};
    finishPointer(cancel, ESX_VIEW_PAN_CANCEL, true);
}

void cancelAllPointerEvents() {
    if (!g_pointer.active) {
        return;
    }
    PointerEvent cancel{PointerAction::Cancel, g_pointer.pointerId,
                        g_pointer.lastX, g_pointer.lastY};
    finishPointer(cancel, ESX_VIEW_PAN_CANCEL, true);
}

} // namespace evk::ui
