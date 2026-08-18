/**
 * @file navigation_stack.cpp
 * @brief 导航栈实现：页面栈 + 导航栏 + push/pop 转场。
 *
 * @details 两层结构：
 *
 *   - **NavigationView**（匿名命名空间）：导航栈的实体层。持有四个固定
 *     孩子——content（页面容器，路由页都挂在这里）、bar、barLine、
 *     backButton，外加转场状态机（transitioning / transitionKind /
 *     popLifecycleActive）。
 *     进入转场有两条路径：push()、pop()。返回不由本层造手势：
 *     平台壳把系统返回（Android 返回键/手势、iOS 边缘手势）统一上报为
 *     EventId::BackPressed，App 层据此调 Navigator::pop。
 *   - **Navigator / Navigator::Impl**：公开 API 与路由表。Route 把页面
 *     element 与其根 View 配对；RouteEvent 经 lifecycle 回调按 View
 *     找到路由后转发给页面 element。
 *
 * 生命周期约定（与 widget_tree.h 的 RouteEvent 注释对应）：
 *   - 每个 Will* 严格配对一次 Did*；Will* 返回 false 否决本次导航，
 *     否决后要给对方补一发 DidEnter，冲销已下发的 WillLeave；
 *   - 转场被取消（转场中尺寸变化强制落地）时按最终归属收尾。
 */
#include "evk/ui/navigation/navigation_stack.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

#include "evk/log.h"
#include "evk/frame_scheduler.h"
#include "evk/ui/animation_scheduler.h"
#include "evk/ui/controls/button_control.h"
#include "evk/ui/render_view.h"
#include "evk/ui/widget_tree.h"

namespace evk::ui {
namespace {

constexpr float kParallax = 0.28f;         ///< 转场中底层页的视差位移系数
constexpr int64_t kTransitionDurationNanos = 280'000'000;  ///< 满程转场时长

enum class TransitionKind {
    None,
    Push,
    Pop,
};

/**
 * @brief 导航栈实体：页面栈 + 导航栏 chrome + 转场状态机。
 *
 * 四个固定孩子（构造时建好、永不增删）：content（页面容器，路由页的
 * 直接父实体）、bar、barLine、backButton；页面只在 content 之下进出。
 *
 * 状态机字段分工：
 *   - transitioning / transitionKind：转场进行中及其方向，期间拒绝新的
 *     push/pop；
 *   - popLifecycleActive：pop 方向的 Will* 对已下发、等待配对 Did* 的
 *     标记——既是防重入锁，也是 finishPop 识别「迟到 finish」的判据。
 */
class NavigationView final : public View {
public:
    explicit NavigationView(float barHeight, NavigationStyle initialStyle)
        : navigationBarHeight(std::max(0.0f, barHeight)),
          style(initialStyle) {
        // 固定孩子的创建顺序即布局依赖：content 在最底，bar 浮于其上，
        // barLine 与 backButton 是 bar 的孩子。
        content = addChild(std::make_unique<View>());
        bar = addChild(std::make_unique<View>());
        barLine = bar->addChild(std::make_unique<View>());
        backButton = bar->addChild(createButtonView(
            backButtonStyle(), [this] {
                if (requestPop) {
                    requestPop();
                }
            }));
        // 返回键箭头：painter 画一个朝左的三角形，颜色随 style。
        backButton->painter = [this](PaintContext& context) {
            const Size size = context.size();
            const float centerX = size.width * 0.50f;
            const float centerY = size.height * 0.50f;
            const float half = std::min(size.width, size.height) * 0.16f;
            context.drawTriangle(
                centerX + half, centerY - half,
                centerX - half, centerY,
                centerX + half, centerY + half,
                style.backArrowColor,
                style.backArrowColor,
                style.backArrowColor);
        };
        applyStyle();
        updateBar();
    }

    View* contentView() const { return content; }

