/**
 * @file input.cpp
 * @brief 单手势状态机实现：平台触摸流（Down/Move/Up/Cancel）→ 点击回调与 pan 事件。
 */
#include "evk/ui/input.h"

#include <chrono>
#include <cmath>

#include "evk/esx_view.h"
#include "evk/ui/view.h"

namespace {

constexpr float kTouchSlop = 12.0f;

// ---- 滑动速度估计 ----

/**
 * @brief 滑动速度估计：维护最近约 100ms 的 (t, x, y) 样本（定长小数组，
 * 满了挤掉最老的），速度 = 窗口首尾样本位移 / 时间差。
 *
 * 窗口取 100ms：太短会被单帧抖动带偏，太长会把甩动前的慢速段平均进来。
 * 若抬起时刻距最后一个样本已超过一个窗口，视为"手指停住后才抬起"，速度归零
 * （否则慢滑停住再抬手也会被误判成甩动）。
 */
constexpr int64_t kVelocityWindowNanos = 100'000'000;
constexpr size_t kMaxMoveSamples = 8;

/// 单条移动采样（时间戳 + 屏幕坐标）。
struct MoveSample {
    int64_t t = 0;
    float x = 0.0f;
    float y = 0.0f;
};

/**
 * @brief 单手势状态机（同一时刻最多跟踪一根手指）。
 *
 * - active          —— 是否有进行中的手势（Down 置位，Up/Cancel 复位）；
 * - dragging        —— 是否已越过触控阈值转为滑动（pan 模式）；
 * - clickCancelled  —— 点击是否已取消（越过阈值或系统 Cancel）；
 * - target/panTarget—— Down 时锁定的输入目标与滑动目标句柄（手势期间不变，
 *                      保证归属稳定；0 表示无该目标）；
 * - downX/Y         —— DOWN 点（屏幕坐标），算总位移的基准；
 * - lastX/Y         —— 上次事件点（屏幕坐标），算逐次 delta；
 * - samples         —— 最近 100ms 的移动采样（速度估计用，见 velocityOf）。
 */
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

/// 全局单手势状态（见 PointerState）。
PointerState g_pointer;

/// 事件时间（纳秒）：优先平台时间戳，为 0 时以 steady_clock 兜底。
int64_t eventTimeNanos(const evk::ui::PointerEvent& event) {
    if (event.timestampNanos != 0) {
        return event.timestampNanos;
    }
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/// 追加移动样本：数组满了挤掉最老的，并丢弃速度窗口之前的旧样本。
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

/// 由窗口内首尾样本估计速度（px/s）；样本不足或手指已停住时为 0。
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

/**
 * @brief 从最深命中点沿父链向上找最近的"原始指针事件目标"：
 * 优先是声明了 acceptsPointerInput 的 SDK 控件（Button/ScrollView 自己跑
 * 状态机），其次是有 clickFunc 的普通视图（点击由输入层合成）。
 *
 * 沿父链向上是因为 hitTest 返回的是"最深命中节点"，而事件应归属到
 * 声明了能力的最近祖先——如点在按钮内部子结构上时事件归按钮。
 */
evk::ui::View* nearestInputTarget(evk::ui::View* hit) {
    for (evk::ui::View* view = hit; view; view = view->parent) {
        if (view->acceptsPointerInput() || view->clickFunc) {
            return view;
        }
    }
    return nullptr;
}

/**
 * @brief 同 nearestInputTarget：找最近的滑动目标
 * （ScrollView/Navigation/绑了 panFunc 的普通视图）。
 */
evk::ui::View* nearestPanTarget(evk::ui::View* hit) {
    for (evk::ui::View* view = hit; view; view = view->parent) {
        if (view->acceptsPanInput() || view->panFunc) {
            return view;
        }
    }
    return nullptr;
}

/**
 * @brief 把原始 Pointer 事件派发给目标视图的 handlePointer 虚函数。
 *
 * SDK 控件（Button/ScrollView）重写它跑自己的状态机；普通视图是空实现——
 * 普通视图的点击不走这里，由 Up 分支直接调 clickFunc 合成。
 */
void sendPointer(esx_view handle, const evk::ui::PointerEvent& event) {
    if (evk::ui::View* view = esxViewFromHandle(handle)) {
        view->handlePointer(event);
    }
}

/**
 * @brief 合成并派发 pan 事件，所有位置量换算成目标视图局部坐标：
 *
 * - x/y          —— 当前手指位置（相对视图左上角）；
 * - delta_x/y    —— 本次 Move 位移（屏幕差值，与坐标基准无关）；
 * - translation  —— DOWN 以来总位移（相对 DOWN 点，与视图坐标无关）；
 * - velocity_x/y —— 最近 100ms 滑动速度（px/s，屏幕值）。
 *
 * App 绑定的 panFunc 优先于控件 handlePan（SDK 控件内部不使用 panFunc 字段）。
 */
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

/// 复位全局手势状态。
void resetPointer() {
    g_pointer = PointerState{};
}

/// candidate 是否位于 root 的子树内（沿父链向上匹配，含 root 自身）。
bool viewIsInSubtree(evk::ui::View* candidate, evk::ui::View* root) {
    for (evk::ui::View* current = candidate; current; current = current->parent) {
        if (current == root) {
            return true;
        }
    }
    return false;
}

/// 活动手势的 target/panTarget 是否引用了 root 子树内的视图。
bool pointerReferencesSubtree(evk::ui::View* root) {
    return g_pointer.active && root &&
           (viewIsInSubtree(esxViewFromHandle(g_pointer.target), root) ||
            viewIsInSubtree(esxViewFromHandle(g_pointer.panTarget), root));
}

/**
 * @brief 手势收尾（Up/Cancel 共用）：先把状态快照出来再复位——
 * 复位必须在派发之前完成，否则回调里同步产生的下一个事件（如立即又 Down）
 * 会与旧状态冲突。
 *
 * 滑动中的手势收尾时补发 PAN_END/PAN_CANCEL（带松手速度）；
 * notifyTarget=true 时先给输入目标发最后一条 Pointer（Up/Cancel），
 * 让控件状态机收尾（如复位按下态）。
 */
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

/**
 * @brief UI 线程唯一的手势入口：把平台触摸流（Down/Move/Up/Cancel）转成
 * 点击回调与 pan 事件，全部逻辑收敛在单手势状态机 g_pointer 上。
 *
 * 整体模型（一次 Down 锁定一套目标，直到 Up/Cancel 收尾）：
 * - Down   → 命中测试 + 认领 input target 与 pan target（可能同一视图）；
 * - Move   → 累计位移，越过 12px 触控阈值即"判定为滑动"：input target 收
 *            Cancel、pan target 收 PAN_BEGIN/UPDATE（此后二者互斥，只走 pan）；
 * - Up     → 未滑动则合成点击（抬起点须仍在视图可见区域内）；
 *            滑动中则收尾 PAN_END（带松手速度）；
 * - Cancel → 控件复位按下态、pan 收 PAN_CANCEL（视图销毁/隐藏/禁用/多指等）。
 *
 * 坐标约定：event.x/y 是屏幕坐标；派发给回调前换算成目标视图局部坐标
 * （减 actualX/Y），回调里看到的永远是"相对自己左上角"。
 */
void dispatchPointerEvent(const PointerEvent& event) {
    if (event.action == PointerAction::Down) {
        // 多指：已有活动手势时先按 Cancel 收尾第一根手指
        // （单手势模型，同时只跟踪一根；Cancel 令控件复位按下态、pan 收尾回弹）。
        if (g_pointer.active) {
            const PointerEvent cancel{PointerAction::Cancel, g_pointer.pointerId,
                                      g_pointer.lastX, g_pointer.lastY};
            finishPointer(cancel, ESX_VIEW_PAN_CANCEL, true);
        }

        View* root = esxRootView();
        if (!root) {
            return;
        }
        // 先刷新屏幕坐标再命中测试：布局可能刚变更过，actual 必须是最新的。
        root->updateActuals();
        View* hit = root->hitTest(event.x, event.y);
        if (!hit) {
            return;
        }

        // 从最深命中点沿父链向上认领目标（Down 时锁定，整个手势不再变）：
        //   target    —— 原始指针事件目标（SDK 控件自己跑状态机；
        //                 普通视图由输入层在 Up 时合成点击）；
        //   panTarget —— 滑动目标（ScrollView/Navigation/绑了 pan 回调的视图）。
        View* target = nearestInputTarget(hit);
        View* panTarget = nearestPanTarget(hit);
        if (!target && !panTarget) {
            return; // 点在空白处：不跟踪这次手势
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

    const float dx = event.x - g_pointer.lastX; // 相对上次事件的位移（逐次 delta）
    const float dy = event.y - g_pointer.lastY;
    const float tx = event.x - g_pointer.downX; // 相对 DOWN 的总位移
    const float ty = event.y - g_pointer.downY;

    if (event.action == PointerAction::Move) {
        pushSample(g_pointer, eventTimeNanos(event), event.x, event.y);
        float velocityX = 0.0f;
        float velocityY = 0.0f;
        velocityOf(g_pointer, eventTimeNanos(event), &velocityX, &velocityY);
        if (!g_pointer.dragging && !g_pointer.clickCancelled &&
            std::sqrt(tx * tx + ty * ty) > kTouchSlop) {
            // 越过 12px 触控阈值：手势从"点击候选"正式转为"滑动"——
            // 1) 点击取消：target 收 Cancel（Button 复位按下态），之后不再收事件；
            // 2) 滑动开始：panTarget 收 PAN_BEGIN（首次 delta = DOWN 以来总位移）。
            // 点击与滑动自此互斥：本次手势剩余事件只走 pan。
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
            // 滑动进行中：持续派发 UPDATE（delta = 本次 Move 位移）。
            sendPan(g_pointer.panTarget, ESX_VIEW_PAN_UPDATE, event, dx, dy,
                    g_pointer.downX, g_pointer.downY, velocityX, velocityY);
        } else if (!g_pointer.clickCancelled) {
            // 未超阈值：Move 透传给 target（Button 据此做移入/移出按下态）。
            sendPointer(g_pointer.target, event);
        }
        g_pointer.lastX = event.x;
        g_pointer.lastY = event.y;
        return;
    }

    if (event.action == PointerAction::Up) {
        if (g_pointer.dragging) {
            // 滑动结束：带松手速度收尾（ScrollView 判 fling、Navigation 判补全返回）。
            finishPointer(event, ESX_VIEW_PAN_END, false);
        } else if (!g_pointer.clickCancelled) {
            // 点击合成（仅普通视图；acceptsPointerInput 的控件在 Up 里自己处理）：
            // 先发 Up 让控件状态机收尾，再检查"抬起点仍在视图可见区域内"
            // 才合成 click——手指按下后滑出再抬起不算点击。
            // click 坐标换算成相对视图左上角的局部坐标（见 esx_view.h）。
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
            // 已判为滑动但没有 pan 目标（如纯 clickFunc 视图上的滑动）：手势作废。
            resetPointer();
        }
        return;
    }

    if (event.action == PointerAction::Cancel) {
        // 系统级取消（视图销毁/隐藏/禁用/surface 失效/第二指按下）：
        // 控件复位按下态，pan 收 PAN_CANCEL（ScrollView 越界回弹、Navigation 回弹）。
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
