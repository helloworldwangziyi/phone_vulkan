#include "evk/ui/pointer_input.h"

#include <chrono>
#include <cmath>

#include "evk/ui/render_view.h"

namespace {

constexpr float kTouchSlop = 12.0f;
constexpr int64_t kVelocityWindowNanos = 100'000'000;
constexpr size_t kMaxMoveSamples = 8;

struct MoveSample {
    int64_t time = 0;
    float x = 0.0f;
    float y = 0.0f;
};

struct PointerState {
    bool active = false;
    bool dragging = false;
    bool clickCancelled = false;
    int32_t pointerId = 0;
    evk::ui::ViewRef target;
    evk::ui::ViewRef panTarget;
    float downX = 0.0f;
    float downY = 0.0f;
    float lastX = 0.0f;
    float lastY = 0.0f;
    MoveSample samples[kMaxMoveSamples];
    size_t sampleCount = 0;
};

PointerState g_pointer;

int64_t eventTimeNanos(const evk::ui::PointerEvent& event) {
    if (event.timeNanos != 0) {
        return event.timeNanos;
    }
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void pushSample(PointerState& state, int64_t time, float x, float y) {
    if (state.sampleCount == kMaxMoveSamples) {
        for (size_t i = 1; i < state.sampleCount; ++i) {
            state.samples[i - 1] = state.samples[i];
        }
        --state.sampleCount;
    }
    state.samples[state.sampleCount++] = {time, x, y};
    while (state.sampleCount > 1 &&
           time - state.samples[0].time > kVelocityWindowNanos) {
        for (size_t i = 1; i < state.sampleCount; ++i) {
            state.samples[i - 1] = state.samples[i];
        }
        --state.sampleCount;
    }
}

void velocityOf(const PointerState& state, int64_t eventNanos,
                float* velocityX, float* velocityY) {
    *velocityX = 0.0f;
    *velocityY = 0.0f;
    if (state.sampleCount < 2) {
        return;
    }
    const MoveSample& last = state.samples[state.sampleCount - 1];
    if (eventNanos - last.time > kVelocityWindowNanos) {
        return;
    }
    const MoveSample& first = state.samples[0];
    const float dt = static_cast<float>(last.time - first.time) * 1e-9f;
    if (dt <= 0.0f) {
        return;
    }
    *velocityX = (last.x - first.x) / dt;
    *velocityY = (last.y - first.y) / dt;
}

evk::ui::View* nearestInputTarget(evk::ui::View* hit) {
    for (evk::ui::View* view = hit; view; view = view->parent) {
        if (view->acceptsPointerInput() || view->onClick) {
            return view;
        }
    }
    return nullptr;
}

evk::ui::View* nearestPanTarget(evk::ui::View* hit) {
    for (evk::ui::View* view = hit; view; view = view->parent) {
        if (view->acceptsPanInput()) {
            return view;
        }
    }
    return nullptr;
}

void sendPointer(const evk::ui::ViewRef& target,
                 const evk::ui::PointerEvent& event) {
    if (evk::ui::View* view = target.get()) {
        view->handlePointer(event);
    }
}

void sendPan(const evk::ui::ViewRef& target, evk::ui::PanState state,
             const evk::ui::PointerEvent& event, float dx, float dy,
             float downX, float downY, float velocityX, float velocityY) {
    evk::ui::View* view = target.get();
    if (!view) {
        return;
    }
    view->handlePan({
        state,
        event.x - view->actualX,
        event.y - view->actualY,
        dx,
        dy,
        event.x - downX,
        event.y - downY,
        velocityX,
        velocityY,
    });
}

void resetPointer() {
    g_pointer = PointerState{};
}

bool pointerReferencesSubtree(evk::ui::View* root) {
    if (!g_pointer.active || !root) {
        return false;
    }
    evk::ui::View* target = g_pointer.target.get();
    evk::ui::View* panTarget = g_pointer.panTarget.get();
    return (target && target->isDescendantOf(root)) ||
           (panTarget && panTarget->isDescendantOf(root));
}

void finishPointer(const evk::ui::PointerEvent& event,
                   evk::ui::PanState panState,
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
                                      g_pointer.lastX, g_pointer.lastY, event.timeNanos};
            finishPointer(cancel, PanState::Cancel, true);
        }

        View* root = rootView();
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
        g_pointer.target = ViewRef(target);
        g_pointer.panTarget = ViewRef(panTarget);
        g_pointer.downX = g_pointer.lastX = event.x;
        g_pointer.downY = g_pointer.lastY = event.y;
        pushSample(g_pointer, eventTimeNanos(event), event.x, event.y);
        sendPointer(g_pointer.target, event);
        return;
    }

    if (!g_pointer.active || event.pointerId != g_pointer.pointerId) {
        return;
    }

    if (View* root = rootView()) {
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
            if (g_pointer.target) {
                PointerEvent cancel = event;
                cancel.action = PointerAction::Cancel;
                sendPointer(g_pointer.target, cancel);
                g_pointer.target = ViewRef{};
            }
            if (g_pointer.panTarget) {
                g_pointer.dragging = true;
                sendPan(g_pointer.panTarget, PanState::Begin, event, tx, ty,
                        g_pointer.downX, g_pointer.downY, velocityX, velocityY);
            }
        } else if (g_pointer.dragging) {
            sendPan(g_pointer.panTarget, PanState::Update, event, dx, dy,
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
            finishPointer(event, PanState::End, false);
        } else if (!g_pointer.clickCancelled) {
            const PointerState finished = g_pointer;
            resetPointer();
            sendPointer(finished.target, event);
            View* target = finished.target.get();
            if (target && !target->acceptsPointerInput() && target->onClick &&
                target->containsVisiblePoint(event.x, event.y)) {
                const auto callback = target->onClick;
                callback({event.x - target->actualX, event.y - target->actualY});
            }
        } else {
            resetPointer();
        }
        return;
    }

    if (event.action == PointerAction::Cancel) {
        const PointerEvent cancel{PointerAction::Cancel, g_pointer.pointerId,
                                  g_pointer.lastX, g_pointer.lastY, event.timeNanos};
        finishPointer(cancel, PanState::Cancel, true);
    }
}

void discardPointerForView(View* view) {
    if (pointerReferencesSubtree(view)) {
        resetPointer();
    }
}

void cancelPointerForView(View* view) {
    if (!pointerReferencesSubtree(view)) {
        return;
    }
    const PointerEvent cancel{PointerAction::Cancel, g_pointer.pointerId,
                              g_pointer.lastX, g_pointer.lastY, 0};
    finishPointer(cancel, PanState::Cancel, true);
}

void cancelAllPointerEvents() {
    if (!g_pointer.active) {
        return;
    }
    const PointerEvent cancel{PointerAction::Cancel, g_pointer.pointerId,
                              g_pointer.lastX, g_pointer.lastY, 0};
    finishPointer(cancel, PanState::Cancel, true);
}

} // namespace evk::ui