    /**
     * @brief 重排 chrome 与全部页面；转场中被调到意味着尺寸真的变了
     *        （旋转 / Surface 重建），直接把转场落到终态：
     *        Push → finishPush（按完成落地），Pop → finishPop(false)（回滚）。
     *
     * 注意：正在跑的动画 tick 不会被取消，到点后还会再调一次 finish。
     * finishPop 靠 popLifecycleActive 挡住重复执行；finishPush 没有
     * 这个守卫，重复落地会重复下发 DidLeave/DidEnter——审查时注意这点。
     */
    void handleBoundsChanged() override {
        if (!content || !bar) {
            return;
        }
        content->setBounds(
            0.0f,
            navigationBarHeight,
            rect.w,
            std::max(0.0f, rect.h - navigationBarHeight));
        bar->setBounds(0.0f, 0.0f, rect.w, navigationBarHeight);
        barLine->setBounds(
            0.0f,
            std::max(0.0f, navigationBarHeight - 1.0f),
            rect.w,
            navigationBarHeight > 0.0f ? 1.0f : 0.0f);
        const float buttonWidth = navigationBarHeight > 0.0f
            ? std::min(navigationBarHeight, 96.0f)
            : 0.0f;
        backButton->setBounds(0.0f, 0.0f, buttonWidth, navigationBarHeight);
        for (View* page : pages) {
            if (page) {
                page->setBounds(0.0f, 0.0f, content->rect.w, content->rect.h);
            }
        }
        if (transitioning) {
            if (transitionKind == TransitionKind::Push) {
                finishPush();
            } else if (transitionKind == TransitionKind::Pop) {
                finishPop(false);
            }
        }
    }

    /**
     * @brief 推入页面。调用前页面必须已是 content 的孩子
     *        （Navigator::push 负责先 mount 挂好）。
     *
     * 生命周期顺序：上一页 WillLeave → 新页 WillEnter，两者都可否决；
     * 新页被否决时给上一页补一发 DidEnter，冲销它已收到的 WillLeave。
     * 首页或非动画：立即落终态并当场补发 Did*；动画转场：终态事件
     * （上一页 DidLeave、新页 DidEnter）延后到 finishPush 下发。
     */
    bool push(View& page, bool animated) {
        if (transitioning || page.parent != content) {
            return false;
        }
        View* previous = pages.empty() ? nullptr : pages.back();
        if (previous && lifecycle &&
            !lifecycle(previous, RouteEvent::WillLeave, true)) {
            return false;
        }
        if (lifecycle && !lifecycle(&page, RouteEvent::WillEnter, true)) {
            if (previous && lifecycle) {
                lifecycle(previous, RouteEvent::DidEnter, true);
            }
            return false;
        }

        pages.push_back(&page);
        page.setVisible(true);
        page.setBounds(
            previous && animated ? content->rect.w : 0.0f,
            0.0f,
            content->rect.w,
            content->rect.h);
        updateBar();

        if (!previous || !animated) {
            if (previous) {
                previous->setBounds(0.0f, 0.0f, content->rect.w, content->rect.h);
                previous->setVisible(false);
                if (lifecycle) {
                    lifecycle(previous, RouteEvent::DidLeave, true);
                }
            }
            page.setBounds(0.0f, 0.0f, content->rect.w, content->rect.h);
            if (lifecycle) {
                lifecycle(&page, RouteEvent::DidEnter, true);
            }
            return true;
        }

        transitioning = true;
        transitionKind = TransitionKind::Push;
        animate(0.0f, 1.0f, [this](float progress) {
            applyPushProgress(progress);
        }, [this] {
            finishPush();
        });
        return true;
    }

    /**
     * @brief 弹出栈顶。beginPopLifecycle 先下 pop 方向的 Will* 对
     *        （可否决）；非动画直接 finishPop(true) 同步完成，
     *        动画则交给 animatePop 从进度 0 走向 1。
     */
    bool pop(bool animated) {
        if (transitioning || pages.size() <= 1 || !beginPopLifecycle()) {
            return false;
        }
        if (!animated) {
            finishPop(true);
            return true;
        }
        transitioning = true;
        transitionKind = TransitionKind::Pop;
        animatePop(0.0f, true);
        return true;
    }

    void setStyle(const NavigationStyle& value) {
        style = value;
        applyStyle();
    }

    // 清空页面栈与全部转场状态。正在跑的动画 tick 不会被取消，之后
    // 仍可能落地一次 finish——靠 finishPush 的空栈返回、finishPop 的
    // popLifecycleActive 守卫挡住，不会误发生命周期事件。
    void clearPages() {
        pages.clear();
        transitioning = false;
        popLifecycleActive = false;
        transitionKind = TransitionKind::None;
        updateBar();
    }

