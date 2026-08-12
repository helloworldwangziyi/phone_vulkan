// ============================================================================
// Navigation：页面栈导航容器（iOS 风格）。
//
// 继承 View：自身即导航容器，重写钩子获得导航行为——
//   acceptsPanInput/handlePan → 左缘滑动手势驱动交互式返回；
//   handleBoundsChanged       → 尺寸变化时取消转场并重排到最终状态。
//
// 视图结构：
//   nav view（通常作为根视图）
//   ├─ container  页面容器（导航栏以下区域），页面都是它的直接子视图；
//   │             页面滑出容器会被自动裁剪（父链 intersect 的 clip 机制）
//   └─ bar        导航栏（后添加 → 永远在页面上层）
//       ├─ barLine     底部分隔线
//       └─ backButton  返回按钮（Button 控件 + drawFunc 画箭头），栈深 >1 可见
//
// 转场统一用"顶层页面滑出进度 p"表示（p=0 盖严，p=1 完全滑出屏右）：
//   push      = p: 1 → 0（新页从右滑入，旧页按 kParallax 比例视差后退）；
//   pop       = p: 0 → 1（顶层滑出，下层归位，完成后销毁顶层）；
//   左滑返回  = 手指直接驱动 p，松手按速度（>400px/s）或进度（>0.5）
//               决定补全（同 pop）还是回弹（回到 p=0）。
// 三条路径最终都汇入同一个 NavTransition 时间动画，时长按剩余进度折算。
//
// 页面所有权：App 以 parent=0 创建页面，push 时经 esxAdoptChild 把
// unique_ptr 移交给容器；pop 动画完成后由 Navigation 销毁页面
// （销毁前先回调 on_pop，让 App 清理挂在页面上的自有数据）。
// 转场进行中忽略新的 push/pop/返回手势；尺寸变化时取消转场并吸附到最终状态。
//
// 生命周期：转场上下文（NavTransition）只存 nav 句柄，tick 时用
// dynamic_cast 重新解析，不留可能悬空的 View 指针；返回按钮回调里的
// Navigation* 原始指针安全——backButton 是 nav 的后代，同树同生死，
// 单线程模型下析构期间不会有事件分发。
// ============================================================================

#include "evk/ui/controls/navigation.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

#include "evk/log.h"
#include "evk/ui/animator.h"
#include "evk/ui/controls/button.h"
#include "evk/ui/view.h"

namespace {

constexpr int64_t kTransitionNanos = 280'000'000;
constexpr int64_t kMinSettleNanos = 80'000'000;
constexpr float kParallax = 0.28f;          // 下层页面视差后退比例
constexpr float kEdgeWidth = 48.0f;         // 左滑返回的左缘热区 px
constexpr float kCompleteVelocity = 400.0f; // 直接判定完成返回的右滑速度 px/s

class Navigation : public evk::ui::View {
public:
    esx_view container = 0; // 页面容器（导航栏以下区域），页面都是它的子视图
    esx_view bar = 0;
    esx_view barLine = 0;
    esx_view backButton = 0;
    float barHeight = 0.0f;
    esx_navigation_style style{};
    std::vector<esx_view> pages; // 栈底 → 栈顶
    bool transitioning = false;
    bool swiping = false;
    esx_navigation_pop_func onPop = nullptr;
    void* onPopUserData = nullptr;

    bool acceptsPanInput() const override { return true; }

    void handlePan(const esx_view_pan_event& event) override {
        if (transitioning) {
            return;
        }
        evk::ui::View* containerView = esxViewFromHandle(container);
        if (!containerView || containerView->rect.w <= 0.0f) {
            return;
        }
        const float w = containerView->rect.w;

        if (event.state == ESX_VIEW_PAN_BEGIN) {
            // 只有从左缘热区出发的手势才认领为返回手势。
            const float downX = event.x - event.translation_x;
            swiping = pages.size() > 1 && downX <= kEdgeWidth;
            if (swiping) {
                const esx_view under = pages[pages.size() - 2];
                esx_view_set_visible(under, 1);
                applyTransitionPositions(pages.back(), under, 0.0f);
            }
            return;
        }
        if (!swiping || pages.size() < 2) {
            return;
        }

        const esx_view top = pages.back();
        const esx_view under = pages[pages.size() - 2];
        const float progress = std::clamp(event.translation_x / w, 0.0f, 1.0f);

        if (event.state == ESX_VIEW_PAN_UPDATE) {
            applyTransitionPositions(top, under, progress);
            return;
        }
        swiping = false;
        if (event.state == ESX_VIEW_PAN_END) {
            // 快甩（速度超阈值）或拖过一半 → 补全返回；否则回弹取消。
            const bool complete = event.velocity_x > kCompleteVelocity || progress > 0.5f;
            startTransition(top, under, progress, complete ? 1.0f : 0.0f, complete);
        } else { // ESX_VIEW_PAN_CANCEL：回弹取消
            startTransition(top, under, progress, 0.0f, false);
        }
    }

