// ============================================================================
// Navigation：页面栈导航容器（iOS 风格）。
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

const char kNavigationType = 0;

constexpr int64_t kTransitionNanos = 280'000'000;
constexpr int64_t kMinSettleNanos = 80'000'000;
constexpr float kParallax = 0.28f;          // 下层页面视差后退比例
constexpr float kEdgeWidth = 48.0f;         // 左滑返回的左缘热区 px
constexpr float kCompleteVelocity = 400.0f; // 直接判定完成返回的右滑速度 px/s

struct NavigationState {
    esx_view nav = 0;
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
    std::weak_ptr<NavigationState> self;
};

// 一次转场：统一用 progress 表示"top 页滑出程度"，
// p=0 时 top 盖住 under，p=1 时 top 完全滑出屏右。
struct NavTransition {
    std::weak_ptr<NavigationState> state;
    esx_view nav = 0;
    esx_view top = 0;
    esx_view under = 0;
    bool completesPop = false; // 结束于 p=1 时是否完成 pop（销毁 top）
    float fromProgress = 0.0f;
    float toProgress = 0.0f;
    int64_t startTime = 0;
    int64_t durationNanos = kTransitionNanos;
};

std::shared_ptr<NavigationState> navState(esx_view nav) {
    evk::ui::View* view = esxViewFromHandle(nav);
    if (!view || view->controlType != &kNavigationType) {
        return nullptr;
    }
    return std::static_pointer_cast<NavigationState>(view->controlState);
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
void applyTransitionPositions(NavigationState& state, esx_view top, esx_view under,
                              float progress) {
    evk::ui::View* container = esxViewFromHandle(state.container);
    if (!container) {
        return;
    }
    const float w = container->rect.w;
    layoutPage(top, w * progress);
    if (under != 0) {
        layoutPage(under, -w * kParallax * (1.0f - progress));
    }
}

void layoutBar(NavigationState& state) {
    evk::ui::View* nav = esxViewFromHandle(state.nav);
    if (!nav) {
        return;
    }
    const float w = nav->rect.w;
    if (state.container != 0) {
        esx_view_set_bounds(state.container, 0.0f, state.barHeight, w,
                            std::max(0.0f, nav->rect.h - state.barHeight));
    }
    if (state.bar != 0) {
        esx_view_set_bounds(state.bar, 0.0f, 0.0f, w, state.barHeight);
    }
    if (state.backButton != 0) {
        const float buttonSize = state.barHeight * 0.6f;
        esx_view_set_bounds(state.backButton, state.barHeight * 0.2f,
                            (state.barHeight - buttonSize) * 0.5f,
                            buttonSize, buttonSize);
    }
}

void updateBackButton(NavigationState& state) {
    if (state.backButton != 0) {
        esx_view_set_visible(state.backButton, state.pages.size() > 1 ? 1 : 0);
    }
}

void finishTransition(NavigationState& state, const NavTransition* transition) {
    state.transitioning = false;
    if (transition->completesPop) {
        if (!state.pages.empty() && state.pages.back() == transition->top) {
            state.pages.pop_back();
        }
        if (transition->under != 0) {
            esx_view_set_visible(transition->under, 1);
            layoutPage(transition->under, 0.0f);
        }
        if (state.onPop) {
            state.onPop(state.nav, transition->top, state.onPopUserData);
        }
        esx_destroy_view(transition->top);
    } else {
        // push 完成或左滑取消：top 归位，under 回到隐藏位。
        layoutPage(transition->top, 0.0f);
        if (transition->under != 0) {
            esx_view_set_visible(transition->under, 0);
            applyTransitionPositions(state, transition->top, transition->under, 0.0f);
        }
    }
    updateBackButton(state);
}

bool navTransitionTick(int64_t frameTimeNanos, void* userData) {
    auto* transition = static_cast<NavTransition*>(userData);
    std::shared_ptr<NavigationState> state = transition->state.lock();
    if (!state || !esxViewFromHandle(transition->nav) || !state->transitioning) {
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
    applyTransitionPositions(*state, transition->top, transition->under, progress);
    if (t >= 1.0f) {
        finishTransition(*state, transition);
        return true;
    }
    return false;
}

void navTransitionCleanup(void* userData) {
    delete static_cast<NavTransition*>(userData);
}

void startTransition(NavigationState& state, esx_view top, esx_view under,
                     float fromProgress, float toProgress, bool completesPop) {
    auto* transition = new NavTransition();
    transition->state = state.self;
    transition->nav = state.nav;
    transition->top = top;
    transition->under = under;
    transition->fromProgress = fromProgress;
    transition->toProgress = toProgress;
    transition->completesPop = completesPop;
    const float distance = std::fabs(toProgress - fromProgress);
    transition->durationNanos =
        std::max(kMinSettleNanos,
                 static_cast<int64_t>(static_cast<float>(kTransitionNanos) * distance));
    state.transitioning = true;
    evk::ui::startAnimation(navTransitionTick, transition, navTransitionCleanup);
}

void handleNavPan(esx_view /*nav*/, const esx_view_pan_event* event, void* userData) {
    auto* state = static_cast<NavigationState*>(userData);
    if (!state || !event || state->transitioning) {
        return;
    }
    evk::ui::View* container = esxViewFromHandle(state->container);
    if (!container || container->rect.w <= 0.0f) {
        return;
    }
    const float w = container->rect.w;

    if (event->state == ESX_VIEW_PAN_BEGIN) {
        // 只有从左缘热区出发的手势才认领为返回手势。
        const float downX = event->x - event->translation_x;
        state->swiping = state->pages.size() > 1 && downX <= kEdgeWidth;
        if (state->swiping) {
            const esx_view under = state->pages[state->pages.size() - 2];
            esx_view_set_visible(under, 1);
            applyTransitionPositions(*state, state->pages.back(), under, 0.0f);
        }
        return;
    }
    if (!state->swiping || state->pages.size() < 2) {
        return;
    }

    const esx_view top = state->pages.back();
    const esx_view under = state->pages[state->pages.size() - 2];
    const float progress = std::clamp(event->translation_x / w, 0.0f, 1.0f);

    if (event->state == ESX_VIEW_PAN_UPDATE) {
        applyTransitionPositions(*state, top, under, progress);
        return;
    }
    state->swiping = false;
    if (event->state == ESX_VIEW_PAN_END) {
        // 快甩（速度超阈值）或拖过一半 → 补全返回；否则回弹取消。
        const bool complete = event->velocity_x > kCompleteVelocity || progress > 0.5f;
        startTransition(*state, top, under, progress, complete ? 1.0f : 0.0f, complete);
    } else { // ESX_VIEW_PAN_CANCEL：回弹取消
        startTransition(*state, top, under, progress, 0.0f, false);
    }
}

void drawBackArrow(esx_view button, void* userData) {
    auto* state = static_cast<NavigationState*>(userData);
    evk::ui::View* view = esxViewFromHandle(button);
    if (!state || !view) {
        return;
    }
    const float w = view->rect.w;
    const float h = view->rect.h;
    const float cy = h * 0.5f;
    const float apex = w * 0.22f;  // 箭头尖端
    const float wing = w * 0.62f;  // 两翼
    const float halfH = h * 0.30f;
    const uint32_t color = state->style.back_arrow_color;
    esx_draw_triangle(button, apex, cy, wing, cy - halfH, wing, cy + halfH,
                      color, color, color);
    const float shaftH = h * 0.10f;
    esx_draw_rect(button, wing - shaftH, cy - shaftH * 0.5f, w * 0.18f, shaftH, color);
}

void handleBackClick(esx_view /*button*/, void* userData) {
    auto* state = static_cast<NavigationState*>(userData);
    if (state) {
        esx_navigation_pop(state->nav, 1);
    }
}

void handleNavBoundsChanged(esx_view /*nav*/, void* userData) {
    auto* state = static_cast<NavigationState*>(userData);
    if (!state) {
        return;
    }
    // 尺寸变化：取消转场/手势并吸附到最终状态（进行中的 tick 会发现
    // transitioning=false 自行退场）。
    state->transitioning = false;
    state->swiping = false;
    layoutBar(*state);
    for (size_t i = 0; i < state->pages.size(); ++i) {
        const bool isTop = i + 1 == state->pages.size();
        layoutPage(state->pages[i], 0.0f);
        esx_view_set_visible(state->pages[i], isTop ? 1 : 0);
    }
}

} // namespace

extern "C" {

esx_view esx_navigation_create(float x, float y, float w, float h,
                               esx_view parent, float nav_bar_height,
                               const esx_navigation_style* style) {
    const esx_view nav = esx_create_view(x, y, w, h, parent);
    if (nav == 0) {
        return 0;
    }

    auto state = std::make_shared<NavigationState>();
    state->nav = nav;
    state->barHeight = std::max(0.0f, nav_bar_height);
    state->style = style ? *style
                         : esx_navigation_style{0x1E293BFF, 0x334155FF, 0x334155FF,
                                                0x475569FF, 0xF8FAFCFF};
    state->self = state;

    evk::ui::View* navView = esxViewFromHandle(nav);
    navView->controlType = &kNavigationType;
    navView->controlState = state;
    navView->panFunc = handleNavPan;
    navView->panUserData = state.get();
    navView->boundsChangedHandler = handleNavBoundsChanged;
    navView->boundsChangedUserData = state.get();

    // 页面容器在下层，导航栏后建在最上层。
    state->container = esx_create_view(0.0f, state->barHeight, w,
                                       std::max(0.0f, h - state->barHeight), nav);
    if (state->barHeight > 0.0f) {
        state->bar = esx_create_view(0.0f, 0.0f, w, state->barHeight, nav);
        esx_view_set_background(state->bar, state->style.bar_color);
        state->barLine = esx_create_view(0.0f, state->barHeight - 1.0f, w, 1.0f,
                                         state->bar);
        esx_view_set_background(state->barLine, state->style.bar_line_color);
        const esx_button_style buttonStyle{state->style.back_button_color,
                                           state->style.back_button_pressed_color,
                                           state->style.back_button_color};
        state->backButton = esx_button_create(0.0f, 0.0f, 0.0f, 0.0f, state->bar,
                                              &buttonStyle, handleBackClick,
                                              state.get());
        esx_view_set_draw_callback(state->backButton, drawBackArrow, state.get());
        esx_view_set_visible(state->backButton, 0);
    }
    // 立即布局一次：否则返回按钮要等首次尺寸变化才有非零 bounds。
    layoutBar(*state);
    return nav;
}

void esx_navigation_push(esx_view nav, esx_view page, int32_t animated) {
    auto state = navState(nav);
    if (!state) {
        return;
    }
    if (state->transitioning || state->swiping) {
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
    evk::ui::View* container = esxViewFromHandle(state->container);
    if (!container || !esxAdoptChild(state->container, page)) {
        return;
    }

    const esx_view under = state->pages.empty() ? 0 : state->pages.back();
    state->pages.push_back(page);
    if (animated != 0 && under != 0 && container->rect.w > 0.0f) {
        // 动画 push：新页停在屏右（p=1），旧页保持可见，动画把 p 推到 0。
        esx_view_set_visible(under, 1);
        applyTransitionPositions(*state, page, under, 1.0f);
        startTransition(*state, page, under, 1.0f, 0.0f, false);
    } else {
        // 无动画/首页面：直接就位，旧页藏到视差后位。
        layoutPage(page, 0.0f);
        if (under != 0) {
            esx_view_set_visible(under, 0);
        }
    }
    updateBackButton(*state);
}

void esx_navigation_pop(esx_view nav, int32_t animated) {
    auto state = navState(nav);
    if (!state) {
        return;
    }
    if (state->transitioning || state->swiping) {
        EVK_LOGW("esx_navigation_pop: transition in progress, ignored");
        return;
    }
    if (state->pages.size() <= 1) {
        EVK_LOGW("esx_navigation_pop: cannot pop the last page");
        return;
    }
    const esx_view top = state->pages.back();
    const esx_view under = state->pages[state->pages.size() - 2];
    esx_view_set_visible(under, 1);
    if (animated != 0) {
        // 动画 pop：top 从 p=0 滑到 p=1，完成后在 finishTransition 里销毁。
        applyTransitionPositions(*state, top, under, 0.0f);
        startTransition(*state, top, under, 0.0f, 1.0f, true);
    } else {
        // 无动画：立即出栈、回调 on_pop 并销毁。
        state->pages.pop_back();
        layoutPage(under, 0.0f);
        if (state->onPop) {
            state->onPop(nav, top, state->onPopUserData);
        }
        esx_destroy_view(top);
    }
    updateBackButton(*state);
}

int32_t esx_navigation_depth(esx_view nav) {
    auto state = navState(nav);
    return state ? static_cast<int32_t>(state->pages.size()) : 0;
}

esx_view esx_navigation_top_page(esx_view nav) {
    auto state = navState(nav);
    return state && !state->pages.empty() ? state->pages.back() : 0;
}

void esx_navigation_set_on_pop(esx_view nav, esx_navigation_pop_func on_pop,
                               void* user_data) {
    auto state = navState(nav);
    if (!state) {
        return;
    }
    state->onPop = on_pop;
    state->onPopUserData = user_data;
}

} // extern "C"