    size_t depth() const { return pages.size(); }
    View* topPage() const { return pages.empty() ? nullptr : pages.back(); }

    std::function<bool(View*, RouteEvent, bool)> lifecycle;
    std::function<void(View*)> popped;
    std::function<void()> requestPop;

private:
    ButtonStyle backButtonStyle() const {
        return {
            style.backButtonColor,
            style.backButtonPressedColor,
            style.backButtonColor,
        };
    }

    void applyStyle() {
        if (bar) {
            bar->setBackground(style.barColor);
        }
        if (barLine) {
            barLine->setBackground(style.barLineColor);
        }
        if (backButton) {
            updateButtonView(*backButton, backButtonStyle(), [this] {
                if (requestPop) {
                    requestPop();
                }
            });
        }
        requestRender();
    }

    void updateBar() {
        if (bar) {
            bar->setVisible(navigationBarHeight > 0.0f);
        }
        if (backButton) {
            backButton->setVisible(navigationBarHeight > 0.0f && pages.size() > 1);
        }
    }

    /**
     * @brief pop 方向的两页 Will* 下发：栈顶 WillLeave → 下层 WillEnter。
     *
     * 任一被否决都放弃本次 pop；下层否决时给栈顶补一发 DidEnter，
     * 冲销它已收到的 WillLeave。成功后置 popLifecycleActive（防重入），
     * 并让下层页提前可见——转场过程中要露出它。
     */
    bool beginPopLifecycle() {
        if (pages.size() <= 1 || popLifecycleActive) {
            return false;
        }
        View* top = pages.back();
        View* below = pages[pages.size() - 2];
        if (lifecycle && !lifecycle(top, RouteEvent::WillLeave, false)) {
            return false;
        }
        if (lifecycle && !lifecycle(below, RouteEvent::WillEnter, false)) {
            if (lifecycle) {
                lifecycle(top, RouteEvent::DidEnter, false);
            }
            return false;
        }
        popLifecycleActive = true;
        below->setVisible(true);
        return true;
    }

    // push 视差：新页从右缘外滑入（x: width→0），旧页向左让出 0.28 视差。
    void applyPushProgress(float progress) {
        if (pages.size() < 2) {
            return;
        }
        const float width = content->rect.w;
        View* next = pages.back();
        View* previous = pages[pages.size() - 2];
        previous->setVisible(true);
        previous->setBounds(
            -width * kParallax * progress, 0.0f, width, content->rect.h);
        next->setBounds(
            width * (1.0f - progress), 0.0f, width, content->rect.h);
    }

    // pop 视差：栈顶向右滑出（x: 0→width），下层从 -0.28 视差处归位。
    void applyPopProgress(float progress) {
        if (pages.size() < 2) {
            return;
        }
        const float width = content->rect.w;
        View* top = pages.back();
        View* below = pages[pages.size() - 2];
        below->setVisible(true);
        below->setBounds(
            -width * kParallax * (1.0f - progress),
            0.0f,
            width,
            content->rect.h);
        top->setBounds(width * progress, 0.0f, width, content->rect.h);
    }

    /**
     * @brief push 落地：两页归位、旧页隐藏，补发旧页 DidLeave +
     *        新页 DidEnter，然后复位转场标志。
     *
     * 无重入守卫：转场中尺寸变化被 handleBoundsChanged 强制落地后，
     * 动画 tick 到点会再调一次本函数，Did* 随之重复下发。
     */
    void finishPush() {
        if (pages.empty()) {
            return;
        }
        View* next = pages.back();
        next->setBounds(0.0f, 0.0f, content->rect.w, content->rect.h);
        if (pages.size() > 1) {
            View* previous = pages[pages.size() - 2];
            previous->setBounds(0.0f, 0.0f, content->rect.w, content->rect.h);
            previous->setVisible(false);
            if (lifecycle) {
                lifecycle(previous, RouteEvent::DidLeave, true);
            }
        }
        if (lifecycle) {
            lifecycle(next, RouteEvent::DidEnter, true);
        }
        transitioning = false;
        transitionKind = TransitionKind::None;
    }

