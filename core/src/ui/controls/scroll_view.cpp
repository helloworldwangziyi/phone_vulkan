// ============================================================================
// ScrollView：可滚动容器控件（viewport + content 双层结构）。
//
// offset 双轨模型：
//   rawX/rawY       —— 手势累计的目标 offset，拖拽期间不做 clamp，
//                      允许越界（越界量供橡皮筋显示）；
//   offsetX/offsetY —— 实际显示的 offset，越界部分按 kRubberBand 阻尼折算，
//                      content 视图的位置永远由它决定。
//
// 方向门禁：只有可滚动（maxOffset > 0）的轴参与拖动/惯性/橡皮筋，
// 不可滚动的轴全程冻结——否则竖滑时的水平抖动会被误判成越界，
// 把 fling 堵死（真机回归，见 tests 的 VerticalOnlyIgnoresHorizontal）。
//
// 松手后的动画状态机（复用 ui/animator 逐帧驱动）：
//   越界          → spring：easeOutCubic 从当前显示位置弹回 clamp 边界；
//   未越界且有速度 → fling：raw 按匀减速积分，冲出边界时转 spring；
//   手指按下（pointer Down 或再次拖动）/ 程序 set_offset / 尺寸变化
//                 → 立即打断动画，从当前显示位置接管。
//
// 生命周期：动画上下文（ScrollAnimation）存 state 的 weak_ptr + 视图句柄，
// tick 时重新解析；视图销毁或 animating 标志被外部复位都会让 tick
// 返回 true 并触发 cleanup 释放上下文。
// ============================================================================

#include "evk/ui/controls/scroll_view.h"

#include <algorithm>
#include <cmath>
#include <memory>

#include "evk/render_loop.h"
#include "evk/ui/animator.h"
#include "evk/ui/view.h"

