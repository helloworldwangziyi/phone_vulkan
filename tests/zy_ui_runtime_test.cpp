#include <cassert>
#include <cstdint>
#include <memory>
#include <vector>

#include "evk/zy_app_lifecycle.h"
#include "evk/zy_frame_scheduler.h"
#include "evk/ui/zy_ui_application.h"
#include "evk/ui/zy_animation_scheduler.h"
#include "evk/ui/zy_paint_canvas.h"
#include "evk/ui/controls/zy_button_control.h"
#include "evk/ui/controls/zy_scroll_control.h"
#include "evk/ui/zy_event_bus.h"
#include "evk/ui/zy_pointer_input.h"
#include "evk/ui/layout/zy_flex_layout.h"
#include "evk/ui/navigation/zy_navigation_stack.h"
#include "evk/ui/zy_render_view.h"
#include "evk/ui/zy_widget_tree.h"

namespace {

evk::ui::Canvas g_canvas;
int g_frames = 0;

int64_t ms(int64_t value) {
    return value * 1'000'000;
}

void renderFrame(int64_t) {
    ++g_frames;
    evk::ui::buildFrame(g_canvas);
}

void resetRuntime() {
    evk::ui::shutdownApp();
    evk::ui::cancelAllPointerEvents();
    evk::ui::stopAllAnimations();
    evk::ui::EventBus::instance().clear();
    evk::cancelPendingFrame();
    g_frames = 0;
    evk::setFrameFunc(renderFrame);
}

void testDirtyFramesAreCoalesced() {
    resetRuntime();
    evk::requestRender();
    evk::requestRender();
    assert(evk::beginFrame(ms(1)));
    assert(g_frames == 1);
    assert(!evk::beginFrame(ms(2)));
}

void testAppLifecycleCallback() {
    int calls = 0;
    evk::setEventFunc([&calls](evk::EventId id, const void* data) {
        assert(id == evk::EventId::EngineReady);
        assert(data == nullptr);
        ++calls;
    });
    evk::dispatchEvent(evk::EventId::EngineReady, nullptr);
    assert(calls == 1);
    evk::setEventFunc({});
    evk::dispatchEvent(evk::EventId::EngineReady, nullptr);
    assert(calls == 1);
}

void testViewTreePaintingAndInput() {
    resetRuntime();
    auto root = std::make_unique<evk::ui::View>();
    root->setBounds(0.0f, 0.0f, 200.0f, 200.0f);
    root->setBackground(0x112233FF);

    int clicks = 0;
    auto child = std::make_unique<evk::ui::View>();
    child->setBounds(20.0f, 30.0f, 100.0f, 80.0f);
    child->onClick = [&clicks](const evk::ui::ClickEvent& event) {
        assert(event.x == 20.0f);
        assert(event.y == 20.0f);
        ++clicks;
    };
    child->painter = [](evk::ui::PaintContext& paint) {
        paint.drawRect({0.0f, 0.0f, 10.0f, 10.0f}, 0xFFFFFFFF);
    };
    root->addChild(std::move(child));
    evk::ui::setRootView(root.get());

    evk::ui::dispatchPointerEvent(
        {evk::ui::PointerAction::Down, 0, 40.0f, 50.0f, ms(10)});
    evk::ui::dispatchPointerEvent(
        {evk::ui::PointerAction::Up, 0, 40.0f, 50.0f, ms(20)});
    assert(clicks == 1);

    evk::requestRender();
    evk::beginFrame(ms(30));
    assert(!g_canvas.vertices().empty());

    evk::ui::setRootView(nullptr);
    root.reset();
}

void testButtonStateMachine() {
    resetRuntime();
    int presses = 0;
    auto button = evk::ui::createButtonView(
        {0x00FF00FF, 0xFF0000FF, 0x777777FF},
        [&presses] { ++presses; });
    button->setBounds(0.0f, 0.0f, 100.0f, 100.0f);
    evk::ui::setRootView(button.get());

    evk::ui::dispatchPointerEvent(
        {evk::ui::PointerAction::Down, 0, 20.0f, 20.0f, ms(1)});
    evk::ui::dispatchPointerEvent(
        {evk::ui::PointerAction::Up, 0, 20.0f, 20.0f, ms(2)});
    assert(presses == 1);

    evk::ui::dispatchPointerEvent(
        {evk::ui::PointerAction::Down, 0, 20.0f, 20.0f, ms(3)});
    evk::ui::dispatchPointerEvent(
        {evk::ui::PointerAction::Move, 0, 180.0f, 180.0f, ms(4)});
    evk::ui::dispatchPointerEvent(
        {evk::ui::PointerAction::Up, 0, 180.0f, 180.0f, ms(5)});
    assert(presses == 1);

    evk::ui::setRootView(nullptr);
}

void testFlexLayout() {
    resetRuntime();
    auto flex = evk::ui::createFlexView(evk::ui::Axis::Vertical);
    flex->setBounds(0.0f, 0.0f, 400.0f, 800.0f);

    auto a = std::make_unique<evk::ui::View>();
    auto b = std::make_unique<evk::ui::View>();
    auto c = std::make_unique<evk::ui::View>();
    evk::ui::View* aView = flex->addChild(std::move(a));
    evk::ui::View* bView = flex->addChild(std::move(b));
    evk::ui::View* cView = flex->addChild(std::move(c));

    evk::ui::setFlexChild(
        *flex, *aView,
        {200.0f, 0.0f, -1.0f, evk::ui::CrossAxisAlignment::Stretch,
         10.0f, 20.0f, 0.0f});
    evk::ui::setFlexChild(
        *flex, *bView,
        {-1.0f, 1.0f, -1.0f, evk::ui::CrossAxisAlignment::Stretch});
    evk::ui::setFlexChild(
        *flex, *cView,
        {-1.0f, 2.0f, 100.0f, evk::ui::CrossAxisAlignment::Center});

    assert(aView->rect.y == 10.0f && aView->rect.h == 200.0f);
    assert(bView->rect.y == 230.0f && bView->rect.h == 190.0f);
    assert(cView->rect.y == 420.0f && cView->rect.h == 380.0f);
    assert(cView->rect.x == 150.0f && cView->rect.w == 100.0f);

    flex->setBounds(0.0f, 0.0f, 200.0f, 400.0f);
    assert(bView->rect.h > 56.0f && bView->rect.h < 57.0f);
    assert(cView->rect.x == 50.0f);
}

void testScrollViewDragAndClamp() {
    resetRuntime();
    auto scroll = evk::ui::createScrollView(0.0f, 1000.0f);
    scroll->setBounds(0.0f, 0.0f, 200.0f, 200.0f);
    evk::ui::setRootView(scroll.get());

    evk::ui::dispatchPointerEvent(
        {evk::ui::PointerAction::Down, 0, 100.0f, 160.0f, ms(1)});
    evk::ui::dispatchPointerEvent(
        {evk::ui::PointerAction::Move, 0, 100.0f, 60.0f, ms(40)});
    evk::ui::dispatchPointerEvent(
        {evk::ui::PointerAction::Up, 0, 100.0f, 60.0f, ms(200)});

    float x = 0.0f;
    float y = 0.0f;
    evk::ui::getScrollOffset(*scroll, &x, &y);
    assert(x == 0.0f);
    assert(y > 90.0f);

    evk::ui::setScrollOffset(*scroll, 0.0f, 5000.0f);
    evk::ui::getScrollOffset(*scroll, &x, &y);
    assert(y == 800.0f);
    evk::ui::setRootView(nullptr);
}

void testEventBusAndUiQueue() {
    resetRuntime();
    auto root = std::make_unique<evk::ui::View>();
    root->setBounds(0.0f, 0.0f, 100.0f, 100.0f);
    evk::ui::setRootView(root.get());

    std::vector<int> calls;
    auto low = evk::ui::EventBus::instance().subscribe(
        7, evk::ui::EventPriority::Low, nullptr,
        [&calls](const void*) {
            calls.push_back(3);
            return false;
        });
    auto high = evk::ui::EventBus::instance().subscribe(
        7, evk::ui::EventPriority::High, nullptr,
        [&calls](const void*) {
            calls.push_back(1);
            return false;
        });
    auto scoped = evk::ui::EventBus::instance().subscribe(
        7, evk::ui::EventPriority::Normal, root.get(),
        [&calls](const void*) {
            calls.push_back(2);
            return false;
        });

    evk::ui::EventBus::instance().emit(7);
    assert((calls == std::vector<int>{1, 2, 3}));
    root->setVisible(false);
    calls.clear();
    evk::ui::EventBus::instance().emit(7);
    assert((calls == std::vector<int>{1, 3}));
    scoped.cancel();

    int posted = 0;
    evk::ui::postUi([&posted] { ++posted; });
    evk::beginFrame(ms(1));
    assert(posted == 1);
    evk::ui::setRootView(nullptr);
}

class CounterState;

class CounterPage final : public evk::ui::StatefulWidget {
public:
    explicit CounterPage(bool allowEnter = true) : allowEnter_(allowEnter) {}

