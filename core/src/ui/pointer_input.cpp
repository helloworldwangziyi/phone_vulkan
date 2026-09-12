/**
 * @file pointer_input.cpp
 * @brief 指针输入调度：per-pointerId 多点触控状态表 + 手势识别
 *        （tap / double tap / long press / pan / scale）与竞技场仲裁。
 *
 * 每根手指一个独立状态机（g_pointers 按 pointerId 查找）：Down 命中测试
 * 选定 target（tap 系接收者）/ panTarget / scaleTarget，之后锁定到
 * Up/Cancel，不会因接收者中途进入动画状态而重新竞争。
 *
 * 竞技场规则（同一序列上先到先赢，输家收 Cancel）：
 * - pan：位移超触控阈值即认领；同一视图同一时间只允许一根手指的 pan
 *   生效（排他，避免双指同滚双倍速）。
 * - scale：同一 acceptsScaleInput 视图集齐双指即开会话，两指的 tap/pan
 *   认领全部作废；任一指抬起使剩余不足两指时收场，剩余指不追溯为 pan/tap。
 * - long press：按下 500ms 未超阈值即触发；触发只废 tap 系（tap /
 *   double tap），之后的移动仍可正常认领 pan（与 Flutter 一致）。
 * - double tap：目标设了 onDoubleTap 时，首次 tap 挂起等待第二击
 *   （300ms / 48px 内同目标）；超时或被别处按下打断则按普通单击结算。
 *
 * 计时器复用 animation_scheduler 的每帧 tick（tickAnimations 在
 * beginFrame 里无条件执行）；用户回调（onClick/onLongPress/handlePan…）
 * 都可能重建视图树或取消指针，故每次回调后一律按 pointerId 重新查找
 * 状态，不持有跨回调的迭代器/引用。
 */

#include "evk/ui/pointer_input.h"

#include <chrono>
#include <cmath>
#include <utility>
#include <vector>

#include "evk/frame_scheduler.h"
#include "evk/ui/animation_scheduler.h"
#include "evk/ui/render_view.h"

