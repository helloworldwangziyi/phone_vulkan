/**
 * @file navigation.cpp
 * @brief Navigation：页面栈导航容器（iOS 风格）。
 *
 * 继承 View：自身即导航容器，重写钩子获得导航行为——
 * - acceptsPanInput/handlePan → 左缘滑动手势驱动交互式返回；
 * - handleBoundsChanged       → 尺寸变化时取消转场并重排到最终状态。
 *
 * 视图结构：
 *   nav view（通常作为根视图）
 *   ├─ container  页面容器（导航栏以下区域），页面都是它的直接子视图；
 *   │             页面滑出容器会被自动裁剪（父链 intersect 的 clip 机制）
 *   └─ bar        导航栏（后添加 → 永远在页面上层）
 *       ├─ barLine     底部分隔线
 *       └─ backButton  返回按钮（Button 控件 + drawFunc 画箭头），栈深 >1 可见
 *
 * 转场统一用"顶层页面滑出进度 p"表示（p=0 盖严，p=1 完全滑出屏右）：
 *   push      = p: 1 → 0（新页从右滑入，旧页按 kParallax 比例视差后退）；
 *   pop       = p: 0 → 1（顶层滑出，下层归位，完成后销毁顶层）；
 *   左滑返回  = 手指直接驱动 p，松手按速度（>400px/s）或进度（>0.5）
 *               决定补全（同 pop）还是回弹（回到 p=0）。
 * 三条路径最终都汇入同一个 NavTransition 时间动画，时长按剩余进度折算。
 *
 * 页面所有权：App 以 parent=0 创建页面，push 时经 esxAdoptChild 把
 * unique_ptr 移交给容器；pop 动画完成后由 Navigation 销毁页面
 * （销毁前先回调 on_pop，让 App 清理挂在页面上的自有数据）。
 * 转场进行中忽略新的 push/pop/返回手势；尺寸变化时取消转场并吸附到最终状态。
 *
 * 页面生命周期钩子（对应 estarx App view_init_func 的 8 态，方向用 forward 区分）：
 *   push：        旧页 WILL_LEAVE → 新页 WILL_ENTER →（转场）→ 旧页 DID_LEAVE → 新页 DID_ENTER
 *   pop/左滑返回：顶页 WILL_LEAVE → 下页 WILL_ENTER →（转场）→ 顶页 DID_LEAVE → 下页 DID_ENTER
 * WILL_* 返回非 0 取消本次导航；左滑回弹/尺寸吸附取消转场时按最终归属收尾
 * （台前页 DID_ENTER、另一页 DID_LEAVE），每个 WILL 严格配对一次 DID。
 *
 * 生命周期：转场上下文（NavTransition）只存 nav 句柄，tick 时用
 * dynamic_cast 重新解析，不留可能悬空的 View 指针；返回按钮回调里的
 * Navigation* 原始指针安全——backButton 是 nav 的后代，同树同生死，
 * 单线程模型下析构期间不会有事件分发。
 */

#include "evk/ui/controls/navigation.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

#include "evk/log.h"
#include "evk/render_loop.h"
#include "evk/ui/animator.h"
#include "evk/ui/controls/button.h"
#include "evk/ui/view.h"

namespace {

constexpr int64_t kTransitionNanos = 280'000'000; ///< 转场动画基准时长 ns（时长按剩余进度折算）
constexpr int64_t kMinSettleNanos = 80'000'000; ///< 转场最短时长 ns（折算下限）
constexpr float kParallax = 0.28f; ///< 下层页面视差后退比例
constexpr float kEdgeWidth = 48.0f; ///< 左滑返回的左缘热区 px
constexpr float kCompleteVelocity = 400.0f; ///< 直接判定完成返回的右滑速度 px/s

/**
 * @brief Navigation 实现：自身即导航容器，重写钩子获得导航行为。
 */
class Navigation : public evk::ui::View {
public:
    esx_view container = 0; ///< 页面容器（导航栏以下区域），页面都是它的子视图；
                            /// 页面滑出 container 会被父链 clip 自动裁剪
    esx_view bar = 0; ///< 导航栏（后创建 → 永远画在页面上层）
    esx_view barLine = 0; ///< 栏底分隔线
    esx_view backButton = 0; ///< 返回按钮（Button 控件 + draw 回调画箭头），栈深>1 可见
    float barHeight = 0.0f; ///< 导航栏高度 px
    esx_navigation_style style{}; ///< 导航栏配色（换肤经 esx_navigation_set_style 就地更新）
    std::vector<esx_view> pages; ///< 页面栈（栈底 → 栈顶），只存句柄不拥有对象
    bool transitioning = false; ///< 转场动画进行中：忽略新的 push/pop/返回手势
    bool swiping = false; ///< 左滑返回手势已认领且进行中
    esx_navigation_pop_func onPop = nullptr; ///< 页面销毁前回调（App 清理自有数据）
    void* onPopUserData = nullptr; ///< on_pop 的 user_data
    /// 待配对的 WILL 钩子上下文：fireWillHooks 记录，fireDidHooks 消费后清除；
    /// 保证每个 WILL 严格配对一次 DID（含转场取消时的归属收尾）。
    esx_view hookLeave = 0; ///< 收到 WILL_LEAVE 的页面
    esx_view hookEnter = 0; ///< 收到 WILL_ENTER 的页面
    int32_t hookForward = 0; ///< 本次导航方向（1=push，0=pop）

