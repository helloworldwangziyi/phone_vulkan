/**
 * @file scroll_view.cpp
 * @brief ScrollView：可滚动容器控件（viewport + content 双层结构）。
 *
 * 继承 View：自身即 viewport，重写输入/布局钩子获得滚动行为——
 * - acceptsPointerInput/handlePointer → 手指按下即打断惯性/回弹动画；
 * - acceptsPanInput/handlePan         → 拖拽、甩动（fling）、橡皮筋；
 * - handleBoundsChanged               → 尺寸变化时 clamp 并吸附 offset。
 *
 * offset 双轨模型：
 * - rawX/rawY       —— 手势累计的目标 offset，拖拽期间不做 clamp，
 *   允许越界（越界量供橡皮筋显示）；
 * - offsetX/offsetY —— 实际显示的 offset，越界部分按 kRubberBand 阻尼折算，
 *   content 视图的位置永远由它决定。
 *
 * 方向门禁：只有可滚动（maxOffset > 0）的轴参与拖动/惯性/橡皮筋，
 * 不可滚动的轴全程冻结——否则竖滑时的水平抖动会被误判成越界，
 * 把 fling 堵死（真机回归，见 tests 的 VerticalOnlyIgnoresHorizontal）。
 *
 * 松手后的动画状态机（复用 ui/animator 逐帧驱动）：
 * - 越界          → spring：easeOutCubic 从当前显示位置弹回 clamp 边界；
 * - 未越界且有速度 → fling：raw 按匀减速积分，冲出边界时转 spring；
 * - 手指按下（pointer Down 或再次拖动）/ 程序 set_offset / 尺寸变化
 *   → 立即打断动画，从当前显示位置接管。
 *
 * 生命周期：动画上下文（ScrollAnimation）只存 viewport 句柄，tick 时用
 * dynamic_cast 重新解析；视图销毁或 animating 标志被外部复位都会让 tick
 * 返回 true 并触发 cleanup 释放上下文。
 */

#include "evk/ui/controls/scroll_view.h"

#include <algorithm>
#include <cmath>
#include <memory>

#include "evk/render_loop.h"
#include "evk/ui/animator.h"
#include "evk/ui/view.h"

namespace {

constexpr float kRubberBand = 0.45f; ///< 越界拖拽阻尼
constexpr float kFlingMinVelocity = 50.0f; ///< 触发惯性的最小速度 px/s
constexpr float kFlingStopVelocity = 30.0f; ///< 惯性停止阈值 px/s
constexpr float kFlingDeceleration = 2400.0f; ///< 惯性减速度 px/s²
constexpr int64_t kSpringDurationNanos = 250'000'000; ///< 回弹动画时长 ns

/**
 * @brief ScrollView 实现：自身即 viewport，重写输入/布局钩子获得滚动行为。
 */
class ScrollView : public evk::ui::View {
public:
    esx_view content = 0; ///< content 子视图句柄（App 把内容挂到它下面）
    float contentWidth = 0.0f; ///< 内容尺寸（决定 maxOffset，可大于 viewport）
    float contentHeight = 0.0f; ///< 内容高度（含义同 contentWidth）
    float rawX = 0.0f; ///< 拖拽累计 offset（越界部分未阻尼，保留越界量）
    float rawY = 0.0f; ///< 同 rawX（纵轴）
    float offsetX = 0.0f; ///< 实际显示 offset（越界部分已阻尼；content.rect 用它）
    float offsetY = 0.0f; ///< 同 offsetX（纵轴）
    esx_scroll_func onScroll = nullptr; ///< 每次 offset 落定（含动画帧）时通知
    void* userData = nullptr; ///< onScroll 的 user_data
    bool animating = false; ///< fling/spring 进行中；手势 BEGIN/程序 set_offset/尺寸变化打断

    bool acceptsPointerInput() const override { return true; }
    bool acceptsPanInput() const override { return true; }

    /// X 轴最大可滚动 offset（内容宽超出 viewport 宽的部分，≥0）。
    float maxOffsetX() const { return std::max(0.0f, contentWidth - rect.w); }
    /// Y 轴最大可滚动 offset（内容高超出 viewport 高的部分，≥0）。
    float maxOffsetY() const { return std::max(0.0f, contentHeight - rect.h); }