    void handleBoundsChanged() override {
        // 尺寸变化：取消转场/手势并吸附到最终状态（进行中的 tick 会发现
        // transitioning=false 自行退场）。
        transitioning = false;
        swiping = false;
        layoutBar();
        for (size_t i = 0; i < pages.size(); ++i) {
            const bool isTop = i + 1 == pages.size();
            layoutPage(pages[i], 0.0f);
            esx_view_set_visible(pages[i], isTop ? 1 : 0);
        }
    }

    // 页面在容器坐标内定位：{x, 0, 容器宽, 容器高}。
    void layoutPage(esx_view page, float x) {
        evk::ui::View* view = esxViewFromHandle(page);
        if (!view || !view->parent) {
            return;
        }
        esx_view_set_bounds(page, x, 0.0f, view->parent->rect.w, view->parent->rect.h);
    }

    // 按进度 p 摆放两页：top 从盖严（0）滑到屏右（w）；
    // under 从视差后位（-w×kParallax）归位（0），两页镜像联动。
    void applyTransitionPositions(esx_view top, esx_view under, float progress) {
        evk::ui::View* containerView = esxViewFromHandle(container);
        if (!containerView) {
            return;
        }
        const float w = containerView->rect.w;
        layoutPage(top, w * progress);
        if (under != 0) {
            layoutPage(under, -w * kParallax * (1.0f - progress));
        }
    }

    void layoutBar() {
        const float w = rect.w;
        if (container != 0) {
            esx_view_set_bounds(container, 0.0f, barHeight, w,
                                std::max(0.0f, rect.h - barHeight));
        }
        if (bar != 0) {
            esx_view_set_bounds(bar, 0.0f, 0.0f, w, barHeight);
        }
        if (backButton != 0) {
            const float buttonSize = barHeight * 0.6f;
            esx_view_set_bounds(backButton, barHeight * 0.2f,
                                (barHeight - buttonSize) * 0.5f,
                                buttonSize, buttonSize);
        }
    }

    void updateBackButton() {
        if (backButton != 0) {
            esx_view_set_visible(backButton, pages.size() > 1 ? 1 : 0);
        }
    }