    /**
     * @brief 触发页面导航生命周期钩子；页面无回调返回 0。
     *
     * WILL_* 返回非 0 表示页面取消本次导航。
     */
    int32_t firePageHook(esx_view page, esx_view_nav_event event, int32_t forward) const {
        evk::ui::View* view = esxViewFromHandle(page);
        if (!view || !view->navFunc) {
            return 0;
        }
        return view->navFunc(handle, page, event, forward, view->navUserData);
    }

    /**
     * @brief 转场开始前发 WILL 对（leave 先于 enter）。
     *
     * 任一页面取消时给已收到 WILL_LEAVE 的页面补发 DID_ENTER（你留在台前）
     * 配对收尾，返回 false。成功时记录上下文，由 fireDidHooks 配对。
     * @note 钩子里改跳其他页面时须取消原导航，否则嵌套导航的上下文会被覆盖
     *       （配对打破，告警）。
     */
    bool fireWillHooks(esx_view leavePage, esx_view enterPage, int32_t forward) {
        if (hookLeave != 0 || hookEnter != 0) {
            EVK_LOGW("navigation: nested navigation from page hook without canceling");
        }
        if (leavePage != 0 &&
            firePageHook(leavePage, ESX_VIEW_NAV_WILL_LEAVE, forward) != 0) {
            return false;
        }
        if (enterPage != 0 &&
            firePageHook(enterPage, ESX_VIEW_NAV_WILL_ENTER, forward) != 0) {
            if (leavePage != 0) {
                firePageHook(leavePage, ESX_VIEW_NAV_DID_ENTER, forward);
            }
            return false;
        }
        hookLeave = leavePage;
        hookEnter = enterPage;
        hookForward = forward;
        return true;
    }

    /**
     * @brief 转场落定（含回弹/吸附取消）：loser 收 DID_LEAVE 先于 winner 收 DID_ENTER。
     *
     * 与 fireWillHooks 记录的 WILL 对配对后清除上下文。上下文为空时无操作。
     */
    void fireDidHooks(esx_view winner, esx_view loser) {
        if (hookLeave == 0 && hookEnter == 0) {
            return;
        }
        const int32_t forward = hookForward;
        hookLeave = 0;
        hookEnter = 0;
        if (loser != 0) {
            firePageHook(loser, ESX_VIEW_NAV_DID_LEAVE, forward);
        }
        if (winner != 0) {
            firePageHook(winner, ESX_VIEW_NAV_DID_ENTER, forward);
        }
    }

    bool acceptsPanInput() const override { return true; }