    std::unique_ptr<evk::ui::State> createState() const override;
    bool allowEnter() const { return allowEnter_; }

private:
    bool allowEnter_;
};

class CounterState final : public evk::ui::State {
public:
    static int alive;
    static CounterState* latest;

    CounterState() {
        ++alive;
        latest = this;
    }

    ~CounterState() override {
        --alive;
    }

    std::unique_ptr<evk::ui::Widget> build(evk::ui::BuildContext&) override {
        ++buildCount;
        return evk::ui::column(
            evk::ui::expanded(evk::ui::container(
                count == 0 ? 0xFF0000FF : 0x00FF00FF)));
    }

    bool onWillEnter(bool) override {
        return widgetAs<CounterPage>().allowEnter();
    }

    void onDidEnter(bool) override { ++enters; }
    void onDidLeave(bool) override { ++leaves; }

    int count = 0;
    int buildCount = 0;
    int enters = 0;
    int leaves = 0;
};

int CounterState::alive = 0;
CounterState* CounterState::latest = nullptr;

std::unique_ptr<evk::ui::State> CounterPage::createState() const {
    return std::make_unique<CounterState>();
}

void testStateAndNavigatorLifecycle() {
    resetRuntime();
    evk::ui::setViewportSize(400.0f, 800.0f);
    evk::ui::runApp(
        evk::ui::makeWidget<CounterPage>(),
        {60.0f, {}});

    evk::ui::Navigator* navigator = evk::ui::appNavigator();
    assert(navigator && navigator->depth() == 1);
    assert(CounterState::alive == 1);
    CounterState* first = CounterState::latest;
    assert(first->buildCount == 1 && first->enters == 1);
    evk::ui::View* firstView = navigator->topView();

    first->setState([first] { first->count = 1; });
    assert(first->buildCount == 2);
    assert(navigator->topView() == firstView);

    assert(navigator->push(evk::ui::makeWidget<CounterPage>(), false));
    assert(navigator->depth() == 2);
    assert(CounterState::alive == 2);
    assert(first->leaves == 1);
    assert(navigator->pop(false));
    assert(navigator->depth() == 1);
    assert(CounterState::alive == 1);
    assert(first->enters == 2);

    assert(!navigator->push(evk::ui::makeWidget<CounterPage>(false), false));
    assert(navigator->depth() == 1);
    assert(CounterState::alive == 1);

    evk::ui::shutdownApp();
    assert(CounterState::alive == 0);
}

void testAnimatedNavigationAndEdgeSwipe() {
    resetRuntime();
    evk::ui::setViewportSize(400.0f, 800.0f);
    evk::ui::runApp(evk::ui::makeWidget<CounterPage>(), {60.0f, {}});
    evk::ui::Navigator& navigator = *evk::ui::appNavigator();
    assert(navigator.push(evk::ui::makeWidget<CounterPage>(), false));

    evk::ui::dispatchPointerEvent(
        {evk::ui::PointerAction::Down, 0, 10.0f, 300.0f, ms(1000)});
    evk::ui::dispatchPointerEvent(
        {evk::ui::PointerAction::Move, 0, 260.0f, 300.0f, ms(1050)});
    evk::ui::dispatchPointerEvent(
        {evk::ui::PointerAction::Up, 0, 260.0f, 300.0f, ms(1060)});

    int64_t time = ms(1100);
    for (int i = 0; i < 30; ++i) {
        time += ms(16);
        evk::beginFrame(time);
    }
    assert(navigator.depth() == 1);
    evk::ui::shutdownApp();
}

} // namespace

int main() {
    testAppLifecycleCallback();
    testDirtyFramesAreCoalesced();
    testViewTreePaintingAndInput();
    testButtonStateMachine();
    testFlexLayout();
    testScrollViewDragAndClamp();
    testEventBusAndUiQueue();
    testStateAndNavigatorLifecycle();
    testAnimatedNavigationAndEdgeSwipe();
    return 0;
}