namespace {

const char kScrollViewType = 0;

constexpr float kRubberBand = 0.45f;          // 越界拖拽阻尼
constexpr float kFlingMinVelocity = 50.0f;    // 触发惯性的最小速度 px/s
constexpr float kFlingStopVelocity = 30.0f;   // 惯性停止阈值 px/s
constexpr float kFlingDeceleration = 2400.0f; // 惯性减速度 px/s²
constexpr int64_t kSpringDurationNanos = 250'000'000;

struct ScrollViewState {
    esx_view content = 0;
    float contentWidth = 0.0f;
    float contentHeight = 0.0f;
    float rawX = 0.0f;    // 拖拽累计 offset（越界部分未阻尼）
    float rawY = 0.0f;
    float offsetX = 0.0f; // 实际显示 offset（越界部分已阻尼）
    float offsetY = 0.0f;
    esx_scroll_func onScroll = nullptr;
    void* userData = nullptr;
    bool animating = false; // fling/spring 进行中；手势 BEGIN 时置回 false 打断
    std::weak_ptr<ScrollViewState> self;
};

struct ScrollAnimation {
    std::weak_ptr<ScrollViewState> state;
    esx_view viewport = 0;
    bool spring = false;
    // fling 状态：内容 offset 速度（px/s，与手指速度方向相反）。
    float velocityX = 0.0f;
    float velocityY = 0.0f;
    int64_t lastTime = 0;
    // spring 状态。
    float fromX = 0.0f;
    float fromY = 0.0f;
    float toX = 0.0f;
    float toY = 0.0f;
    int64_t startTime = 0;
};

std::shared_ptr<ScrollViewState> scrollState(esx_view scrollView) {
    evk::ui::View* view = esxViewFromHandle(scrollView);
    if (!view || view->controlType != &kScrollViewType) {
        return nullptr;
    }
    return std::static_pointer_cast<ScrollViewState>(view->controlState);
}

float maxOffsetX(const ScrollViewState& state, const evk::ui::View* viewport) {
    return std::max(0.0f, state.contentWidth - viewport->rect.w);
}

float maxOffsetY(const ScrollViewState& state, const evk::ui::View* viewport) {
    return std::max(0.0f, state.contentHeight - viewport->rect.h);
}

// 越界部分按阻尼折算后的显示 offset：范围内原样，越界量 ×kRubberBand，
// 形成"越拉越费力"的橡皮筋手感（iOS 同款简化模型）。
float dampedOffset(float offset, float max) {
    if (offset < 0.0f) {
        return offset * kRubberBand;
    }
    if (offset > max) {
        return max + (offset - max) * kRubberBand;
    }
    return offset;
}

void setDisplayedOffset(esx_view scrollView, ScrollViewState& state,
                        float offsetX, float offsetY, bool notify) {
    evk::ui::View* content = esxViewFromHandle(state.content);
    if (!content) {
        return;
    }
    state.offsetX = offsetX;
    state.offsetY = offsetY;
    content->rect = {-state.offsetX, -state.offsetY,
                     state.contentWidth, state.contentHeight};
    evk::requestRender();
    if (notify && state.onScroll) {
        state.onScroll(scrollView, state.offsetX, state.offsetY, state.userData);
    }
}

// 拖拽路径：raw 不过 clamp，显示值越界部分加阻尼（橡皮筋）。
void applyRawOffset(esx_view scrollView, ScrollViewState& state, bool notify) {
    evk::ui::View* viewport = esxViewFromHandle(scrollView);
    if (!viewport) {
        return;
    }
    setDisplayedOffset(scrollView, state,
                       dampedOffset(state.rawX, maxOffsetX(state, viewport)),
                       dampedOffset(state.rawY, maxOffsetY(state, viewport)),
                       notify);
}

// 程序设定/尺寸变化路径：clamp 并吸附，同时打断进行中的动画。
void snapOffset(esx_view scrollView, ScrollViewState& state, bool notify) {
    evk::ui::View* viewport = esxViewFromHandle(scrollView);
    if (!viewport) {
        return;
    }
    state.animating = false;
    state.rawX = std::clamp(state.rawX, 0.0f, maxOffsetX(state, viewport));
    state.rawY = std::clamp(state.rawY, 0.0f, maxOffsetY(state, viewport));
    setDisplayedOffset(scrollView, state, state.rawX, state.rawY, notify);
}

// 转入回弹：从当前显示位置（含阻尼过冲）弹回 clamp 边界。
void enterSpring(ScrollAnimation* anim, ScrollViewState& state,
                 evk::ui::View* viewport) {
    anim->spring = true;
    anim->fromX = state.offsetX;
    anim->fromY = state.offsetY;
    anim->toX = std::clamp(state.rawX, 0.0f, maxOffsetX(state, viewport));
    anim->toY = std::clamp(state.rawY, 0.0f, maxOffsetY(state, viewport));
    state.rawX = anim->toX;
    state.rawY = anim->toY;
    anim->startTime = 0; // 首帧再记录，避开注册到下一帧的间隔
}

// 匀减速：每帧从速度里扣掉 loss（= kFlingDeceleration × dt），过零即停。
float decayVelocity(float velocity, float loss) {
    if (velocity > 0.0f) {
        return std::max(0.0f, velocity - loss);
    }
    return std::min(0.0f, velocity + loss);
}

bool scrollAnimationTick(int64_t frameTimeNanos, void* userData) {
    auto* anim = static_cast<ScrollAnimation*>(userData);
    std::shared_ptr<ScrollViewState> state = anim->state.lock();
    evk::ui::View* viewport = esxViewFromHandle(anim->viewport);
    if (!state || !viewport || !state->animating) {
        return true; // 视图销毁或手势/程序已接管
    }

    if (anim->spring) {
        if (anim->startTime == 0) {
            anim->startTime = frameTimeNanos;
        }
        const float t = static_cast<float>(frameTimeNanos - anim->startTime) /
                        static_cast<float>(kSpringDurationNanos);
        if (t >= 1.0f) {
            setDisplayedOffset(anim->viewport, *state, anim->toX, anim->toY, true);
            state->animating = false;
            return true;
        }
        const float e = evk::ui::easeOutCubic(std::clamp(t, 0.0f, 1.0f));
        setDisplayedOffset(anim->viewport, *state,
                           anim->fromX + (anim->toX - anim->fromX) * e,
                           anim->fromY + (anim->toY - anim->fromY) * e, true);
        return false;
    }

    // fling：首帧只记时间，拿不到可靠的帧间隔。
    if (anim->lastTime == 0) {
        anim->lastTime = frameTimeNanos;
        return false;
    }
    float dt = static_cast<float>(frameTimeNanos - anim->lastTime) * 1e-9f;
    anim->lastTime = frameTimeNanos;
    if (dt <= 0.0f) {
        return false;
    }
    if (dt > 0.05f) {
        dt = 0.05f; // 帧间隔异常（卡顿）时限制步长
    }

    state->rawX += anim->velocityX * dt;
    state->rawY += anim->velocityY * dt;
    applyRawOffset(anim->viewport, *state, true);

    const float maxX = maxOffsetX(*state, viewport);
    const float maxY = maxOffsetY(*state, viewport);
    if (state->rawX < 0.0f || state->rawX > maxX ||
        state->rawY < 0.0f || state->rawY > maxY) {
        enterSpring(anim, *state, viewport); // 冲出边界：转回弹
        return false;
    }

    const float loss = kFlingDeceleration * dt;
    anim->velocityX = decayVelocity(anim->velocityX, loss);
    anim->velocityY = decayVelocity(anim->velocityY, loss);
    if (std::fabs(anim->velocityX) < kFlingStopVelocity &&
        std::fabs(anim->velocityY) < kFlingStopVelocity) {
        state->animating = false;
        return true;
    }
    return false;
}

void scrollAnimationCleanup(void* userData) {
    delete static_cast<ScrollAnimation*>(userData);
}

void startScrollAnimation(esx_view scrollView, ScrollViewState& state,
                          bool springOnly, float velocityX, float velocityY) {
    auto* anim = new ScrollAnimation();
    anim->state = state.self;
    anim->viewport = scrollView;
    anim->velocityX = velocityX;
    anim->velocityY = velocityY;
    state.animating = true;
    if (springOnly) {
        if (evk::ui::View* viewport = esxViewFromHandle(scrollView)) {
            enterSpring(anim, state, viewport);
        }
    }
    evk::ui::startAnimation(scrollAnimationTick, anim, scrollAnimationCleanup);
}

bool rawOverscrolled(ScrollViewState& state, evk::ui::View* viewport) {
    return state.rawX < 0.0f || state.rawX > maxOffsetX(state, viewport) ||
           state.rawY < 0.0f || state.rawY > maxOffsetY(state, viewport);
}

void handleScrollPan(esx_view scrollView, const esx_view_pan_event* event,
                     void* userData) {
    auto* state = static_cast<ScrollViewState*>(userData);
    evk::ui::View* viewport = esxViewFromHandle(scrollView);
    if (!state || !viewport || !event) {
        return;
    }

    // 只有可滚动的方向才参与拖动/惯性/橡皮筋；不可滚动的轴全程冻结，
    // 否则竖滑时的水平抖动会被误判成越界，把 fling 堵死。
    const bool canX = maxOffsetX(*state, viewport) > 0.0f;
    const bool canY = maxOffsetY(*state, viewport) > 0.0f;

    switch (event->state) {
        case ESX_VIEW_PAN_BEGIN:
            // BEGIN 的 delta 是 DOWN 以来的总位移，与原实现一致按一次位移处理。
            state->animating = false;
            state->rawX = state->offsetX - (canX ? event->delta_x : 0.0f);
            state->rawY = state->offsetY - (canY ? event->delta_y : 0.0f);
            applyRawOffset(scrollView, *state, true);
            break;
        case ESX_VIEW_PAN_UPDATE:
            if (canX) {
                state->rawX -= event->delta_x;
            }
            if (canY) {
                state->rawY -= event->delta_y;
            }
            applyRawOffset(scrollView, *state, true);
            break;
        case ESX_VIEW_PAN_END: {
            if (rawOverscrolled(*state, viewport)) {
                startScrollAnimation(scrollView, *state, true, 0.0f, 0.0f);
                break;
            }
            // 手指速度与内容 offset 速度方向相反。
            const float flingX = canX ? -event->velocity_x : 0.0f;
            const float flingY = canY ? -event->velocity_y : 0.0f;
            if (std::fabs(flingX) >= kFlingMinVelocity ||
                std::fabs(flingY) >= kFlingMinVelocity) {
                startScrollAnimation(scrollView, *state, false, flingX, flingY);
            }
            break;
        }
        case ESX_VIEW_PAN_CANCEL:
            if (rawOverscrolled(*state, viewport)) {
                startScrollAnimation(scrollView, *state, true, 0.0f, 0.0f);
            }
            break;
    }
}

// 手指按下（无需拖动）即打断惯性/回弹，从当前显示位置接管。
void handleScrollPointer(esx_view scrollView, const evk::ui::PointerEvent& event,
                         void* userData) {
    auto* state = static_cast<ScrollViewState*>(userData);
    if (!state || !esxViewFromHandle(scrollView)) {
        return;
    }
    if (event.action == evk::ui::PointerAction::Down) {
        state->animating = false;
        state->rawX = state->offsetX;
        state->rawY = state->offsetY;
    }
}

void handleViewportBoundsChanged(esx_view scrollView, void* userData) {
    auto* state = static_cast<ScrollViewState*>(userData);
    if (state) {
        snapOffset(scrollView, *state, false);
    }
}

} // namespace