    /**
     * @brief 左缘滑动返回手势（iOS 风格交互式 pop）。
     *
     * 关键：Navigation 与 ScrollView 都是 pan 候选，区分靠"认领条件"——
     * BEGIN 时反推手指按下位置（x - translation_x），只有落在左缘 kEdgeWidth
     * 热区内且栈深>1 才认领（swiping=true）；否则整个手势让给 ScrollView。
     * 认领后手指直接驱动转场进度 p = translation_x / 容器宽
     * （0 = 盖严，1 = 完全滑出屏右）；松手按"快甩(>400px/s) 或 拖过半(>0.5)"
     * 决定补全返回，否则回弹。
     */
    void handlePan(const esx_view_pan_event& event) override {
        if (transitioning) {
            return; // 转场进行中：忽略新手势
        }
        evk::ui::View* containerView = esxViewFromHandle(container);
        if (!containerView || containerView->rect.w <= 0.0f) {
            return;
        }
        const float w = containerView->rect.w; // 转场距离单位 = 容器宽度

        if (event.state == ESX_VIEW_PAN_BEGIN) {
            // 反推 DOWN 位置：event.x 是当前手指位置（相对本视图），
            // translation_x 是 DOWN 以来总位移，二者相减即按下点。
            const float downX = event.x - event.translation_x;
            swiping = pages.size() > 1 && downX <= kEdgeWidth;
            if (swiping) {
                const esx_view top = pages.back();
                const esx_view under = pages[pages.size() - 2];
                // 页面可在 WILL 钩子里取消本次返回（如表单未保存）。
                if (!fireWillHooks(top, under, 0)) {
                    swiping = false;
                    return;
                }
                esx_view_set_visible(under, 1); // 露出下层页面
                applyTransitionPositions(top, under, 0.0f); // 初始：顶层盖严
            }
            return;
        }
        if (!swiping || pages.size() < 2) {
            return; // 未认领（不是返回手势）或栈不足：忽略
        }

        const esx_view top = pages.back();
        const esx_view under = pages[pages.size() - 2];
        // 转场进度统一用 p 表示：顶层页面滑出屏右的程度（0 盖严，1 完全滑出）。
        const float progress = std::clamp(event.translation_x / w, 0.0f, 1.0f);

        if (event.state == ESX_VIEW_PAN_UPDATE) {
            // 手指直接驱动两页位置（顶层右移，下层从视差位归位）。
            applyTransitionPositions(top, under, progress);
            return;
        }
        swiping = false;
        if (event.state == ESX_VIEW_PAN_END) {
            // 快甩（速度超阈值）或拖过一半 → 补全返回；否则回弹取消。
            const bool complete = event.velocity_x > kCompleteVelocity || progress > 0.5f;
            startTransition(top, under, progress, complete ? 1.0f : 0.0f, complete);
        } else { // ESX_VIEW_PAN_CANCEL：回弹取消（回到 p=0，顶页留在台前）
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
        // 吸附后栈顶留在台前：按"栈顶 winner"给待配对的 WILL 对收尾。
        if (!pages.empty()) {
            const esx_view winner = pages.back();
            fireDidHooks(winner, winner == hookLeave ? hookEnter : hookLeave);
        }
    }

    /**
     * @brief 页面在容器坐标内定位：{x, 0, 容器宽, 容器高}——页面永远铺满容器，
     * 转场只改 x。
     *
     * 页面本身是 Column 等 Flex 容器，set_bounds 会经 handleBoundsChanged
     * 级联重排页面内容，所以页面内部布局随页面移动自动保持，不需要额外的
     * layout 调用。
     */
    void layoutPage(esx_view page, float x) {
        evk::ui::View* view = esxViewFromHandle(page);
        if (!view || !view->parent) {
            return;
        }
        esx_view_set_bounds(page, x, 0.0f, view->parent->rect.w, view->parent->rect.h);
    }

    /**
     * @brief 按进度 p 摆放两页：top 从盖严（x=0）滑到屏右（x=w）；
     * under 从视差后位（-w×kParallax）归位（x=0），两页镜像联动。
     *
     * 所有转场（push/pop/左滑）都收敛到这一组位置公式，
     * 动画 tick 与手势拖拽共用，保证手指驱动与动画驱动手感一致。
     */
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

    /**
     * @brief 导航结构布局：nav 自身 rect 下分两块——
     *   container：导航栏以下区域（页面容器），页面都是它的直接子视图，
     *              页面滑出它会被父链 clip 自动裁剪；
     *   bar：顶部导航栏（后创建 = 永远画在页面上层），内含底部分隔线与返回按钮。
     *
     * 由创建时与 handleBoundsChanged 调用（尺寸变化重排并取消转场）；
     * 返回按钮大小按栏高比例折算（barHeight×0.6）。
     */
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

    /// 按栈深刷新返回按钮可见性（栈深 >1 时显示）。
    void updateBackButton() {
        if (backButton != 0) {
            esx_view_set_visible(backButton, pages.size() > 1 ? 1 : 0);
        }
    }

    void startTransition(esx_view top, esx_view under,
                         float fromProgress, float toProgress, bool completesPop);
};

/**
 * @brief 一次转场的上下文：统一用 progress 表示"top 页滑出程度"，
 * p=0 时 top 盖住 under，p=1 时 top 完全滑出屏右。
 *
 * 与 ScrollAnimation 同样的句柄-only 安全约定：tick 时重解析 nav，
 * 不留悬空指针。push/pop/左滑三条路径统一复用。
 */
struct NavTransition {
    esx_view nav = 0; ///< 目标 Navigation 句柄（每次 tick 重解析）
    esx_view top = 0; ///< 顶层页面（进度 p 作用于它）
    esx_view under = 0; ///< 下层页面（视差联动）
    bool completesPop = false; ///< 结束于 p=1 时是否完成 pop（销毁 top 页面）
    float fromProgress = 0.0f; ///< 起始进度（左滑转场从手势松手时的 p 续接）
    float toProgress = 0.0f; ///< 目标进度（0=盖严/归位，1=完全滑出）
    int64_t startTime = 0; ///< 0=未开始（首帧记录，避开注册间隙）
    int64_t durationNanos = kTransitionNanos; ///< 按剩余进度折算（startTransition）
};

/**
 * @brief 转场落定收尾。
 *
 * completesPop 时销毁 top 页（先配对发 DID、回调 on_pop）；否则 top 归位、
 * under 藏回隐藏位。最后刷新返回按钮可见性。
 */
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
        // 页面销毁前发配对 DID：顶页 DID_LEAVE（页面仍有效）→ 下页 DID_ENTER。
        nav.fireDidHooks(transition->under, transition->top);
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
        // push 落定与左滑回弹都是 top 留在台前：under DID_LEAVE → top DID_ENTER。
        nav.fireDidHooks(transition->top, transition->under);
    }
    nav.updateBackButton();
}

