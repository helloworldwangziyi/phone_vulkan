#include "evk/ui/navigation/zy_navigation_stack.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

#include "evk/zy_log.h"
#include "evk/zy_frame_scheduler.h"
#include "evk/ui/zy_animation_scheduler.h"
#include "evk/ui/controls/zy_button_control.h"
#include "evk/ui/zy_pointer_input.h"
#include "evk/ui/zy_render_view.h"
#include "evk/ui/zy_widget_tree.h"

namespace evk::ui {
namespace {

constexpr float kBackGestureEdge = 48.0f;
constexpr float kBackVelocity = 400.0f;
constexpr float kParallax = 0.28f;
constexpr int64_t kTransitionDurationNanos = 280'000'000;

enum class TransitionKind {
    None,
    Push,
    Pop,
};

class NavigationView final : public View {
public:
    explicit NavigationView(float barHeight, NavigationStyle initialStyle)
        : navigationBarHeight(std::max(0.0f, barHeight)),
          style(initialStyle) {
        content = addChild(std::make_unique<View>());
        bar = addChild(std::make_unique<View>());
        barLine = bar->addChild(std::make_unique<View>());
        backButton = bar->addChild(createButtonView(
            backButtonStyle(), [this] {
                if (requestPop) {
                    requestPop();
                }
            }));
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

    bool acceptsPanInput() const override {
        return pages.size() > 1 && !transitioning;
    }

    void handlePan(const PanEvent& event) override {
        const float width = content ? content->rect.w : rect.w;
        if (width <= 0.0f || pages.size() <= 1) {
            return;
        }
        switch (event.state) {
            case PanState::Begin: {
                const float downX = event.x - event.translationX;
                if (downX > kBackGestureEdge || !beginPopLifecycle()) {
                    return;
                }
                gestureActive = true;
                transitioning = true;
                transitionKind = TransitionKind::Pop;
                gestureProgress = std::clamp(event.translationX / width, 0.0f, 1.0f);
                applyPopProgress(gestureProgress);
                break;
            }
            case PanState::Update:
                if (!gestureActive) {
                    return;
                }
                gestureProgress = std::clamp(event.translationX / width, 0.0f, 1.0f);
                applyPopProgress(gestureProgress);
                break;
            case PanState::End:
                if (!gestureActive) {
                    return;
                }
                gestureActive = false;
                animatePop(
                    gestureProgress,
                    event.velocityX > kBackVelocity || gestureProgress > 0.5f);
                break;
            case PanState::Cancel:
                if (!gestureActive) {
                    return;
                }
                gestureActive = false;
                animatePop(gestureProgress, false);
                break;
        }
    }

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

    void clearPages() {
        pages.clear();
        transitioning = false;
        gestureActive = false;
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
        gestureProgress = 0.0f;
    }

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

    View* content = nullptr;
    View* bar = nullptr;
    View* barLine = nullptr;
    View* backButton = nullptr;
    float navigationBarHeight = 0.0f;
    NavigationStyle style;
    std::vector<View*> pages;
    bool transitioning = false;
    bool gestureActive = false;
    bool popLifecycleActive = false;
    float gestureProgress = 0.0f;
    TransitionKind transitionKind = TransitionKind::None;
};

} // namespace

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
    for (auto it = impl_->routes.rbegin(); it != impl_->routes.rend(); ++it) {
        if (it->element) {
            it->element->unmount();
        }
    }
    impl_->routes.clear();
}

} // namespace evk::ui