    void startTransition(esx_view top, esx_view under,
                         float fromProgress, float toProgress, bool completesPop);
};

// 一次转场：统一用 progress 表示"top 页滑出程度"，
// p=0 时 top 盖住 under，p=1 时 top 完全滑出屏右。
struct NavTransition {
    esx_view nav = 0;
    esx_view top = 0;
    esx_view under = 0;
    bool completesPop = false; // 结束于 p=1 时是否完成 pop（销毁 top）
    float fromProgress = 0.0f;
    float toProgress = 0.0f;
    int64_t startTime = 0;
    int64_t durationNanos = kTransitionNanos;
};

void finishTransition(Navigation& nav, const NavTransition* transition) {
    nav.transitioning = false;
    if (transition->completesPop) {
        if (!nav.pages.empty() && nav.pages.back() == transition->top) {
            nav.pages.pop_back();
        }
        if (transition->under != 0) {
            esx_view_set_visible(transition->under, 1);
            nav.layoutPage(transition->under, 0.0f);
        }
        if (nav.onPop) {
            nav.onPop(nav.handle, transition->top, nav.onPopUserData);
        }
        esx_destroy_view(transition->top);
    } else {
        // push 完成或左滑取消：top 归位，under 回到隐藏位。
        nav.layoutPage(transition->top, 0.0f);
        if (transition->under != 0) {
            esx_view_set_visible(transition->under, 0);
            nav.applyTransitionPositions(transition->top, transition->under, 0.0f);
        }
    }
    nav.updateBackButton();
}

bool navTransitionTick(int64_t frameTimeNanos, void* userData) {
    auto* transition = static_cast<NavTransition*>(userData);
    auto* nav = dynamic_cast<Navigation*>(esxViewFromHandle(transition->nav));
    if (!nav || !nav->transitioning) {
        return true; // 导航销毁或转场被取消（如尺寸变化吸附）
    }
    if (transition->startTime == 0) {
        transition->startTime = frameTimeNanos;
    }
    const float t = static_cast<float>(frameTimeNanos - transition->startTime) /
                    static_cast<float>(transition->durationNanos);
    const float progress =
        transition->fromProgress +
        (transition->toProgress - transition->fromProgress) *
            evk::ui::easeOutCubic(std::clamp(t, 0.0f, 1.0f));
    nav->applyTransitionPositions(transition->top, transition->under, progress);
    if (t >= 1.0f) {
        finishTransition(*nav, transition);
        return true;
    }
    return false;
}

void navTransitionCleanup(void* userData) {
    delete static_cast<NavTransition*>(userData);
}

void Navigation::startTransition(esx_view top, esx_view under,
                                 float fromProgress, float toProgress,
                                 bool completesPop) {
    auto* transition = new NavTransition();
    transition->nav = handle;
    transition->top = top;
    transition->under = under;
    transition->fromProgress = fromProgress;
    transition->toProgress = toProgress;
    transition->completesPop = completesPop;
    const float distance = std::fabs(toProgress - fromProgress);
    transition->durationNanos =
        std::max(kMinSettleNanos,
                 static_cast<int64_t>(static_cast<float>(kTransitionNanos) * distance));
    transitioning = true;
    evk::ui::startAnimation(navTransitionTick, transition, navTransitionCleanup);
}

// userData 是 Navigation*：backButton 是 nav 的后代，同树同生死，
// 且事件分发与析构都在 UI 线程，不会悬空。
void drawBackArrow(esx_view button, void* userData) {
    auto* nav = static_cast<Navigation*>(userData);
    evk::ui::View* view = esxViewFromHandle(button);
    if (!nav || !view) {
        return;
    }
    const float w = view->rect.w;
    const float h = view->rect.h;
    const float cy = h * 0.5f;
    const float apex = w * 0.22f;  // 箭头尖端
    const float wing = w * 0.62f;  // 两翼
    const float halfH = h * 0.30f;
    const uint32_t color = nav->style.back_arrow_color;
    esx_draw_triangle(button, apex, cy, wing, cy - halfH, wing, cy + halfH,
                      color, color, color);
    const float shaftH = h * 0.10f;
    esx_draw_rect(button, wing - shaftH, cy - shaftH * 0.5f, w * 0.18f, shaftH, color);
}

void handleBackClick(esx_view /*button*/, void* userData) {
    auto* nav = static_cast<Navigation*>(userData);
    if (nav) {
        esx_navigation_pop(nav->handle, 1);
    }
}

// 句柄 → Navigation；句柄无效或视图不是 Navigation 时返回 nullptr。
Navigation* navigationFromHandle(esx_view nav) {
    return dynamic_cast<Navigation*>(esxViewFromHandle(nav));
}

} // namespace

