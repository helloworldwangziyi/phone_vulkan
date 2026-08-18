#include "evk/ui/controls/scroll_control.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

#include "evk/frame_scheduler.h"
#include "evk/ui/animation_scheduler.h"
#include "evk/ui/pointer_input.h"
#include "evk/ui/render_view.h"

namespace evk::ui {
namespace {

constexpr float kRubberBand = 0.45f;
constexpr float kFlingMinVelocity = 50.0f;
constexpr float kFlingStopVelocity = 30.0f;
constexpr float kFlingDeceleration = 2400.0f;
constexpr int64_t kSpringDurationNanos = 250'000'000;

class ScrollView final : public View {
public:
    View* content = nullptr;
    float contentWidth = 0.0f;
    float contentHeight = 0.0f;
    float rawX = 0.0f;
    float rawY = 0.0f;
    float offsetX = 0.0f;
    float offsetY = 0.0f;
    /// 最近一次收编越界时的视口尺寸，供 handleBoundsChanged 识别同尺寸回调。
    float snappedW = -1.0f;
    float snappedH = -1.0f;
    std::function<void(float, float)> onScroll;
    bool animating = false;

    bool acceptsPointerInput() const override { return true; }
    bool acceptsPanInput() const override { return true; }

    float effectiveContentWidth() const {
        return contentWidth > 0.0f ? contentWidth : rect.w;
    }
    float maxOffsetX() const {
        return std::max(0.0f, effectiveContentWidth() - rect.w);
    }
    float maxOffsetY() const { return std::max(0.0f, contentHeight - rect.h); }

    bool rawOverscrolled() const {
        return rawX < 0.0f || rawX > maxOffsetX() ||
               rawY < 0.0f || rawY > maxOffsetY();
    }

    void setDisplayedOffset(float x, float y, bool notify) {
        if (!content) {
            return;
        }
        offsetX = x;
        offsetY = y;
        content->setBounds(
            -offsetX, -offsetY, effectiveContentWidth(), contentHeight);
        requestRender();
        if (notify && onScroll) {
            onScroll(offsetX, offsetY);
        }
    }

    static float dampedOffset(float offset, float maximum) {
        if (offset < 0.0f) {
            return offset * kRubberBand;
        }
        if (offset > maximum) {
            return maximum + (offset - maximum) * kRubberBand;
        }
        return offset;
    }

    void applyRawOffset(bool notify) {
        setDisplayedOffset(
            dampedOffset(rawX, maxOffsetX()),
            dampedOffset(rawY, maxOffsetY()),
            notify);
    }

    void snapOffset(bool notify) {
        animating = false;
        rawX = std::clamp(rawX, 0.0f, maxOffsetX());
        rawY = std::clamp(rawY, 0.0f, maxOffsetY());
        setDisplayedOffset(rawX, rawY, notify);
    }

    void handlePointer(const PointerEvent& event) override {
        if (event.action == PointerAction::Down) {
            animating = false;
            rawX = offsetX;
            rawY = offsetY;
        }
    }

    void handlePan(const PanEvent& event) override {
        const bool canX = maxOffsetX() > 0.0f;
        const bool canY = maxOffsetY() > 0.0f;
        switch (event.state) {
            case PanState::Begin:
                animating = false;
                rawX = offsetX - (canX ? event.deltaX : 0.0f);
                rawY = offsetY - (canY ? event.deltaY : 0.0f);
                applyRawOffset(true);
                break;
            case PanState::Update:
                if (canX) {
                    rawX -= event.deltaX;
                }
                if (canY) {
                    rawY -= event.deltaY;
                }
                applyRawOffset(true);
                break;
            case PanState::End: {
                if (rawOverscrolled()) {
                    startScrollAnimation(true, 0.0f, 0.0f);
                    break;
                }
                const float flingX = canX ? -event.velocityX : 0.0f;
                const float flingY = canY ? -event.velocityY : 0.0f;
                if (std::fabs(flingX) >= kFlingMinVelocity ||
                    std::fabs(flingY) >= kFlingMinVelocity) {
                    startScrollAnimation(false, flingX, flingY);
                }
                break;
            }
            case PanState::Cancel:
                if (rawOverscrolled()) {
                    startScrollAnimation(true, 0.0f, 0.0f);
                }
                break;
        }
    }

    void handleBoundsChanged() override {
        // 仅视口尺寸真实变化时才收编越界。rebuild 链（updateChildren 收尾、
        // 同值 setBounds）会无条件调到本钩子，原样 snap 会把拖动中的橡皮筋
        // 越界直接钳回界内——「手没松开就复位」的成因。
        if (rect.w == snappedW && rect.h == snappedH) {
            return;
        }
        snappedW = rect.w;
        snappedH = rect.h;
        snapOffset(false);
    }