extern "C" {

esx_view esx_scroll_view_create(float x, float y, float width, float height,
                                float content_width, float content_height,
                                esx_view parent) {
    const esx_view viewport = esx_create_view(x, y, width, height, parent);
    if (viewport == 0) {
        return 0;
    }

    auto state = std::make_shared<ScrollViewState>();
    state->contentWidth = std::max(0.0f, content_width);
    state->contentHeight = std::max(0.0f, content_height);
    state->content = esx_create_view(0, 0, state->contentWidth, state->contentHeight,
                                     viewport);
    if (state->content == 0) {
        esx_destroy_view(viewport);
        return 0;
    }
    state->self = state;

    evk::ui::View* view = esxViewFromHandle(viewport);
    view->controlType = &kScrollViewType;
    view->controlState = state;
    view->pointerHandler = handleScrollPointer;
    view->pointerUserData = state.get();
    view->panFunc = handleScrollPan;
    view->panUserData = state.get();
    view->boundsChangedHandler = handleViewportBoundsChanged;
    view->boundsChangedUserData = state.get();
    return viewport;
}

esx_view esx_scroll_view_get_content(esx_view scroll_view) {
    auto state = scrollState(scroll_view);
    return state ? state->content : 0;
}

void esx_scroll_view_set_content_size(esx_view scroll_view, float width, float height) {
    auto state = scrollState(scroll_view);
    evk::ui::View* viewport = esxViewFromHandle(scroll_view);
    if (!state || !viewport) {
        return;
    }
    state->contentWidth = std::max(0.0f, width);
    state->contentHeight = std::max(0.0f, height);
    snapOffset(scroll_view, *state, false);
}

void esx_scroll_view_set_offset(esx_view scroll_view, float offset_x, float offset_y) {
    auto state = scrollState(scroll_view);
    if (!state) {
        return;
    }
    state->rawX = offset_x;
    state->rawY = offset_y;
    snapOffset(scroll_view, *state, true);
}

void esx_scroll_view_get_offset(esx_view scroll_view, float* offset_x, float* offset_y) {
    auto state = scrollState(scroll_view);
    if (!state) {
        return;
    }
    if (offset_x) {
        *offset_x = state->offsetX;
    }
    if (offset_y) {
        *offset_y = state->offsetY;
    }
}

void esx_scroll_view_set_on_scroll(esx_view scroll_view, esx_scroll_func on_scroll,
                                   void* user_data) {
    auto state = scrollState(scroll_view);
    if (!state) {
        return;
    }
    state->onScroll = on_scroll;
    state->userData = user_data;
}

} // extern "C"