    /**
     * @brief pop 落地。completed=false 是回滚：栈顶留下、下层重新隐藏，
     *        按「下层 DidLeave + 栈顶 DidEnter」冲销 beginPopLifecycle
     *        下发的 Will* 对；completed=true 才真正出栈：栈顶 DidLeave、
     *        下层 DidEnter，再经 popped 回调销毁栈顶页。
     *
     * popLifecycleActive 守卫：clearPages 或强制回滚之后，迟到的动画
     * finish 落到这里只复位标志并返回，不会重复下发事件。
     */
    void finishPop(bool completed) {
        if (pages.size() < 2 || !popLifecycleActive) {
            transitioning = false;
            transitionKind = TransitionKind::None;
            return;
        }
        View* top = pages.back();
        View* below = pages[pages.size() - 2];
        if (!completed) {
            top->setBounds(0.0f, 0.0f, content->rect.w, content->rect.h);
            below->setBounds(0.0f, 0.0f, content->rect.w, content->rect.h);
            below->setVisible(false);
            if (lifecycle) {
                lifecycle(below, RouteEvent::DidLeave, false);
                lifecycle(top, RouteEvent::DidEnter, false);
            }
        } else {
            pages.pop_back();
            below->setBounds(0.0f, 0.0f, content->rect.w, content->rect.h);
            below->setVisible(true);
            if (lifecycle) {
                lifecycle(top, RouteEvent::DidLeave, false);
                lifecycle(below, RouteEvent::DidEnter, false);
            }
            updateBar();
            if (popped) {
                popped(top);
            }
        }
        popLifecycleActive = false;
        transitioning = false;
        transitionKind = TransitionKind::None;
    }

    // pop 转场统一入口：from 是当前进度，complete 决定走向 1
    // （完成出栈）还是弹回 0（撤销）。
    void animatePop(float from, bool complete) {
        transitioning = true;
        transitionKind = TransitionKind::Pop;
        const float to = complete ? 1.0f : 0.0f;
        animate(from, to, [this](float progress) {
            applyPopProgress(progress);
        }, [this, complete] {
            finishPop(complete);
        });
    }

    /**
     * @brief 转场动画驱动：时长按 |to-from| 折算（从中间进度续转只走
     *        剩余路程），easeOutCubic 插值。
     *
     * tick 约定：返回 true 注销动画；最后一帧（t>=1）先 apply(to) 定格
     * 再 finish()。navigation 的 ViewRef 失效（视图已销毁）时直接
     * 注销——状态标志随视图一起销毁，无需补 finish。
     */
    void animate(
        float from,
        float to,
        std::function<void(float)> apply,
        std::function<void()> finish) {
        struct Animation {
            ViewRef navigation;
            float from = 0.0f;
            float to = 1.0f;
            int64_t start = 0;
            int64_t duration = kTransitionDurationNanos;
            std::function<void(float)> apply;
            std::function<void()> finish;
        };
        auto animation = std::make_shared<Animation>();
        animation->navigation = ref();
        animation->from = from;
        animation->to = to;
        animation->duration = std::max<int64_t>(
            1,
            static_cast<int64_t>(
                static_cast<float>(kTransitionDurationNanos) * std::fabs(to - from)));
        animation->apply = std::move(apply);
        animation->finish = std::move(finish);
        startAnimation([animation](int64_t frameTimeNanos) {
            if (!animation->navigation.get()) {
                return true;
            }
            if (animation->start == 0) {
                animation->start = frameTimeNanos;
            }
            const float t = static_cast<float>(frameTimeNanos - animation->start) /
                            static_cast<float>(animation->duration);
            if (t >= 1.0f) {
                animation->apply(animation->to);
                animation->finish();
                return true;
            }
            const float eased = easeOutCubic(std::clamp(t, 0.0f, 1.0f));
            animation->apply(
                animation->from + (animation->to - animation->from) * eased);
            return false;
        });
    }