extern "C" {

esx_view esx_navigation_create(float x, float y, float w, float h,
                               esx_view parent, float nav_bar_height,
                               const esx_navigation_style* style) {
    auto view = std::make_unique<Navigation>();
    view->barHeight = std::max(0.0f, nav_bar_height);
    view->style = style ? *style
                        : esx_navigation_style{0x1E293BFF, 0x334155FF, 0x334155FF,
                                               0x475569FF, 0xF8FAFCFF};

    Navigation* raw = view.get();
    const esx_view nav = esxAdoptViewNode(std::move(view), x, y, w, h, parent);
    if (nav == 0) {
        return 0;
    }

    // 页面容器在下层，导航栏后建在最上层。
    raw->container = esx_create_view(0.0f, raw->barHeight, w,
                                     std::max(0.0f, h - raw->barHeight), nav);
    if (raw->barHeight > 0.0f) {
        raw->bar = esx_create_view(0.0f, 0.0f, w, raw->barHeight, nav);
        esx_view_set_background(raw->bar, raw->style.bar_color);
        raw->barLine = esx_create_view(0.0f, raw->barHeight - 1.0f, w, 1.0f, raw->bar);
        esx_view_set_background(raw->barLine, raw->style.bar_line_color);
        const esx_button_style buttonStyle{raw->style.back_button_color,
                                           raw->style.back_button_pressed_color,
                                           raw->style.back_button_color};
        raw->backButton = esx_button_create(0.0f, 0.0f, 0.0f, 0.0f, raw->bar,
                                            &buttonStyle, handleBackClick, raw);
        esx_view_set_draw_callback(raw->backButton, drawBackArrow, raw);
        esx_view_set_visible(raw->backButton, 0);
    }
    // 立即布局一次：否则返回按钮要等首次尺寸变化才有非零 bounds。
    raw->layoutBar();
    return nav;
}

void esx_navigation_push(esx_view nav, esx_view page, int32_t animated) {
    Navigation* self = navigationFromHandle(nav);
    if (!self) {
        return;
    }
    if (self->transitioning || self->swiping) {
        EVK_LOGW("esx_navigation_push: transition in progress, ignored");
        return;
    }
    evk::ui::View* pageView = esxViewFromHandle(page);
    if (!pageView) {
        EVK_LOGW("esx_navigation_push: unknown page handle {}", page);
        return;
    }
    if (pageView->parent != nullptr || esxRootView() == pageView) {
        EVK_LOGW("esx_navigation_push: page {} must be created with parent=0", page);
        return;
    }
    evk::ui::View* container = esxViewFromHandle(self->container);
    if (!container || !esxAdoptChild(self->container, page)) {
        return;
    }

    const esx_view under = self->pages.empty() ? 0 : self->pages.back();
    self->pages.push_back(page);
    if (animated != 0 && under != 0 && container->rect.w > 0.0f) {
        // 动画 push：新页停在屏右（p=1），旧页保持可见，动画把 p 推到 0。
        esx_view_set_visible(under, 1);
        self->applyTransitionPositions(page, under, 1.0f);
        self->startTransition(page, under, 1.0f, 0.0f, false);
    } else {
        // 无动画/首页面：直接就位，旧页藏到视差后位。
        self->layoutPage(page, 0.0f);
        if (under != 0) {
            esx_view_set_visible(under, 0);
        }
    }
    self->updateBackButton();
}

void esx_navigation_pop(esx_view nav, int32_t animated) {
    Navigation* self = navigationFromHandle(nav);
    if (!self) {
        return;
    }
    if (self->transitioning || self->swiping) {
        EVK_LOGW("esx_navigation_pop: transition in progress, ignored");
        return;
    }
    if (self->pages.size() <= 1) {
        EVK_LOGW("esx_navigation_pop: cannot pop the last page");
        return;
    }
    const esx_view top = self->pages.back();
    const esx_view under = self->pages[self->pages.size() - 2];
    esx_view_set_visible(under, 1);
    if (animated != 0) {
        // 动画 pop：top 从 p=0 滑到 p=1，完成后在 finishTransition 里销毁。
        self->applyTransitionPositions(top, under, 0.0f);
        self->startTransition(top, under, 0.0f, 1.0f, true);
    } else {
        // 无动画：立即出栈、回调 on_pop 并销毁。
        self->pages.pop_back();
        self->layoutPage(under, 0.0f);
        if (self->onPop) {
            self->onPop(nav, top, self->onPopUserData);
        }
        esx_destroy_view(top);
    }
    self->updateBackButton();
}

int32_t esx_navigation_depth(esx_view nav) {
    Navigation* self = navigationFromHandle(nav);
    return self ? static_cast<int32_t>(self->pages.size()) : 0;
}

esx_view esx_navigation_top_page(esx_view nav) {
    Navigation* self = navigationFromHandle(nav);
    return self && !self->pages.empty() ? self->pages.back() : 0;
}

void esx_navigation_set_on_pop(esx_view nav, esx_navigation_pop_func on_pop,
                               void* user_data) {
    Navigation* self = navigationFromHandle(nav);
    if (!self) {
        return;
    }
    self->onPop = on_pop;
    self->onPopUserData = user_data;
}

} // extern "C"