namespace {

constexpr float kTouchSlop = 12.0f;
constexpr int64_t kVelocityWindowNanos = 100'000'000;
constexpr size_t kMaxMoveSamples = 8;
/// 双击窗口：第一击 Up 后 300ms 内第二击落下（Flutter kDoubleTapTimeout）。
constexpr int64_t kDoubleTapTimeoutNanos = 300'000'000;
/// 双击两击落点允许的最大间距（px）。
constexpr float kDoubleTapMaxDistance = 48.0f;
/// 长按判定时长（Flutter kLongPressTimeout）。
constexpr int64_t kLongPressTimeoutNanos = 500'000'000;
/// 双指间距下限（px），防除零。
constexpr float kMinSpan = 1.0f;
constexpr float kPi = 3.14159265358979f;

struct MoveSample {
    int64_t time = 0;
    float x = 0.0f;
    float y = 0.0f;
};

struct PointerState {
    bool dragging = false;
    bool tapDead = false;          ///< tap 系（tap/doubleTap/longPress）已作废
    bool longPressFired = false;
    bool doubleTapSecond = false;  ///< 本序列是双击的第二击
    bool inScaleSession = false;
    int32_t pointerId = 0;
    int64_t downTimeNanos = 0;
    evk::ui::ViewRef target;
    evk::ui::ViewRef panTarget;
    evk::ui::ViewRef scaleTarget;
    float downX = 0.0f;
    float downY = 0.0f;
    float lastX = 0.0f;
    float lastY = 0.0f;
    MoveSample samples[kMaxMoveSamples];
    size_t sampleCount = 0;
};

/// 全部活跃指针的状态表。
std::vector<PointerState> g_pointers;

/// 双击等待中的首次单击：超时或被别处按下打断时按普通单击结算。
struct PendingTap {
    evk::ui::ViewRef target;
    float x = 0.0f;           ///< 首次 tap 的 Up 点（屏幕坐标）
    float y = 0.0f;
    int64_t deadline = 0;     ///< = 首次 Up 时刻 + kDoubleTapTimeoutNanos
};
bool g_pendingTapActive = false;
PendingTap g_pendingTap;

/// 进行中的双指缩放/旋转会话。
struct ScaleSession {
    evk::ui::ViewRef view;
    std::vector<int32_t> pointerIds;  ///< 入会顺序即几何优先级（取前两枚活跃成员）
    float lastSpan = 0.0f;
    float lastAngle = 0.0f;
    float scale = 1.0f;               ///< 相对 Begin 的累计缩放比
    float rotation = 0.0f;            ///< 相对 Begin 的累计旋转角（弧度）
};
std::vector<ScaleSession> g_scaleSessions;

/// 手势计时器（long press 截止 / double tap 结算）是否已挂进动画调度器。
bool g_gestureTimerRunning = false;

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
        if (view->acceptsPointerInput() || view->onClick || view->onDoubleTap ||
            view->onLongPress) {
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

evk::ui::View* nearestScaleTarget(evk::ui::View* hit) {
    for (evk::ui::View* view = hit; view; view = view->parent) {
        if (view->acceptsScaleInput()) {
            return view;
        }
    }
    return nullptr;
}

PointerState* findPointer(int32_t pointerId) {
    for (auto& state : g_pointers) {
        if (state.pointerId == pointerId) {
            return &state;
        }
    }
    return nullptr;
}

void erasePointer(int32_t pointerId) {
    for (auto it = g_pointers.begin(); it != g_pointers.end(); ++it) {
        if (it->pointerId == pointerId) {
            g_pointers.erase(it);
            return;
        }
    }
}

/// 同一视图是否已被其他手指的 pan 拖动（pan 排他判定）。
bool panClaimedByOther(evk::ui::View* view, int32_t selfId) {
    if (!view) {
        return false;
    }
    for (auto& state : g_pointers) {
        if (state.pointerId != selfId && state.dragging &&
            state.panTarget.get() == view) {
            return true;
        }
    }
    return false;
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

/// 按 id 收尾一条指针序列：先移除状态表项（回调安全），再发终态事件。
void finishPointer(int32_t pointerId, const evk::ui::PointerEvent& event,
                   evk::ui::PanState panState, bool notifyTarget) {
    PointerState* state = findPointer(pointerId);
    if (!state) {
        return;
    }
    const PointerState finished = *state;
    erasePointer(pointerId);

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

/// 挂起的单击按普通 tap 结算（双击超时 / 被别处按下打断）。
void resolvePendingTap() {
    if (!g_pendingTapActive) {
        return;
    }
    const PendingTap pending = g_pendingTap;
    g_pendingTapActive = false;
    evk::ui::View* view = pending.target.get();
    if (!view || view->acceptsPointerInput() || !view->onClick) {
        return;
    }
    // Up 时已验证界内，结算不再复查（与 Flutter 一致：tap 成立即生效）。
    const auto callback = view->onClick;
    callback({pending.x - view->actualX, pending.y - view->actualY});
}

// ---------------------------------------------------------------------------
// scale 会话
// ---------------------------------------------------------------------------

ScaleSession* findSessionByView(const evk::ui::View* view) {
    for (auto& session : g_scaleSessions) {
        if (session.view.get() == view) {
            return &session;
        }
    }
    return nullptr;
}

ScaleSession* findSessionByPointer(int32_t pointerId) {
    for (auto& session : g_scaleSessions) {
        for (int32_t id : session.pointerIds) {
            if (id == pointerId) {
                return &session;
            }
        }
    }
    return nullptr;
}

/// 会话几何：取最早两枚活跃成员，出间距、夹角与中点（屏幕坐标）。
bool sessionGeometry(const ScaleSession& session, float* span, float* angle,
                     float* focalX, float* focalY) {
    const PointerState* first = nullptr;
    const PointerState* second = nullptr;
    for (int32_t id : session.pointerIds) {
        const PointerState* state = findPointer(id);
        if (!state || !state->inScaleSession) {
            continue;
        }
        if (!first) {
            first = state;
        } else {
            second = state;
            break;
        }
    }
    if (!first || !second) {
        return false;
    }
    const float dx = second->lastX - first->lastX;
    const float dy = second->lastY - first->lastY;
    *span = std::sqrt(dx * dx + dy * dy);
    *angle = std::atan2(dy, dx);
    *focalX = (first->lastX + second->lastX) * 0.5f;
    *focalY = (first->lastY + second->lastY) * 0.5f;
    return true;
}

float normalizeAngle(float radians) {
    while (radians > kPi) {
        radians -= 2.0f * kPi;
    }
    while (radians < -kPi) {
        radians += 2.0f * kPi;
    }
    return radians;
}

void sendScale(ScaleSession& session, evk::ui::ScaleState state,
               float deltaScale, float deltaRotation,
               float focalX, float focalY, int32_t pointerCount) {
    evk::ui::View* view = session.view.get();
    if (!view) {
        return;
    }
    view->handleScale({state, focalX - view->actualX, focalY - view->actualY,
                       session.scale, session.rotation, deltaScale,
                       deltaRotation, pointerCount});
    evk::requestRender();
}

/// 开会话：竞技场 scale 胜，两指的 tap/pan 认领全部作废（发 Cancel）。
void startScaleSession(evk::ui::View* view, int32_t firstId, int32_t secondId,
                       int64_t now) {
    for (int32_t id : {firstId, secondId}) {
        PointerState* state = findPointer(id);
        if (!state) {
            continue;
        }
        state->tapDead = true;
        state->inScaleSession = true;
        const evk::ui::PointerEvent cancel{evk::ui::PointerAction::Cancel, id,
                                           state->lastX, state->lastY, now};
        if (state->target) {
            sendPointer(state->target, cancel);
            if (PointerState* self = findPointer(id)) {
                self->target = evk::ui::ViewRef{};
            }
        }
        if (PointerState* self = findPointer(id)) {
            if (self->dragging) {
                self->dragging = false;
                sendPan(self->panTarget, evk::ui::PanState::Cancel, cancel,
                        0.0f, 0.0f, self->downX, self->downY, 0.0f, 0.0f);
            }
        }
    }

    ScaleSession session;
    session.view = evk::ui::ViewRef(view);
    session.pointerIds = {firstId, secondId};
    g_scaleSessions.push_back(std::move(session));

    ScaleSession& current = g_scaleSessions.back();
    float span = 0.0f;
    float angle = 0.0f;
    float focalX = 0.0f;
    float focalY = 0.0f;
    if (sessionGeometry(current, &span, &angle, &focalX, &focalY)) {
        current.lastSpan = span;
        current.lastAngle = angle;
    }
    sendScale(current, evk::ui::ScaleState::Begin, 1.0f, 0.0f, focalX, focalY,
              static_cast<int32_t>(current.pointerIds.size()));
}

/// 第 3+ 指加入已有会话：只计 pointerCount，几何仍取最早两枚成员。
void joinScaleSession(ScaleSession& session, int32_t pointerId, int64_t now) {
    session.pointerIds.push_back(pointerId);
    if (PointerState* state = findPointer(pointerId)) {
        state->tapDead = true;
        state->inScaleSession = true;
        const evk::ui::PointerEvent cancel{
            evk::ui::PointerAction::Cancel, pointerId, state->lastX,
            state->lastY, now};
        if (state->target) {
            sendPointer(state->target, cancel);
            if (PointerState* self = findPointer(pointerId)) {
                self->target = evk::ui::ViewRef{};
            }
        }
    }
}

/// 会话成员移动：按最早两枚成员的几何增量累计 scale/rotation 并上报。
void updateScaleSession(const PointerState& state) {
    ScaleSession* session = findSessionByPointer(state.pointerId);
    if (!session) {
        if (PointerState* self = findPointer(state.pointerId)) {
            self->inScaleSession = false;
            self->tapDead = true;
        }
        return;
    }
    float span = 0.0f;
    float angle = 0.0f;
    float focalX = 0.0f;
    float focalY = 0.0f;
    if (!sessionGeometry(*session, &span, &angle, &focalX, &focalY)) {
        return;
    }
    // 间距退化（双指重合）时只跟进基线，不上报（防除零跳变）。
    if (span < kMinSpan || session->lastSpan < kMinSpan) {
        session->lastSpan = span;
        session->lastAngle = angle;
        return;
    }
    const float deltaScale = span / session->lastSpan;
    const float deltaRotation = normalizeAngle(angle - session->lastAngle);
    session->scale *= deltaScale;
    session->rotation += deltaRotation;
    session->lastSpan = span;
    session->lastAngle = angle;
    sendScale(*session, evk::ui::ScaleState::Update, deltaScale, deltaRotation,
              focalX, focalY, static_cast<int32_t>(session->pointerIds.size()));
}

/// 成员离场：剩余 ≥2 枚则重置几何基线继续；不足 2 枚发 End/Cancel 解散。
void leaveScaleSession(PointerState& state, bool cancelled) {
    ScaleSession* session = findSessionByPointer(state.pointerId);
    if (!session) {
        state.inScaleSession = false;
        state.tapDead = true;
        return;
    }
    // 收场几何含本指最后位置（本指此刻仍在会话标记内）。
    float span = 0.0f;
    float angle = 0.0f;
    float focalX = state.lastX;
    float focalY = state.lastY;
    sessionGeometry(*session, &span, &angle, &focalX, &focalY);

    for (auto it = session->pointerIds.begin(); it != session->pointerIds.end();
         ++it) {
        if (*it == state.pointerId) {
            session->pointerIds.erase(it);
            break;
        }
    }
    state.inScaleSession = false;
    state.tapDead = true;

    size_t remaining = 0;
    for (int32_t id : session->pointerIds) {
        if (PointerState* member = findPointer(id)) {
            if (member->inScaleSession) {
                ++remaining;
            }
        }
    }
    if (remaining >= 2) {
        // 几何对可能换人：基线重置为当前值，累计值不跳变。
        if (sessionGeometry(*session, &span, &angle, &focalX, &focalY)) {
            session->lastSpan = span;
            session->lastAngle = angle;
        }
        return;
    }

    const evk::ui::ViewRef view = session->view;
    const float totalScale = session->scale;
    const float totalRotation = session->rotation;
    for (int32_t id : session->pointerIds) {
        if (PointerState* member = findPointer(id)) {
            member->inScaleSession = false;
            member->tapDead = true;
        }
    }
    for (auto it = g_scaleSessions.begin(); it != g_scaleSessions.end(); ++it) {
        if (&*it == session) {
            g_scaleSessions.erase(it);
            break;
        }
    }
    if (evk::ui::View* target = view.get()) {
        target->handleScale({cancelled ? evk::ui::ScaleState::Cancel
                                       : evk::ui::ScaleState::End,
                             focalX - target->actualX, focalY - target->actualY,
                             totalScale, totalRotation, 1.0f, 0.0f,
                             static_cast<int32_t>(remaining)});
        evk::requestRender();
    }
}

// ---------------------------------------------------------------------------
// 手势计时器（long press 截止 / double tap 结算）
// ---------------------------------------------------------------------------

/// 每帧 tick：到期的 long press 触发、超时的挂起 tap 结算。
/// 返回 true 表示无事可做、从动画列表移除（与 startAnimation 契约一致）。
bool tickGestureTimer(int64_t now) {
    // 先就地标状态并收集到期项，回调一律在遍历后执行（回调可能重置指针表）。
    struct DueLongPress {
        evk::ui::ViewRef target;
        float x = 0.0f;
        float y = 0.0f;
    };
    std::vector<DueLongPress> due;
    for (auto& state : g_pointers) {
        if (state.tapDead || state.longPressFired || state.dragging ||
            state.inScaleSession) {
            continue;
        }
        evk::ui::View* target = state.target.get();
        if (!target || !target->onLongPress) {
            continue;
        }
        if (now - state.downTimeNanos < kLongPressTimeoutNanos) {
            continue;
        }
        state.longPressFired = true;
        state.tapDead = true;
        due.push_back({state.target, state.lastX, state.lastY});
        state.target = evk::ui::ViewRef{};
    }
    for (const DueLongPress& item : due) {
        evk::ui::View* target = item.target.get();
        if (!target || !target->onLongPress) {
            continue;
        }
        const auto callback = target->onLongPress;
        callback({item.x - target->actualX, item.y - target->actualY});
        evk::requestRender();
    }
    if (g_pendingTapActive && now >= g_pendingTap.deadline) {
        resolvePendingTap();
    }

    bool needed = g_pendingTapActive;
    if (!needed) {
        for (auto& state : g_pointers) {
            if (state.tapDead || state.longPressFired || state.dragging ||
                state.inScaleSession) {
                continue;
            }
            evk::ui::View* target = state.target.get();
            if (target && target->onLongPress) {
                needed = true;
                break;
            }
        }
    }
    if (!needed) {
        g_gestureTimerRunning = false;
    }
    return !needed;
}

void ensureGestureTimer() {
    if (g_gestureTimerRunning) {
        return;
    }
    g_gestureTimerRunning = true;
    evk::ui::startAnimation([](int64_t now) { return tickGestureTimer(now); });
}

/// 指针状态是否引用给定子树（target/panTarget/scaleTarget 任一）。
bool pointerReferencesSubtree(const PointerState& state, evk::ui::View* root) {
    if (!root) {
        return false;
    }
    evk::ui::View* target = state.target.get();
    evk::ui::View* panTarget = state.panTarget.get();
    evk::ui::View* scaleTarget = state.scaleTarget.get();
    return (target && target->isDescendantOf(root)) ||
           (panTarget && panTarget->isDescendantOf(root)) ||
           (scaleTarget && scaleTarget->isDescendantOf(root));
}

/// 解散引用给定子树的会话；silent=false 时向视图发 ScaleState::Cancel。
void endSessionsForView(evk::ui::View* view, bool silent) {
    for (auto it = g_scaleSessions.begin(); it != g_scaleSessions.end();) {
        evk::ui::View* sessionView = it->view.get();
        if (!sessionView || !sessionView->isDescendantOf(view)) {
            ++it;
            continue;
        }
        for (int32_t id : it->pointerIds) {
            if (PointerState* member = findPointer(id)) {
                member->inScaleSession = false;
                member->tapDead = true;
            }
        }
        const evk::ui::ViewRef target = it->view;
        const float totalScale = it->scale;
        const float totalRotation = it->rotation;
        it = g_scaleSessions.erase(it);
        if (!silent) {
            if (evk::ui::View* v = target.get()) {
                v->handleScale({evk::ui::ScaleState::Cancel, 0.0f, 0.0f,
                                totalScale, totalRotation, 1.0f, 0.0f, 0});
                evk::requestRender();
            }
        }
    }
}

} // namespace

namespace evk::ui {

void dispatchPointerEvent(const PointerEvent& event) {
    const int64_t now = eventTimeNanos(event);

    if (event.action == PointerAction::Down) {
        // 平台复用 pointerId：同 id 残留态先按 Cancel 收尾。
        if (findPointer(event.pointerId)) {
            const PointerEvent cancel{PointerAction::Cancel, event.pointerId,
                                      event.x, event.y, now};
            finishPointer(event.pointerId, cancel, PanState::Cancel, true);
        }

        View* root = rootView();
        if (!root) {
            return;
        }
        root->updateActuals();
        View* hit = root->hitTest(event.x, event.y);
        if (!hit) {
            // 落在空白区的新按下视为别处手势：打断双击等待。
            resolvePendingTap();
            return;
        }
        View* target = nearestInputTarget(hit);
        View* panTarget = nearestPanTarget(hit);
        View* scaleTarget = nearestScaleTarget(hit);
        if (!target && !panTarget && !scaleTarget) {
            resolvePendingTap();
            return;
        }

        // 双击匹配：同目标、时限内、两击落点间距内 → 本序列记为第二击；
        // 否则挂起的单击立即按普通 tap 结算（竞技场输家退场）。
        bool doubleTapSecond = false;
        if (g_pendingTapActive) {
            const float ddx = event.x - g_pendingTap.x;
            const float ddy = event.y - g_pendingTap.y;
            const bool match =
                target && g_pendingTap.target.get() == target &&
                now <= g_pendingTap.deadline &&
                ddx * ddx + ddy * ddy <=
                    kDoubleTapMaxDistance * kDoubleTapMaxDistance;
            if (match) {
                doubleTapSecond = true;
                g_pendingTapActive = false;
            } else {
                resolvePendingTap();
            }
        }

        PointerState state;
        state.pointerId = event.pointerId;
        state.downTimeNanos = now;
        state.target = ViewRef(target);
        state.panTarget = ViewRef(panTarget);
        state.scaleTarget = ViewRef(scaleTarget);
        state.doubleTapSecond = doubleTapSecond;
        state.downX = state.lastX = event.x;
        state.downY = state.lastY = event.y;
        pushSample(state, now, event.x, event.y);
        g_pointers.push_back(state);

        sendPointer(state.target, event);
        // Down 回调（ButtonView::handlePointer 等）之后不得再用本帧早些时候
        // 拿到的裸指针：回调里可能已重建视图树。一律按 id 重查 + ViewRef 判活。
        PointerState* self = findPointer(event.pointerId);
        if (!self) {
            return;
        }
        View* liveTarget = self->target.get();
        if (liveTarget && liveTarget->onLongPress) {
            ensureGestureTimer();
        }

        // scale 会话：同视图已有会话 → 加入（第 3+ 指）；否则若有另一枚
        // 活跃指盯着同一 scaleTarget → 两指集齐，开会话。
        View* liveScaleTarget = self->scaleTarget.get();
        if (liveScaleTarget) {
            if (ScaleSession* existing = findSessionByView(liveScaleTarget)) {
                joinScaleSession(*existing, event.pointerId, now);
            } else {
                for (auto& other : g_pointers) {
                    if (other.pointerId != event.pointerId &&
                        !other.inScaleSession &&
                        other.scaleTarget.get() == liveScaleTarget) {
                        startScaleSession(liveScaleTarget, other.pointerId,
                                          event.pointerId, now);
                        break;
                    }
                }
            }
        }
        return;
    }

    PointerState* state = findPointer(event.pointerId);
    if (!state) {
        return;
    }

    if (View* root = rootView()) {
        root->updateActuals();
    }

    // scale 会话成员：位置更新后进几何，tap/pan 通道全部关闭。
    if (state->inScaleSession) {
        state->lastX = event.x;
        state->lastY = event.y;
        if (event.action == PointerAction::Move) {
            updateScaleSession(*state);
        } else {
            leaveScaleSession(*state, event.action == PointerAction::Cancel);
            erasePointer(event.pointerId);
        }
        return;
    }

    if (event.action == PointerAction::Move) {
        pushSample(*state, now, event.x, event.y);
        float velocityX = 0.0f;
        float velocityY = 0.0f;
        velocityOf(*state, now, &velocityX, &velocityY);
        const float dx = event.x - state->lastX;
        const float dy = event.y - state->lastY;
        const float tx = event.x - state->downX;
        const float ty = event.y - state->downY;

        // 超阈值即尝试 pan 认领：即使 tap 系已作废（如 long press 已
        // 触发、或因排他被拒）也允许后续移动转为拖动——Flutter 里长按
        // 不锁死滚动；排他被拒的手指在赢家抬指后可接过拖动。
        if (!state->dragging &&
            std::sqrt(tx * tx + ty * ty) > kTouchSlop) {
            // tap 系作废（发 Cancel 复位按压视觉），只处理一次。
            if (!state->tapDead) {
                state->tapDead = true;
                if (state->target) {
                    PointerEvent cancel = event;
                    cancel.action = PointerAction::Cancel;
                    sendPointer(state->target, cancel);
                    if (PointerState* self = findPointer(event.pointerId)) {
                        self->target = ViewRef{};
                    }
                }
            }
            if (PointerState* self = findPointer(event.pointerId)) {
                View* panView = self->panTarget.get();
                if (panView && !panClaimedByOther(panView, event.pointerId)) {
                    self->dragging = true;
                    sendPan(self->panTarget, PanState::Begin, event, tx, ty,
                            self->downX, self->downY, velocityX, velocityY);
                }
            }
        } else if (state->dragging) {
            sendPan(state->panTarget, PanState::Update, event, dx, dy,
                    state->downX, state->downY, velocityX, velocityY);
        } else if (!state->tapDead) {
            sendPointer(state->target, event);
        }
        if (PointerState* self = findPointer(event.pointerId)) {
            self->lastX = event.x;
            self->lastY = event.y;
        }
        return;
    }

    if (event.action == PointerAction::Up) {
        if (state->dragging) {
            finishPointer(event.pointerId, event, PanState::End, false);
            return;
        }
        if (!state->tapDead) {
            // 先移除状态表项再回调：回调内可自由重建/取消（Flutter 的
            // arena sweep 同样在收场后派发）。
            const ViewRef target = state->target;
            const bool second = state->doubleTapSecond;
            erasePointer(event.pointerId);
            sendPointer(target, event);
            View* view = target.get();
            if (!view) {
                return;
            }
            const float localX = event.x - view->actualX;
            const float localY = event.y - view->actualY;
            if (second) {
                // 双击第二击成立：触发 onDoubleTap，首次挂起 tap 已作废。
                if (view->onDoubleTap) {
                    const auto callback = view->onDoubleTap;
                    callback({localX, localY});
                }
                return;
            }
            if (!view->acceptsPointerInput() && view->onClick &&
                view->containsVisiblePoint(event.x, event.y)) {
                if (view->onDoubleTap) {
                    // 目标关心双击：单击挂起，等待第二击或超时结算。
                    g_pendingTap.target = target;
                    g_pendingTap.x = event.x;
                    g_pendingTap.y = event.y;
                    g_pendingTap.deadline = now + kDoubleTapTimeoutNanos;
                    g_pendingTapActive = true;
                    ensureGestureTimer();
                } else {
                    const auto callback = view->onClick;
                    callback({localX, localY});
                }
            }
            return;
        }
        erasePointer(event.pointerId);
        return;
    }

    if (event.action == PointerAction::Cancel) {
        const PointerEvent cancel{PointerAction::Cancel, state->pointerId,
                                  state->lastX, state->lastY, now};
        finishPointer(event.pointerId, cancel, PanState::Cancel, true);
    }
}

void discardPointerForView(View* view) {
    if (!view) {
        return;
    }
    // 析构路径：静默丢弃，不向垂死视图回发事件。
    if (g_pendingTapActive) {
        View* pending = g_pendingTap.target.get();
        if (pending && pending->isDescendantOf(view)) {
            g_pendingTapActive = false;
        }
    }
    endSessionsForView(view, true);
    for (auto it = g_pointers.begin(); it != g_pointers.end();) {
        if (pointerReferencesSubtree(*it, view)) {
            it = g_pointers.erase(it);
        } else {
            ++it;
        }
    }
}

void cancelPointerForView(View* view) {
    if (!view) {
        return;
    }
    if (g_pendingTapActive) {
        View* pending = g_pendingTap.target.get();
        if (pending && pending->isDescendantOf(view)) {
            g_pendingTapActive = false;
        }
    }
    endSessionsForView(view, false);
    std::vector<int32_t> ids;
    for (auto& state : g_pointers) {
        if (pointerReferencesSubtree(state, view)) {
            ids.push_back(state.pointerId);
        }
    }
    for (int32_t id : ids) {
        if (PointerState* state = findPointer(id)) {
            const PointerEvent cancel{PointerAction::Cancel, id, state->lastX,
                                      state->lastY, 0};
            finishPointer(id, cancel, PanState::Cancel, true);
        }
    }
}

void cancelAllPointerEvents() {
    g_pendingTapActive = false;
    while (!g_scaleSessions.empty()) {
        endSessionsForView(g_scaleSessions.back().view.get(), false);
        // 会话视图已死（ViewRef 为空）时 endSessionsForView 不处理，兜底直删。
        if (!g_scaleSessions.empty() &&
            !g_scaleSessions.back().view.get()) {
            for (int32_t id : g_scaleSessions.back().pointerIds) {
                if (PointerState* member = findPointer(id)) {
                    member->inScaleSession = false;
                    member->tapDead = true;
                }
            }
            g_scaleSessions.pop_back();
        }
    }
    while (!g_pointers.empty()) {
        const PointerState back = g_pointers.back();
        const PointerEvent cancel{PointerAction::Cancel, back.pointerId,
                                  back.lastX, back.lastY, 0};
        finishPointer(back.pointerId, cancel, PanState::Cancel, true);
    }
    // 与 stopAllAnimations 的调用约定配对（shutdownApp/壳层销毁），
    // 允许下一次 ensureGestureTimer 重新挂 tick；残留的已挂 tick 幂等无害。
    g_gestureTimerRunning = false;
}

bool isPanGestureActive(View* view) {
    for (auto& state : g_pointers) {
        if (state.dragging && state.panTarget.get() == view) {
            return true;
        }
    }
    return false;
}

} // namespace evk::ui