    /// raw offset 是否越界（任一轴超出 [0, maxOffset]）。
    bool rawOverscrolled() const {
        return rawX < 0.0f || rawX > maxOffsetX() || rawY < 0.0f || rawY > maxOffsetY();
    }

    /**
     * @brief 滚动 = 平移 content 视图的 rect（相对 viewport 左上角偏移 -offset）。
     *
     * 这是滚动机制的全部：content 向上/左移动 → 下一帧绘制时配合 clipFor
     * 的 viewport 裁剪，只显示 viewport 范围内的部分；没有脏标记系统，
     * "改 rect + requestRender"就是一次滚动更新（拖拽/动画每帧各来一次）。
     * notify=true 时同步 onScroll 回调（声明式层 ScrollW 的 onScroll）。
     */
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

    /// 拖拽路径：raw 不过 clamp，显示值越界部分加阻尼（橡皮筋）。
    void applyRawOffset(bool notify) {
        setDisplayedOffset(dampedOffset(rawX, maxOffsetX()),
                           dampedOffset(rawY, maxOffsetY()), notify);
    }

    /// 程序设定/尺寸变化路径：clamp 并吸附，同时打断进行中的动画。
    void snapOffset(bool notify) {
        animating = false;
        rawX = std::clamp(rawX, 0.0f, maxOffsetX());
        rawY = std::clamp(rawY, 0.0f, maxOffsetY());
        setDisplayedOffset(rawX, rawY, notify);
    }

    /**
     * @brief 手指按下（无需拖动）即打断惯性/回弹，从当前显示位置接管——
     * 哪怕这次手势最终被判为点击（未超 12px 阈值），惯性也必须停，
     * 否则"点一下列表"会先看到惯性继续滑（体验断裂）。
     */
    void handlePointer(const evk::ui::PointerEvent& event) override {
        if (event.action == evk::ui::PointerAction::Down) {
            animating = false;
            rawX = offsetX;
            rawY = offsetY;
        }
    }

    /**
     * @brief 滑动手势处理（pan 事件由输入层按目标派发，坐标已换算为相对本视图）。
     *
     * delta_x/y：本次 Move 的位移；velocity_x/y：最近 100ms 的滑动速度。
     * 核心规则：内容跟随手指同向移动——手指右移（delta_x>0）→ rawX 减小
     *   → offsetX 减小 → content.rect.x=-offsetX 增大 → 内容右移，露出左侧内容。
     */
    void handlePan(const esx_view_pan_event& event) override {
        // 方向门禁：只有可滚动的轴才参与拖动/惯性/橡皮筋，不可滚动的轴全程冻结，
        // 否则竖滑时的水平抖动会被误判成越界，把 fling 堵死。
        const bool canX = maxOffsetX() > 0.0f;
        const bool canY = maxOffsetY() > 0.0f;

        switch (event.state) {
            case ESX_VIEW_PAN_BEGIN:
                // BEGIN 的 delta 是 DOWN 以来的总位移，与原实现一致按一次位移处理。
                // 先打断进行中的 fling/spring（手指接管），raw 从当前显示位置起算。
                animating = false;
                rawX = offsetX - (canX ? event.delta_x : 0.0f);
                rawY = offsetY - (canY ? event.delta_y : 0.0f);
                applyRawOffset(true);
                break;
            case ESX_VIEW_PAN_UPDATE:
                // 逐次累加位移；raw 不做 clamp（越界量保留，供阻尼/回弹使用）。
                if (canX) {
                    rawX -= event.delta_x;
                }
                if (canY) {
                    rawY -= event.delta_y;
                }
                applyRawOffset(true);
                break;
            case ESX_VIEW_PAN_END: {
                // 松手决策：越界 → spring 回弹；否则速度足够 → fling 惯性；
                // 两者都不满足 → 停在当前位置。
                if (rawOverscrolled()) {
                    startScrollAnimation(true, 0.0f, 0.0f);
                    break;
                }
                // 手指速度与内容 offset 速度方向相反（手指上滑 → 内容下移看更后面）。
                const float flingX = canX ? -event.velocity_x : 0.0f;
                const float flingY = canY ? -event.velocity_y : 0.0f;
                if (std::fabs(flingX) >= kFlingMinVelocity ||
                    std::fabs(flingY) >= kFlingMinVelocity) {
                    startScrollAnimation(false, flingX, flingY);
                }
                break;
            }
            case ESX_VIEW_PAN_CANCEL:
                // 手势被系统取消（页面切换等）：越界则弹回边界，否则停在原地。
                if (rawOverscrolled()) {
                    startScrollAnimation(true, 0.0f, 0.0f);
                }
                break;
        }
    }

