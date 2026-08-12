// ============================================================================
// ScrollView：可滚动容器控件（viewport + content 双层结构）。
//
// 继承 View：自身即 viewport，重写输入/布局钩子获得滚动行为——
//   acceptsPointerInput/handlePointer → 手指按下即打断惯性/回弹动画；
//   acceptsPanInput/handlePan         → 拖拽、甩动（fling）、橡皮筋；
//   handleBoundsChanged               → 尺寸变化时 clamp 并吸附 offset。
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
// 生命周期：动画上下文（ScrollAnimation）只存 viewport 句柄，tick 时用
// dynamic_cast 重新解析；视图销毁或 animating 标志被外部复位都会让 tick
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

constexpr float kRubberBand = 0.45f;          // 越界拖拽阻尼
constexpr float kFlingMinVelocity = 50.0f;    // 触发惯性的最小速度 px/s
constexpr float kFlingStopVelocity = 30.0f;   // 惯性停止阈值 px/s
constexpr float kFlingDeceleration = 2400.0f; // 惯性减速度 px/s²
constexpr int64_t kSpringDurationNanos = 250'000'000;

class ScrollView : public evk::ui::View {
public:
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

    bool acceptsPointerInput() const override { return true; }
    bool acceptsPanInput() const override { return true; }

    float maxOffsetX() const { return std::max(0.0f, contentWidth - rect.w); }
    float maxOffsetY() const { return std::max(0.0f, contentHeight - rect.h); }

    bool rawOverscrolled() const {
        return rawX < 0.0f || rawX > maxOffsetX() || rawY < 0.0f || rawY > maxOffsetY();
    }

    void setDisplayedOffset(float x, float y, bool notify) {
        evk::ui::View* contentView = esxViewFromHandle(content);
        if (!contentView) {
            return;
        }
        offsetX = x;
        offsetY = y;
        contentView->rect = {-offsetX, -offsetY, contentWidth, contentHeight};
        evk::requestRender();
        if (notify && onScroll) {
            onScroll(handle, offsetX, offsetY, userData);
        }
    }

    // 拖拽路径：raw 不过 clamp，显示值越界部分加阻尼（橡皮筋）。
    void applyRawOffset(bool notify) {
        setDisplayedOffset(dampedOffset(rawX, maxOffsetX()),
                           dampedOffset(rawY, maxOffsetY()), notify);
    }

    // 程序设定/尺寸变化路径：clamp 并吸附，同时打断进行中的动画。
    void snapOffset(bool notify) {
        animating = false;
        rawX = std::clamp(rawX, 0.0f, maxOffsetX());
        rawY = std::clamp(rawY, 0.0f, maxOffsetY());
        setDisplayedOffset(rawX, rawY, notify);
    }

    // 手指按下（无需拖动）即打断惯性/回弹，从当前显示位置接管。
    void handlePointer(const evk::ui::PointerEvent& event) override {
        if (event.action == evk::ui::PointerAction::Down) {
            animating = false;
            rawX = offsetX;
            rawY = offsetY;
        }
    }

    void handlePan(const esx_view_pan_event& event) override {
        // 只有可滚动的方向才参与拖动/惯性/橡皮筋；不可滚动的轴全程冻结，
        // 否则竖滑时的水平抖动会被误判成越界，把 fling 堵死。
        const bool canX = maxOffsetX() > 0.0f;
        const bool canY = maxOffsetY() > 0.0f;

        switch (event.state) {
            case ESX_VIEW_PAN_BEGIN:
                // BEGIN 的 delta 是 DOWN 以来的总位移，与原实现一致按一次位移处理。
                animating = false;
                rawX = offsetX - (canX ? event.delta_x : 0.0f);
                rawY = offsetY - (canY ? event.delta_y : 0.0f);
                applyRawOffset(true);
                break;
            case ESX_VIEW_PAN_UPDATE:
                if (canX) {
                    rawX -= event.delta_x;
                }
                if (canY) {
                    rawY -= event.delta_y;
                }
                applyRawOffset(true);
                break;
            case ESX_VIEW_PAN_END: {
                if (rawOverscrolled()) {
                    startScrollAnimation(true, 0.0f, 0.0f);
                    break;
                }
                // 手指速度与内容 offset 速度方向相反。
                const float flingX = canX ? -event.velocity_x : 0.0f;
                const float flingY = canY ? -event.velocity_y : 0.0f;
                if (std::fabs(flingX) >= kFlingMinVelocity ||
                    std::fabs(flingY) >= kFlingMinVelocity) {
                    startScrollAnimation(false, flingX, flingY);
                }
                break;
            }
            case ESX_VIEW_PAN_CANCEL:
                if (rawOverscrolled()) {
                    startScrollAnimation(true, 0.0f, 0.0f);
                }
                break;
        }
    }

    void handleBoundsChanged() override { snapOffset(false); }

private:
    // 越界部分按阻尼折算后的显示 offset：范围内原样，越界量 ×kRubberBand，
    // 形成"越拉越费力"的橡皮筋手感（iOS 同款简化模型）。
    static float dampedOffset(float offset, float max) {
        if (offset < 0.0f) {
            return offset * kRubberBand;
        }
        if (offset > max) {
            return max + (offset - max) * kRubberBand;
        }
        return offset;
    }

    void startScrollAnimation(bool springOnly, float velocityX, float velocityY);
};

