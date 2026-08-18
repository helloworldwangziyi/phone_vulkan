/**
 * @file scroll_control.cpp
 * @brief 滚动控件实现：ScrollView（手势 → 偏移）、ScrollContentView
 *        （单子填充布局）、ScrollAnimation（fling 惯性 / 回弹状态机）。
 */

#include "evk/ui/view/scroll_control.h"

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

/// 橡皮筋阻尼系数：越界部分的显示位移打 0.45 折。
constexpr float kRubberBand = 0.45f;
/// 松手速度低于该值不起 fling（避免轻蹭就滑走）。
constexpr float kFlingMinVelocity = 50.0f;
/// fling 衰减到该值以下即停（px/s）。
constexpr float kFlingStopVelocity = 30.0f;
/// fling 减速度（px/s²），越大滑停得越快。
constexpr float kFlingDeceleration = 2400.0f;
/// 回弹动画时长（250ms，easeOutCubic）。
constexpr int64_t kSpringDurationNanos = 250'000'000;

/// 手势方向锁：一段拖动手势只滚一个轴（表格横竖互斥，避免斜向跟手）。
enum class PanAxis {
    None,
    Horizontal,
    Vertical,
};

/**
 * @brief 滚动视口：位置固定的容器，持有内容尺寸、跟手偏移与显示偏移。
 *
 * rawX/rawY 是未钳制的跟手值（手势直接累加），offsetX/offsetY 是经
 * 橡皮筋阻尼后的显示值；手势（handlePan）只改 raw*，渲染与 onScroll
 * 回调只看 offset*。松手时按是否越界、速度大小进入回弹或 fling。
 * 双轴都可滚时按方向锁单轴响应（见 handlePan）。
 */
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
    /// 本段手势锁定的轴（Begin 时按主方向决定，End/Cancel/Down 复位）。
    PanAxis panAxis = PanAxis::None;

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
            panAxis = PanAxis::None;
            rawX = offsetX;
            rawY = offsetY;
        }
    }

    void handlePan(const PanEvent& event) override {
        const bool canX = maxOffsetX() > 0.0f;
        const bool canY = maxOffsetY() > 0.0f;
        switch (event.state) {
            case PanState::Begin: {
                animating = false;
                // 方向锁：Begin 的位移是按下以来的全程位移（已超 slop），
                // 主方向即锁定方向；主方向不可滚时退到另一轴。
                const float absX = std::fabs(event.deltaX);
                const float absY = std::fabs(event.deltaY);
                if (canX && (!canY || absX > absY)) {
                    panAxis = PanAxis::Horizontal;
                } else if (canY) {
                    panAxis = PanAxis::Vertical;
                }
                rawX = offsetX -
                       (panAxis == PanAxis::Horizontal ? event.deltaX : 0.0f);
                rawY = offsetY -
                       (panAxis == PanAxis::Vertical ? event.deltaY : 0.0f);
                applyRawOffset(true);
                break;
            }
            case PanState::Update:
                // 只累加锁定轴的位移，另一轴的分量整段丢弃。
                if (panAxis == PanAxis::Horizontal) {
                    rawX -= event.deltaX;
                }
                if (panAxis == PanAxis::Vertical) {
                    rawY -= event.deltaY;
                }
                applyRawOffset(true);
                break;
            case PanState::End: {
                const PanAxis axis = panAxis;
                panAxis = PanAxis::None;
                if (rawOverscrolled()) {
                    startScrollAnimation(true, 0.0f, 0.0f);
                    break;
                }
                // fling 同样只取锁定轴的速度分量。
                const float flingX =
                    axis == PanAxis::Horizontal ? -event.velocityX : 0.0f;
                const float flingY =
                    axis == PanAxis::Vertical ? -event.velocityY : 0.0f;
                if (std::fabs(flingX) >= kFlingMinVelocity ||
                    std::fabs(flingY) >= kFlingMinVelocity) {
                    startScrollAnimation(false, flingX, flingY);
                }
                break;
            }
            case PanState::Cancel:
                panAxis = PanAxis::None;
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

/**
 * @brief content 层：ScrollView 的唯一孩子，App 子树的挂载点。
 *
 * 自身 bounds 被 ScrollView 写成 (-offset, -offset, 内容宽, 内容高) 来
 * 实现平移；bounds 变化时把唯一孩子塞满自己，让子树整体跟随。
 */
class ScrollContentView final : public View {
public:
    void handleBoundsChanged() override {
        if (!children.empty()) {
            children.front()->setBounds(0.0f, 0.0f, rect.w, rect.h);
        }
    }
};

/**
 * @brief 列表 content 层：固定行高纵向堆叠全部孩子（ListView 的布局）。
 *
 * 与 ScrollContentView 的差异：不是塞满唯一孩子，而是把第 i 个孩子放到
 * (0, i × itemExtent, 内容宽, itemExtent)。随滚动整体平移的逻辑不变。
 * 行数变化（孩子增删）或自身尺寸变化时全量重排——行高固定使重排是
 * 纯算术 O(n)，差异比对避免无变化时的级联。
 */
class ListContentView final : public View {
public:
    float itemExtent = 0.0f;

    void layoutRows() {
        for (size_t i = 0; i < children.size(); ++i) {
            View* child = children[i].get();
            const float y = itemExtent * static_cast<float>(i);
            if (child->rect.x != 0.0f || child->rect.y != y ||
                child->rect.w != rect.w || child->rect.h != itemExtent) {
                child->setBounds(0.0f, y, rect.w, itemExtent);
            }
        }
    }

    void handleBoundsChanged() override { layoutRows(); }
    void handleChildRemoved(size_t) override { layoutRows(); }
};

/**
 * @brief 一次滚动动画的状态：spring = 回弹插值（from → to），否则
 *        fling 惯性（速度按 kFlingDeceleration 衰减，越界即转入回弹）。
 */
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

/// 创建公共路径：造 ScrollView 视口并挂入指定的 content 层。
std::unique_ptr<View> makeScrollView(
    std::unique_ptr<View> contentView,
    float contentWidth,
    float contentHeight,
    std::function<void(float, float)> onScroll) {
    auto scroll = std::make_unique<ScrollView>();
    scroll->contentWidth = std::max(0.0f, contentWidth);
    scroll->contentHeight = std::max(0.0f, contentHeight);
    scroll->onScroll = std::move(onScroll);
    scroll->content = scroll->addChild(std::move(contentView));
    scroll->setDisplayedOffset(0.0f, 0.0f, false);
    return scroll;
}

} // namespace

std::unique_ptr<View> createScrollView(
    float contentWidth,
    float contentHeight,
    std::function<void(float, float)> onScroll) {
    return makeScrollView(
        std::make_unique<ScrollContentView>(),
        contentWidth,
        contentHeight,
        std::move(onScroll));
}

std::unique_ptr<View> createListView(
    float itemExtent,
    float contentWidth,
    float contentHeight,
    std::function<void(float, float)> onScroll) {
    auto content = std::make_unique<ListContentView>();
    content->itemExtent = std::max(0.0f, itemExtent);
    return makeScrollView(
        std::move(content), contentWidth, contentHeight, std::move(onScroll));
}

void updateListView(
    View& listView,
    float itemExtent,
    float contentWidth,
    float contentHeight,
    std::function<void(float, float)> onScroll) {
    updateScrollView(listView, contentWidth, contentHeight, std::move(onScroll));
    View* content = scrollContent(listView);
    auto* list = dynamic_cast<ListContentView*>(content);
    if (!list) {
        return;
    }
    const float extent = std::max(0.0f, itemExtent);
    if (list->itemExtent != extent) {
        list->itemExtent = extent;
        list->layoutRows();
    }
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