    /// 尺寸变化：clamp 并吸附当前 offset。
    void handleBoundsChanged() override { snapOffset(false); }

private:
    /**
     * @brief 越界部分按阻尼折算后的显示 offset：范围内原样，越界量 ×kRubberBand，
     * 形成"越拉越费力"的橡皮筋手感（iOS 同款简化模型）。
     */
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

/**
 * @brief 一次 fling/spring 动画的上下文：只存 viewport 句柄，tick 时重新解析，
 * 绝不留可能悬空的 View 指针（视图销毁后 tick 解析失败即退场清理）。
 *
 * 两个模式复用同一结构：spring=true 走回弹字段，false 走惯性字段。
 */
struct ScrollAnimation {
    esx_view viewport = 0; ///< 目标 ScrollView 句柄（每次 tick 重解析）
    bool spring = false; ///< true=回弹模式；false=fling 惯性模式
    /// fling 状态：内容 offset 速度（px/s，与手指速度方向相反）。
    float velocityX = 0.0f;
    float velocityY = 0.0f;
    int64_t lastTime = 0; ///< 上一帧时间（首帧只记时，拿不到可靠帧间隔）
    /// spring 状态：从当前显示位置（含阻尼过冲）弹回 clamp 边界。
    float fromX = 0.0f;
    float fromY = 0.0f;
    float toX = 0.0f; ///< = clamp(raw)，见 enterSpring
    float toY = 0.0f;
    int64_t startTime = 0; ///< 0=未开始（首帧才记录，避开注册间隙）
};

/// 匀减速：每帧从速度里扣掉 loss（= kFlingDeceleration × dt），过零即停。
float decayVelocity(float velocity, float loss) {
    if (velocity > 0.0f) {
        return std::max(0.0f, velocity - loss);
    }
    return std::min(0.0f, velocity + loss);
}

/// 转入回弹：从当前显示位置（含阻尼过冲）弹回 clamp 边界。
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

/// 动画 tick：spring 按 easeOutCubic 插值回弹；fling 匀减速积分，冲界转 spring、低速停住。
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

/// 动画结束清理：释放 ScrollAnimation 上下文。
void scrollAnimationCleanup(void* userData) {
    delete static_cast<ScrollAnimation*>(userData);
}

/// 启动 fling/spring 动画（springOnly=true 直接进入回弹）。
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

/// 句柄 → ScrollView；句柄无效或视图不是 ScrollView 时返回 nullptr。
ScrollView* scrollViewFromHandle(esx_view scrollView) {
    return dynamic_cast<ScrollView*>(esxViewFromHandle(scrollView));
}

} // namespace

extern "C" {

esx_view esx_scroll_view_create(float content_width, float content_height,
                                esx_view parent) {
    auto view = std::make_unique<ScrollView>();
    view->contentWidth = std::max(0.0f, content_width);
    view->contentHeight = std::max(0.0f, content_height);

    ScrollView* raw = view.get();
    const esx_view viewport = esxAdoptViewNode(std::move(view), parent);
    if (viewport == 0) {
        return 0;
    }

    raw->content = esx_create_view(viewport);
    if (raw->content == 0) {
        esx_destroy_view(viewport);
        return 0;
    }
    // content 初始矩形 = 内容尺寸（滚动的 0 偏移位置）；viewport 被布局后
    // handleBoundsChanged → snapOffset 会按 offset 重算 content 位置。
    esx_view_set_bounds(raw->content, 0.0f, 0.0f, raw->contentWidth,
                        raw->contentHeight);
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