/// 转场动画 tick：easeOutCubic 推进进度，t≥1 时 finishTransition 落定。
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

/// 转场结束清理：释放 NavTransition 上下文。
void navTransitionCleanup(void* userData) {
    delete static_cast<NavTransition*>(userData);
}

/// 启动转场时间动画：时长按剩余进度折算（下限 kMinSettleNanos）。
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

/**
 * @brief 画返回箭头（draw 回调）。
 *
 * userData 是 Navigation*：backButton 是 nav 的后代，同树同生死，
 * 且事件分发与析构都在 UI 线程，不会悬空。
 */
void drawBackArrow(esx_view button, void* userData) {
    auto* nav = static_cast<Navigation*>(userData);
    evk::ui::View* view = esxViewFromHandle(button);
    if (!nav || !view) {
        return;
    }
    const float w = view->rect.w;
    const float h = view->rect.h;
    const float cy = h * 0.5f;
    const float apex = w * 0.22f; // 箭头尖端
    const float wing = w * 0.62f; // 两翼
    const float halfH = h * 0.30f;
    const uint32_t color = nav->style.back_arrow_color;
    esx_draw_triangle(button, apex, cy, wing, cy - halfH, wing, cy + halfH,
                      color, color, color);
    const float shaftH = h * 0.10f;
    esx_draw_rect(button, wing - shaftH, cy - shaftH * 0.5f, w * 0.18f, shaftH, color);
}

/// 返回按钮点击：动画 pop 栈顶页面。
void handleBackClick(esx_view /*button*/, void* userData) {
    auto* nav = static_cast<Navigation*>(userData);
    if (nav) {
        esx_navigation_pop(nav->handle, 1);
    }
}

/// 句柄 → Navigation；句柄无效或视图不是 Navigation 时返回 nullptr。
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
    if (!container) {
        return;
    }
    // 接管前发 WILL 对：旧页 WILL_LEAVE → 新页 WILL_ENTER；页面可取消 push
    // （取消时 page 未被接管，App 自行销毁或复用）。
    const esx_view under = self->pages.empty() ? 0 : self->pages.back();
    if (!self->fireWillHooks(under, page, 1)) {
        EVK_LOGI("esx_navigation_push: canceled by page hook");
        return;
    }
    if (!esxAdoptChild(self->container, page)) {
        // 接管失败：按"旧页留在台前"给已发的 WILL 对配对收尾。
        self->fireDidHooks(under, page);
        return;
    }

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
        self->fireDidHooks(page, under);
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
    // 转场前发 WILL 对：顶页 WILL_LEAVE → 下页 WILL_ENTER；页面可取消返回。
    if (!self->fireWillHooks(top, under, 0)) {
        EVK_LOGI("esx_navigation_pop: canceled by page hook");
        return;
    }
    esx_view_set_visible(under, 1);
    if (animated != 0) {
        // 动画 pop：top 从 p=0 滑到 p=1，完成后在 finishTransition 里销毁。
        self->applyTransitionPositions(top, under, 0.0f);
        self->startTransition(top, under, 0.0f, 1.0f, true);
    } else {
        // 无动画：立即出栈、发配对 DID、回调 on_pop 并销毁。
        self->pages.pop_back();
        self->layoutPage(under, 0.0f);
        self->fireDidHooks(under, top);
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

void esx_navigation_set_style(esx_view nav, const esx_navigation_style* style) {
    Navigation* self = navigationFromHandle(nav);
    if (!self || !style) {
        return;
    }
    self->style = *style;
    if (self->bar != 0) {
        esx_view_set_background(self->bar, style->bar_color);
    }
    if (self->barLine != 0) {
        esx_view_set_background(self->barLine, style->bar_line_color);
    }
    if (self->backButton != 0) {
        const esx_button_style buttonStyle{style->back_button_color,
                                           style->back_button_pressed_color,
                                           style->back_button_color};
        esx_button_set_style(self->backButton, &buttonStyle);
    }
    evk::requestRender();
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