// 一次 fling/spring 动画的上下文：只存 viewport 句柄，
// tick 时重新解析，绝不留可能悬空的 View 指针。
struct ScrollAnimation {
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

// 匀减速：每帧从速度里扣掉 loss（= kFlingDeceleration × dt），过零即停。
float decayVelocity(float velocity, float loss) {
    if (velocity > 0.0f) {
        return std::max(0.0f, velocity - loss);
    }
    return std::min(0.0f, velocity + loss);
}

// 转入回弹：从当前显示位置（含阻尼过冲）弹回 clamp 边界。
void enterSpring(ScrollAnimation* anim, ScrollView& view) {
    anim->spring = true;
    anim->fromX = view.offsetX;
    anim->fromY = view.offsetY;
    anim->toX = std::clamp(view.rawX, 0.0f, view.maxOffsetX());
    anim->toY = std::clamp(view.rawY, 0.0f, view.maxOffsetY());
    view.rawX = anim->toX;
    view.rawY = anim->toY;
    anim->startTime = 0; // 首帧再记录，避开注册到下一帧的间隔
}

bool scrollAnimationTick(int64_t frameTimeNanos, void* userData) {
    auto* anim = static_cast<ScrollAnimation*>(userData);
    auto* view = dynamic_cast<ScrollView*>(esxViewFromHandle(anim->viewport));
    if (!view || !view->animating) {
        return true; // 视图销毁或手势/程序已接管
    }

    if (anim->spring) {
        if (anim->startTime == 0) {
            anim->startTime = frameTimeNanos;
        }
        const float t = static_cast<float>(frameTimeNanos - anim->startTime) /
                        static_cast<float>(kSpringDurationNanos);
        if (t >= 1.0f) {
            view->setDisplayedOffset(anim->toX, anim->toY, true);
            view->animating = false;
            return true;
        }
        const float e = evk::ui::easeOutCubic(std::clamp(t, 0.0f, 1.0f));
        view->setDisplayedOffset(anim->fromX + (anim->toX - anim->fromX) * e,
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

    view->rawX += anim->velocityX * dt;
    view->rawY += anim->velocityY * dt;
    view->applyRawOffset(true);

    if (view->rawOverscrolled()) {
        enterSpring(anim, *view); // 冲出边界：转回弹
        return false;
    }

    const float loss = kFlingDeceleration * dt;
    anim->velocityX = decayVelocity(anim->velocityX, loss);
    anim->velocityY = decayVelocity(anim->velocityY, loss);
    if (std::fabs(anim->velocityX) < kFlingStopVelocity &&
        std::fabs(anim->velocityY) < kFlingStopVelocity) {
        view->animating = false;
        return true;
    }
    return false;
}

void scrollAnimationCleanup(void* userData) {
    delete static_cast<ScrollAnimation*>(userData);
}

void ScrollView::startScrollAnimation(bool springOnly, float velocityX, float velocityY) {
    auto* anim = new ScrollAnimation();
    anim->viewport = handle;
    anim->velocityX = velocityX;
    anim->velocityY = velocityY;
    animating = true;
    if (springOnly) {
        enterSpring(anim, *this);
    }
    evk::ui::startAnimation(scrollAnimationTick, anim, scrollAnimationCleanup);
}

// 句柄 → ScrollView；句柄无效或视图不是 ScrollView 时返回 nullptr。
ScrollView* scrollViewFromHandle(esx_view scrollView) {
    return dynamic_cast<ScrollView*>(esxViewFromHandle(scrollView));
}

} // namespace

extern "C" {

esx_view esx_scroll_view_create(float x, float y, float width, float height,
                                float content_width, float content_height,
                                esx_view parent) {
    auto view = std::make_unique<ScrollView>();
    view->contentWidth = std::max(0.0f, content_width);
    view->contentHeight = std::max(0.0f, content_height);

    ScrollView* raw = view.get();
    const esx_view viewport =
        esxAdoptViewNode(std::move(view), x, y, width, height, parent);
    if (viewport == 0) {
        return 0;
    }

    raw->content = esx_create_view(0, 0, raw->contentWidth, raw->contentHeight, viewport);
    if (raw->content == 0) {
        esx_destroy_view(viewport);
        return 0;
    }
    return viewport;
}

esx_view esx_scroll_view_get_content(esx_view scroll_view) {
    ScrollView* self = scrollViewFromHandle(scroll_view);
    return self ? self->content : 0;
}

void esx_scroll_view_set_content_size(esx_view scroll_view, float width, float height) {
    ScrollView* self = scrollViewFromHandle(scroll_view);
    if (!self) {
        return;
    }
    self->contentWidth = std::max(0.0f, width);
    self->contentHeight = std::max(0.0f, height);
    self->snapOffset(false);
}

void esx_scroll_view_set_offset(esx_view scroll_view, float offset_x, float offset_y) {
    ScrollView* self = scrollViewFromHandle(scroll_view);
    if (!self) {
        return;
    }
    self->rawX = offset_x;
    self->rawY = offset_y;
    self->snapOffset(true);
}

void esx_scroll_view_get_offset(esx_view scroll_view, float* offset_x, float* offset_y) {
    ScrollView* self = scrollViewFromHandle(scroll_view);
    if (!self) {
        return;
    }
    if (offset_x) {
        *offset_x = self->offsetX;
    }
    if (offset_y) {
        *offset_y = self->offsetY;
    }
}

void esx_scroll_view_set_on_scroll(esx_view scroll_view, esx_scroll_func on_scroll,
                                   void* user_data) {
    ScrollView* self = scrollViewFromHandle(scroll_view);
    if (!self) {
        return;
    }
    self->onScroll = on_scroll;
    self->userData = user_data;
}

} // extern "C"