    void startScrollAnimation(bool springOnly, float velocityX, float velocityY);
};

class ScrollContentView final : public View {
public:
    void handleBoundsChanged() override {
        if (!children.empty()) {
            children.front()->setBounds(0.0f, 0.0f, rect.w, rect.h);
        }
    }
};

struct ScrollAnimation {
    ViewRef viewport;
    bool spring = false;
    float velocityX = 0.0f;
    float velocityY = 0.0f;
    int64_t lastTime = 0;
    float fromX = 0.0f;
    float fromY = 0.0f;
    float toX = 0.0f;
    float toY = 0.0f;
    int64_t startTime = 0;
};

ScrollView* asScroll(View& view) {
    return dynamic_cast<ScrollView*>(&view);
}

const ScrollView* asScroll(const View& view) {
    return dynamic_cast<const ScrollView*>(&view);
}

float decayVelocity(float velocity, float loss) {
    if (velocity > 0.0f) {
        return std::max(0.0f, velocity - loss);
    }
    return std::min(0.0f, velocity + loss);
}

void enterSpring(ScrollAnimation& animation, ScrollView& view) {
    animation.spring = true;
    animation.fromX = view.offsetX;
    animation.fromY = view.offsetY;
    animation.toX = std::clamp(view.rawX, 0.0f, view.maxOffsetX());
    animation.toY = std::clamp(view.rawY, 0.0f, view.maxOffsetY());
    view.rawX = animation.toX;
    view.rawY = animation.toY;
    animation.startTime = 0;
}

bool tickScrollAnimation(
    const std::shared_ptr<ScrollAnimation>& animation,
    int64_t frameTimeNanos) {
    auto* view = dynamic_cast<ScrollView*>(animation->viewport.get());
    if (!view || !view->animating) {
        return true;
    }

    if (animation->spring) {
        if (animation->startTime == 0) {
            animation->startTime = frameTimeNanos;
        }
        const float t = static_cast<float>(frameTimeNanos - animation->startTime) /
                        static_cast<float>(kSpringDurationNanos);
        if (t >= 1.0f) {
            view->setDisplayedOffset(animation->toX, animation->toY, true);
            view->animating = false;
            return true;
        }
        const float eased = easeOutCubic(std::clamp(t, 0.0f, 1.0f));
        view->setDisplayedOffset(
            animation->fromX + (animation->toX - animation->fromX) * eased,
            animation->fromY + (animation->toY - animation->fromY) * eased,
            true);
        return false;
    }

    if (animation->lastTime == 0) {
        animation->lastTime = frameTimeNanos;
        return false;
    }
    float dt = static_cast<float>(frameTimeNanos - animation->lastTime) * 1e-9f;
    animation->lastTime = frameTimeNanos;
    dt = std::clamp(dt, 0.0f, 0.05f);
    view->rawX += animation->velocityX * dt;
    view->rawY += animation->velocityY * dt;
    view->applyRawOffset(true);
    if (view->rawOverscrolled()) {
        enterSpring(*animation, *view);
        return false;
    }

    const float loss = kFlingDeceleration * dt;
    animation->velocityX = decayVelocity(animation->velocityX, loss);
    animation->velocityY = decayVelocity(animation->velocityY, loss);
    if (std::fabs(animation->velocityX) < kFlingStopVelocity &&
        std::fabs(animation->velocityY) < kFlingStopVelocity) {
        view->animating = false;
        return true;
    }
    return false;
}

void ScrollView::startScrollAnimation(
    bool springOnly, float velocityX, float velocityY) {
    auto animation = std::make_shared<ScrollAnimation>();
    animation->viewport = ref();
    animation->velocityX = velocityX;
    animation->velocityY = velocityY;
    animating = true;
    if (springOnly) {
        enterSpring(*animation, *this);
    }
    startAnimation([animation](int64_t frameTimeNanos) {
        return tickScrollAnimation(animation, frameTimeNanos);
    });
}

} // namespace

std::unique_ptr<View> createScrollView(
    float contentWidth,
    float contentHeight,
    std::function<void(float, float)> onScroll) {
    auto scroll = std::make_unique<ScrollView>();
    scroll->contentWidth = std::max(0.0f, contentWidth);
    scroll->contentHeight = std::max(0.0f, contentHeight);
    scroll->onScroll = std::move(onScroll);
    scroll->content = scroll->addChild(std::make_unique<ScrollContentView>());
    scroll->setDisplayedOffset(0.0f, 0.0f, false);
    return scroll;
}

View* scrollContent(View& scrollView) {
    ScrollView* scroll = asScroll(scrollView);
    return scroll ? scroll->content : nullptr;
}

void updateScrollView(
    View& scrollView,
    float contentWidth,
    float contentHeight,
    std::function<void(float, float)> onScroll) {
    ScrollView* scroll = asScroll(scrollView);
    if (!scroll) {
        return;
    }
    const float width = std::max(0.0f, contentWidth);
    const float height = std::max(0.0f, contentHeight);
    // 内容尺寸没变就不动滚动状态：widget 重建每次都会走到这里，无条件
    // snap 会打断进行中的橡皮筋越界和 fling（animating 被清、raw 被钳）。
    const bool sizeChanged =
        width != scroll->contentWidth || height != scroll->contentHeight;
    scroll->contentWidth = width;
    scroll->contentHeight = height;
    scroll->onScroll = std::move(onScroll);
    if (sizeChanged) {
        scroll->snapOffset(false);
    }
}

void setScrollOffset(View& scrollView, float x, float y) {
    ScrollView* scroll = asScroll(scrollView);
    if (!scroll) {
        return;
    }
    scroll->rawX = x;
    scroll->rawY = y;
    scroll->snapOffset(true);
}

void getScrollOffset(const View& scrollView, float* x, float* y) {
    const ScrollView* scroll = asScroll(scrollView);
    if (!scroll) {
        return;
    }
    if (x) {
        *x = scroll->offsetX;
    }
    if (y) {
        *y = scroll->offsetY;
    }
}

} // namespace evk::ui