    View* content = nullptr;   ///< 页面容器：路由页的直接父实体
    View* bar = nullptr;       ///< 导航栏（分隔线与返回键的父实体）
    View* barLine = nullptr;   ///< 导航栏底部 1px 分隔线
    View* backButton = nullptr;  ///< 返回键：点击走 requestPop → Navigator::pop
    float navigationBarHeight = 0.0f;
    NavigationStyle style;
    std::vector<View*> pages;  ///< 页面栈：栈底在前、栈顶在后
    bool transitioning = false;  ///< 转场进行中
    bool popLifecycleActive = false;  ///< pop 的 Will* 对已下发、待配对
    TransitionKind transitionKind = TransitionKind::None;
};

} // namespace

/**
 * @brief Navigator 的实现核：路由表 + 三根回调线的接线。
 *
 * Route 把页面 element（工位子树）与其根 View（实体）配对；
 * 三个回调把 NavigationView 连回上层：
 *   - requestPop：返回键点击 → Navigator::pop(true)；
 *   - lifecycle ：RouteEvent 按 View 找到路由后转发给页面 element；
 *                 页面不属于本栈（找不到路由）时放行，不拦截；
 *   - popped    ：pop 完成后销毁页面——unmount element 并摘出路由表，
 *                 这是 pop 方向页面的唯一销毁点。
 */
class Navigator::Impl {
public:
    struct Route {
        std::unique_ptr<Element> element;
        View* view = nullptr;
    };

    Impl(Navigator& owner, float barHeight, NavigationStyle style)
        : owner(owner) {
        auto created = std::make_unique<NavigationView>(barHeight, style);
        navigation = created.get();
        root = std::move(created);
        navigation->requestPop = [this] { this->owner.pop(true); };
        navigation->lifecycle = [this](View* page, RouteEvent event, bool forward) {
            Route* route = find(page);
            return route && route->element
                ? route->element->dispatchRouteEvent(event, forward)
                : true;
        };
        navigation->popped = [this](View* page) {
            const auto it = std::find_if(routes.begin(), routes.end(),
                                         [page](const Route& route) {
                                             return route.view == page;
                                         });
            if (it == routes.end()) {
                return;
            }
            it->element->unmount();
            routes.erase(it);
        };
    }

    Route* find(View* page) {
        const auto it = std::find_if(routes.begin(), routes.end(),
                                     [page](const Route& route) {
                                         return route.view == page;
                                     });
        return it == routes.end() ? nullptr : &*it;
    }

    Navigator& owner;
    std::unique_ptr<View> root;
    NavigationView* navigation = nullptr;
    std::vector<Route> routes;
};

Navigator::Navigator(float navigationBarHeight, NavigationStyle style)
    : impl_(std::make_unique<Impl>(*this, navigationBarHeight, style)) {}

Navigator::~Navigator() {
    clear();
}

Navigator& Navigator::of(BuildContext& context) {
    return context.navigator();
}

bool Navigator::push(std::unique_ptr<Widget> page, bool animated) {
    if (!page) {
        return false;
    }
    // 先把页面 mount 到 contentView 拿到根 View，再进栈；
    // 进栈失败（转场中 / 生命周期否决）要回滚：unmount 并摘出路由表。
    Impl::Route route;
    route.element = mountWidget(
        std::move(page), impl_->navigation->contentView(), this);
    route.view = route.element ? route.element->renderObject() : nullptr;
    if (!route.view) {
        if (route.element) {
            route.element->unmount();
        }
        return false;
    }
    impl_->routes.push_back(std::move(route));
    if (!impl_->navigation->push(*impl_->routes.back().view, animated)) {
        impl_->routes.back().element->unmount();
        impl_->routes.pop_back();
        return false;
    }
    return true;
}

bool Navigator::pop(bool animated) {
    return impl_->navigation->pop(animated);
}

size_t Navigator::depth() const {
    return impl_->navigation->depth();
}

View* Navigator::topView() const {
    return impl_->navigation->topPage();
}

View& Navigator::view() const {
    return *impl_->root;
}

void Navigator::setStyle(const NavigationStyle& style) {
    impl_->navigation->setStyle(style);
}

void Navigator::setBounds(float width, float height) {
    impl_->root->setBounds(0.0f, 0.0f, width, height);
}

void Navigator::clear() {
    if (!impl_) {
        return;
    }
    impl_->navigation->clearPages();
    // 逆序拆除路由：栈顶先 unmount（后入先出），最后清表。
    for (auto it = impl_->routes.rbegin(); it != impl_->routes.rend(); ++it) {
        if (it->element) {
            it->element->unmount();
        }
    }
    impl_->routes.clear();
}

} // namespace evk::ui
