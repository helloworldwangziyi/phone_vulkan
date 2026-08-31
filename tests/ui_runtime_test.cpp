#include <cassert>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "evk/app_lifecycle.h"
#include "evk/frame_scheduler.h"
#include "evk/ui/ui_application.h"
#include "evk/ui/animation_scheduler.h"
#include "evk/ui/font_engine.h"
#include "evk/ui/text_layout.h"
#include "evk/ui/texture_store.h"
#include "evk/ui/paint_canvas.h"
#include "evk/ui/view/button_control.h"
#include "evk/ui/view/scroll_control.h"
#include "evk/ui/event_bus.h"
#include "evk/ui/pointer_input.h"
#include "evk/ui/layout/flex_layout.h"
#include "evk/ui/navigation/navigation_stack.h"
#include "evk/ui/render_view.h"
#include "evk/ui/widget_tree.h"
#include "evk/ui/widgets.h"

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
        return true;
    });
    assert(evk::dispatchEvent(evk::EventId::EngineReady, nullptr));
    assert(calls == 1);
    evk::setEventFunc({});
    assert(!evk::dispatchEvent(evk::EventId::EngineReady, nullptr));
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

    // 橡皮筋回归：手指按住拖出顶部越界后，同尺寸 rebuild（updateScrollView、
    // 同值 setBounds 触发的 handleBoundsChanged）不得把越界状态钳回界内。
    evk::ui::setScrollOffset(*scroll, 0.0f, 0.0f);
    evk::ui::dispatchPointerEvent(
        {evk::ui::PointerAction::Down, 0, 100.0f, 100.0f, ms(300)});
    evk::ui::dispatchPointerEvent(
        {evk::ui::PointerAction::Move, 0, 100.0f, 190.0f, ms(320)});
    evk::ui::getScrollOffset(*scroll, &x, &y);
    assert(y < 0.0f);  ///< 顶部越界，显示偏移走 0.45 阻尼

    evk::ui::updateScrollView(*scroll, 0.0f, 1000.0f);  ///< 同尺寸重建
    scroll->setBounds(0.0f, 0.0f, 200.0f, 200.0f);      ///< 同值重排
    evk::ui::getScrollOffset(*scroll, &x, &y);
    assert(y < 0.0f);  ///< 越界状态保留，拖拽不中断

    // 几何尺寸真实变化时仍要收编：内容缩短后 maxOffset 变化，越界被钳回。
    evk::ui::updateScrollView(*scroll, 0.0f, 500.0f);
    evk::ui::getScrollOffset(*scroll, &x, &y);
    assert(y == 0.0f);
    evk::ui::setRootView(nullptr);
}

/// 列表控件：定高行落位、双轴滚动钳制、删行后自动重排。
void testListViewLayoutAndScroll() {
    resetRuntime();
    evk::ui::setViewportSize(200.0f, 200.0f);

    std::vector<std::unique_ptr<evk::ui::Widget>> rows;
    for (int i = 0; i < 10; ++i) {
        rows.push_back(evk::ui::container(0x111111FF));
    }
    // 行高 50 × 10 行，内容宽 400（视口 200：横向纵向都可滚）。
    evk::ui::runApp(evk::ui::listView(50.0f, std::move(rows), 400.0f), {});

    evk::ui::View* viewport = evk::ui::appNavigator()->topView();
    evk::ui::View* content = evk::ui::scrollContent(*viewport);
    assert(content && content->children.size() == 10);
    // 定高行落位：第 i 行 = (0, i×50, 内容宽 400, 50)。
    for (size_t i = 0; i < content->children.size(); ++i) {
        const evk::ui::Rect r = content->children[i]->rect;
        assert(r.x == 0.0f && r.y == 50.0f * static_cast<float>(i) &&
               r.w == 400.0f && r.h == 50.0f);
    }

    // 双轴钳制：maxX = 400-200 = 200，maxY = 500-200 = 300。
    evk::ui::setScrollOffset(*viewport, 1000.0f, 1000.0f);
    float x = 0.0f;
    float y = 0.0f;
    evk::ui::getScrollOffset(*viewport, &x, &y);
    assert(x == 200.0f && y == 300.0f);
    // 滚动 = content 反向平移。
    assert(content->rect.x == -200.0f && content->rect.y == -300.0f);

    // 删行（View 层）：下方行自动上移补位。
    content->removeChild(content->children[2].get());
    assert(content->children.size() == 9);
    for (size_t i = 0; i < content->children.size(); ++i) {
        assert(content->children[i]->rect.y == 50.0f * static_cast<float>(i));
    }

    evk::ui::shutdownApp();
}

/// 方向锁：一段拖动手势只滚主方向轴，另一轴分量整段丢弃（横竖互斥）。
void testScrollAxisLock() {
    resetRuntime();
    auto scroll = evk::ui::createScrollView(400.0f, 1000.0f);  // 双轴可滚
    scroll->setBounds(0.0f, 0.0f, 200.0f, 200.0f);
    evk::ui::setRootView(scroll.get());

    float x = 0.0f;
    float y = 0.0f;

    // 主方向横向（|dx| > |dy|）：只横向滚。
    evk::ui::dispatchPointerEvent(
        {evk::ui::PointerAction::Down, 0, 100.0f, 100.0f, ms(1)});
    evk::ui::dispatchPointerEvent(
        {evk::ui::PointerAction::Move, 0, 60.0f, 95.0f, ms(20)});
    evk::ui::getScrollOffset(*scroll, &x, &y);
    assert(x > 0.0f && y == 0.0f);
    // 锁定后纵向分量再大也不响应。
    evk::ui::dispatchPointerEvent(
        {evk::ui::PointerAction::Move, 0, 55.0f, 40.0f, ms(40)});
    evk::ui::getScrollOffset(*scroll, &x, &y);
    assert(y == 0.0f);
    evk::ui::dispatchPointerEvent(
        {evk::ui::PointerAction::Up, 0, 55.0f, 40.0f, ms(60)});
    const float lockedX = x;

    // 新一段手势主方向纵向：只纵向滚，横向偏移保持。
    evk::ui::dispatchPointerEvent(
        {evk::ui::PointerAction::Down, 0, 100.0f, 100.0f, ms(80)});
    evk::ui::dispatchPointerEvent(
        {evk::ui::PointerAction::Move, 0, 95.0f, 60.0f, ms(100)});
    evk::ui::getScrollOffset(*scroll, &x, &y);
    assert(x == lockedX && y > 0.0f);
    evk::ui::dispatchPointerEvent(
        {evk::ui::PointerAction::Up, 0, 95.0f, 60.0f, ms(120)});

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

void testAnimatedNavigationAndBackEvent() {
    resetRuntime();
    evk::ui::setViewportSize(400.0f, 800.0f);
    evk::ui::runApp(evk::ui::makeWidget<CounterPage>(), {60.0f, {}});
    evk::ui::Navigator& navigator = *evk::ui::appNavigator();
    assert(navigator.push(evk::ui::makeWidget<CounterPage>(), false));

    // 平台壳上报系统返回 → App 入口（此处用测试替身模拟 app_entry 的
    // BackPressed 分支）pop 导航栈；栈底不消费，交还平台壳。
    evk::setEventFunc([](evk::EventId id, const void*) {
        if (id != evk::EventId::BackPressed) {
            return true;
        }
        evk::ui::Navigator* nav = evk::ui::appNavigator();
        if (nav && nav->depth() > 1) {
            nav->pop(true);
            return true;
        }
        return false;
    });
    assert(evk::dispatchEvent(evk::EventId::BackPressed, nullptr));

    int64_t time = ms(1100);
    for (int i = 0; i < 30; ++i) {
        time += ms(16);
        evk::beginFrame(time);
    }
    assert(navigator.depth() == 1);
    // 栈底再按返回：不消费，由平台壳收尾。
    assert(!evk::dispatchEvent(evk::EventId::BackPressed, nullptr));
    evk::setEventFunc({});
    evk::ui::shutdownApp();
}

void testSafeAreaInsets() {
    resetRuntime();
    // 安全区可在建 UI 之前到达（壳层时序），值要先存住。
    evk::ui::setViewportSize(400.0f, 800.0f);
    evk::ui::setSafeAreaInsets(24.0f, 34.0f, 0.0f, 0.0f);
    evk::ui::runApp(evk::ui::makeWidget<CounterPage>(), {60.0f, {}});

    // 根视图 = 视口扣除安全区：top 24 / bottom 34。
    evk::ui::View& root = evk::ui::appNavigator()->view();
    assert(root.rect.x == 0.0f && root.rect.y == 24.0f);
    assert(root.rect.w == 400.0f && root.rect.h == 800.0f - 24.0f - 34.0f);

    // 视口变化（旋转）后仍保持内缩；横屏左右 inset 生效。
    evk::ui::setViewportSize(800.0f, 400.0f);
    evk::ui::setSafeAreaInsets(0.0f, 21.0f, 44.0f, 44.0f);
    assert(root.rect.x == 44.0f && root.rect.y == 0.0f);
    assert(root.rect.w == 800.0f - 88.0f && root.rect.h == 400.0f - 21.0f);

    evk::ui::setSafeAreaInsets(0.0f, 0.0f, 0.0f, 0.0f);
    evk::ui::shutdownApp();
}

/// 读整个文件到内存（字体资产用）。
std::vector<unsigned char> readFile(const char* path) {
    std::FILE* f = std::fopen(path, "rb");
    if (!f) {
        return {};
    }
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<unsigned char> data(static_cast<size_t>(size));
    const size_t read = size > 0 ? std::fread(data.data(), 1, data.size(), f) : 0;
    std::fclose(f);
    data.resize(read);
    return data;
}

/// 字体引擎：注册、测量、回退、光栅化进 atlas、Canvas drawText 分批。
void testFontEngine(const char* latinPath, const char* cjkPath) {
    auto& fonts = evk::ui::FontEngine::instance();
    fonts.reset();
    assert(fonts.fontCount() == 0);

    const auto latinData = readFile(latinPath);
    const auto cjkData = readFile(cjkPath);
    assert(!latinData.empty() && !cjkData.empty());

    const evk::ui::FontId latin = fonts.addFont(latinData.data(), latinData.size());
    const evk::ui::FontId cjk = fonts.addFont(cjkData.data(), cjkData.size());
    assert(latin == 0 && cjk == 1);
    assert(fonts.addFont(nullptr, 0) == evk::ui::kInvalidFont);

    // 测量：非空文本有正宽度，宽度随字号增长。
    float w1 = 0.0f, h1 = 0.0f;
    fonts.measureText("Hello, world!", 32.0f, latin, &w1, &h1);
    assert(w1 > 0.0f && h1 > 0.0f);
    float w2 = 0.0f, h2 = 0.0f;
    fonts.measureText("Hello, world!", 64.0f, latin, &w2, &h2);
    assert(w2 > w1 && h2 > h1);

    // 回退：汉字在 Roboto 里没有，注册顺序回退到 CJK 字体后宽度仍可测。
    float wCjk = 0.0f;
    fonts.measureText("行情速览", 32.0f, latin, &wCjk, nullptr);
    assert(wCjk > 0.0f);
    // 同样文本用 CJK 首选字体测量：四个表意字，宽度显著大于空串。
    fonts.measureText("行情速览", 32.0f, cjk, &wCjk, nullptr);
    assert(wCjk > 32.0f * 3.0f); ///< 表意字宽接近字号，4 个字远超 3 倍字号

    // 光栅化：drawText 触发字形进 atlas，Canvas 出现纹理批次。
    evk::ui::Canvas canvas;
    canvas.clear();
    canvas.drawText("Hi 你好", evk::ui::kFontAny, 0.0f, 0.0f, 32.0f,
                    {0.0f, 0.0f, 400.0f, 100.0f}, evk::ui::Color::rgba(0xFFFFFFFF));
    assert(canvas.vertices().size() >= 6);          ///< 至少一个字形四边形
    bool hasTextBatch = false;
    for (const auto& batch : canvas.batches()) {
        if (batch.textureId != 0) {
            hasTextBatch = true;
        }
    }
    assert(hasTextBatch);                             ///< 文字进 atlas 纹理批次
    assert(fonts.pageCount() >= 1);
    const evk::ui::TextureId pageTex = fonts.pageTexture(0);
    assert(pageTex != evk::ui::kInvalidTexture);
    assert(evk::ui::TextureStore::instance().pixels(pageTex) != nullptr);

    // 脏页标记（TextureStore 语义）：首帧光栅化置脏，上传后被取走，
    // 重复绘制同样文本不再置脏（字形命中缓存）。
    assert(evk::ui::TextureStore::instance().consumeDirty(pageTex));
    canvas.clear();
    canvas.drawText("Hi 你好", evk::ui::kFontAny, 0.0f, 0.0f, 32.0f,
                    {0.0f, 0.0f, 400.0f, 100.0f}, evk::ui::Color::rgba(0xFFFFFFFF));
    assert(!evk::ui::TextureStore::instance().consumeDirty(pageTex));

    // 嵌套 Column 必须把文字的固有行高上报给外层，否则内层会被压成 0 高，
    // painter 不会产生任何字形批次。
    evk::ui::runApp(
        evk::ui::column(evk::ui::column(
            evk::ui::text("中英混排 Mixed 123", 32.0f, 0xFFFFFFFF,
                          evk::ui::kFontAny))),
        {});
    evk::ui::setViewportSize(400.0f, 800.0f);
    assert(evk::beginFrame(ms(10)));
    bool widgetHasTextBatch = false;
    for (const auto& batch : g_canvas.batches()) {
        widgetHasTextBatch = widgetHasTextBatch || batch.textureId != 0;
    }
    assert(widgetHasTextBatch);
    evk::ui::shutdownApp();

    fonts.reset();
}

/// 多行排版：CJK 任意断、英文按词断、硬断、\n、缓存、clip 裁剪与
/// 换行 Text 控件的高度回灌。
void testTextLayout(const char* latinPath, const char* cjkPath) {
    auto& fonts = evk::ui::FontEngine::instance();
    fonts.reset();
    const auto latinData = readFile(latinPath);
    const auto cjkData = readFile(cjkPath);
    assert(!latinData.empty() && !cjkData.empty());
    const evk::ui::FontId latin = fonts.addFont(latinData.data(), latinData.size());
    fonts.addFont(cjkData.data(), cjkData.size());

    // 行高与单行测量一致；CJK 单字宽（Roboto 缺字，回退到 CJK 字体）。
    float lineHeight = 0.0f;
    fonts.measureText("", 32.0f, latin, nullptr, &lineHeight);
    assert(lineHeight > 0.0f);
    float cjkW = 0.0f;
    fonts.measureText("行", 32.0f, latin, &cjkW, nullptr);
    assert(cjkW > 0.0f);

    // CJK 任意断：4 字按 2.5 字宽排 → 2 行，每行 2 字。
    evk::ui::TextLayout layout;
    layout.layout("行情速览", 32.0f, latin, cjkW * 2.5f);
    assert(layout.lineCount() == 2);
    assert(layout.lineText(0) == "行情");
    assert(layout.lineText(1) == "速览");
    assert(layout.lineHeight() == lineHeight);
    assert(layout.totalHeight() == lineHeight * 2.0f);

    // 相同输入复用缓存（revision 不增）；宽度变了才重排。
    const int rev = layout.revision();
    layout.layout("行情速览", 32.0f, latin, cjkW * 2.5f);
    assert(layout.revision() == rev);
    layout.layout("行情速览", 32.0f, latin, cjkW * 3.5f);
    assert(layout.revision() == rev + 1);
    assert(layout.lineCount() == 2);
    assert(layout.lineText(0) == "行情速");
    assert(layout.lineText(1) == "览");

    float spaceW = 0.0f;
    float aW = 0.0f;
    fonts.measureText(" ", 32.0f, latin, &spaceW, nullptr);
    fonts.measureText("a", 32.0f, latin, &aW, nullptr);
    assert(spaceW > 0.0f && aW > 0.0f);

    // 避头尾（libunibreak / UAX #14，lang=zh）：闭标点（，）不落行首，
    // 开标点（“）不留行尾；英文与 CJK 交界允许断行。
    float commaW = 0.0f;
    float openQuoteW = 0.0f;
    fonts.measureText("，", 32.0f, latin, &commaW, nullptr);
    fonts.measureText("“", 32.0f, latin, &openQuoteW, nullptr);
    assert(commaW > 0.0f && openQuoteW > 0.0f);
    // "行情"恰好放下、连逗号超宽：情 与 ，之间不可断，只能从 行 后断；
    // 逗号跟着"情"留在行尾，不会成为下一行行首。
    layout.layout("行情，速览", 32.0f, latin, cjkW * 2.0f + commaW * 0.5f);
    assert(layout.lineCount() == 3);
    assert(layout.lineText(0) == "行");
    assert(layout.lineText(1) == "情，");
    assert(layout.lineText(2) == "速览");
    // 开引号“之后不可断：宁可"说"独占一行，也不把“留在行尾。
    layout.layout("说“你好”", 32.0f, latin, cjkW + openQuoteW + cjkW * 0.5f);
    assert(layout.lineCount() == 3);
    assert(layout.lineText(0) == "说");
    assert(layout.lineText(1) == "“你");
    assert(layout.lineText(2) == "好”");
    // 拉丁字母与 CJK 交界处允许断行（AL × ID）。
    layout.layout("abc行情", 32.0f, latin, aW * 3.0f + cjkW * 1.5f);
    assert(layout.lineCount() == 2);
    assert(layout.lineText(0) == "abc行");
    assert(layout.lineText(1) == "情");

    // 英文按词断：行宽刚好放下两个词，第三个词回退到最近空格断行，
    // 断点处的空格丢弃（行尾不留、下行不带）。
    const float twoWords = 2.0f * aW + spaceW + 2.0f * aW; // "aa bb"
    layout.layout("aa bb cc", 32.0f, latin, twoWords + aW * 0.5f);
    assert(layout.lineCount() == 2);
    assert(layout.lineText(0) == "aa bb");
    assert(layout.lineText(1) == "cc");

    // 超长单词硬断：10 个 a 按 3.x 字宽 → "aaa" × 3 + "a"。
    float aaaW = 0.0f;
    fonts.measureText("aaa", 32.0f, latin, &aaaW, nullptr);
    layout.layout("aaaaaaaaaa", 32.0f, latin, aaaW + aW * 0.5f);
    assert(layout.lineCount() == 4);
    assert(layout.lineText(0) == "aaa");
    assert(layout.lineText(3) == "a");

    // '\n' 强制断行：尾部空行保留，连续 '\n' 产生空行；行首空白跳过。
    layout.layout("ab\ncd\n", 32.0f, latin, 1000.0f);
    assert(layout.lineCount() == 3);
    assert(layout.lineText(0) == "ab");
    assert(layout.lineText(1) == "cd");
    assert(layout.lineText(2).empty());
    layout.layout("a\n\nb", 32.0f, latin, 1000.0f);
    assert(layout.lineCount() == 3);
    assert(layout.lineText(1).empty());
    layout.layout("  ab", 32.0f, latin, 1000.0f);
    assert(layout.lineCount() == 1 && layout.lineText(0) == "ab");
    layout.layout("", 32.0f, latin, 1000.0f);
    assert(layout.lineCount() == 0 && layout.totalHeight() == 0.0f);

    // clip 裁剪：只画与裁剪区相交的行，视口外的行不生成顶点。
    layout.layout("ab\ncd\nef", 32.0f, latin, 1000.0f);
    assert(layout.lineCount() == 3);
    evk::ui::Canvas canvas;
    const evk::ui::Color white = evk::ui::Color::rgba(0xFFFFFFFF);
    canvas.clear();
    layout.paint(canvas, 0.0f, 0.0f, white, {0.0f, 0.0f, 500.0f, lineHeight});
    assert(canvas.vertices().size() == 2 * 6);        ///< 只画第 0 行（2 字形）
    canvas.clear();
    layout.paint(canvas, 0.0f, 0.0f, white,
                 {0.0f, lineHeight, 500.0f, lineHeight});
    assert(canvas.vertices().size() == 2 * 6);        ///< 只画第 1 行
    canvas.clear();
    layout.paint(canvas, 0.0f, 0.0f, white,
                 {0.0f, lineHeight * 10.0f, 500.0f, lineHeight});
    assert(canvas.vertices().empty());                ///< 全部在视口外

    // 行距倍数：行高 = 自然行高 × 倍数；总高随行数线性缩放。
    layout.layout("行情速览", 32.0f, latin, cjkW * 2.5f, 1.5f);
    assert(layout.lineHeight() == lineHeight * 1.5f);
    assert(layout.totalHeight() == lineHeight * 1.5f * 2.0f);

    // 水平对齐：两行都以 'a' 开头（xoff 相同），首顶点 x 差即对齐偏移差。
    // "abbbb" 与 "ac" 右对齐时右缘齐平：dx1 - dx0 = w0 - w1；居中减半。
    const float bigW = 1000.0f;
    layout.layout("abbbb\nac", 32.0f, latin, bigW, 1.0f, evk::ui::TextAlign::kLeft);
    canvas.clear();
    layout.paint(canvas, 0.0f, 0.0f, white, {0.0f, 0.0f, bigW, 500.0f});
    assert(canvas.vertices().size() == (5 + 2) * 6);
    const float leftDx = canvas.vertices()[6 * 5].x - canvas.vertices()[0].x;
    assert(leftDx == 0.0f);
    const float wDiff = layout.lines()[0].width - layout.lines()[1].width;
    assert(wDiff > 0.0f);
    layout.layout("abbbb\nac", 32.0f, latin, bigW, 1.0f, evk::ui::TextAlign::kRight);
    canvas.clear();
    layout.paint(canvas, 0.0f, 0.0f, white, {0.0f, 0.0f, bigW, 500.0f});
    const float rightDx = canvas.vertices()[6 * 5].x - canvas.vertices()[0].x;
    assert(rightDx > wDiff - 0.01f && rightDx < wDiff + 0.01f);
    layout.layout("abbbb\nac", 32.0f, latin, bigW, 1.0f,
                  evk::ui::TextAlign::kCenter);
    canvas.clear();
    layout.paint(canvas, 0.0f, 0.0f, white, {0.0f, 0.0f, bigW, 500.0f});
    const float centerDx = canvas.vertices()[6 * 5].x - canvas.vertices()[0].x;
    assert(centerDx > wDiff * 0.5f - 0.01f && centerDx < wDiff * 0.5f + 0.01f);

    // maxLines + 省略号：超出行丢弃，末行削尾补"…"且总宽不越界。
    layout.layout("行情速览行情速览", 32.0f, latin, cjkW * 2.5f, 1.0f,
                  evk::ui::TextAlign::kLeft, 1);
    assert(layout.lineCount() == 1);
    assert(layout.lines()[0].ellipsized);
    assert(layout.lines()[0].width <= cjkW * 2.5f + 0.01f);
    assert(layout.lines()[0].width > layout.lines()[0].textWidth); // 含省略号
    assert(layout.lineText(0).size() < std::string("行情速览行情速览").size());
    // 不限宽时只截行不削尾：第二行保留全文，宽度 = 正文 + 省略号。
    layout.layout("ab\ncd\nef", 32.0f, latin, 0.0f, 1.0f,
                  evk::ui::TextAlign::kLeft, 2);
    assert(layout.lineCount() == 2);
    assert(layout.lineText(1) == "cd");
    assert(layout.lines()[1].ellipsized);
    // 行数未超上限时不截断。
    layout.layout("ab\ncd", 32.0f, latin, 0.0f, 1.0f,
                  evk::ui::TextAlign::kLeft, 2);
    assert(layout.lineCount() == 2 && !layout.lines()[1].ellipsized);

    // 换行 Text 控件：挂进 Column 后高度 = 行数 × 行高（视图拿到宽度后
    // 经 setFlexChild 回灌），绘制只出可见行的字形批次。
    resetRuntime();
    evk::ui::setViewportSize(400.0f, 800.0f);
    // 构造一段 2.x 屏宽的中文长文本。
    std::string longText;
    for (int i = 0; i < 20; ++i) {
        longText += "行情速览";
    }
    evk::ui::TextLayout expect;
    expect.layout(longText.c_str(), 32.0f, latin, 400.0f);
    assert(expect.lineCount() > 1);
    evk::ui::runApp(
        evk::ui::column(evk::ui::text(longText, 32.0f, 0xFFFFFFFF,
                                      evk::ui::kFontAny, /*softWrap=*/true)),
        {});
    evk::ui::View* page = evk::ui::appNavigator()->topView();
    assert(page && page->children.size() == 1);
    const evk::ui::View* textView = page->children[0].get();
    assert(textView->rect.w == 400.0f);
    assert(textView->rect.h == expect.totalHeight());
    assert(evk::beginFrame(ms(10)));
    bool hasTextBatch = false;
    for (const auto& batch : g_canvas.batches()) {
        hasTextBatch = hasTextBatch || batch.textureId != 0;
    }
    assert(hasTextBatch);
    evk::ui::shutdownApp();

    fonts.reset();
}
void testCanvas2dPrimitives() {
    auto& store = evk::ui::TextureStore::instance();
    store.reset();
    evk::ui::Canvas canvas;
    const evk::ui::Rect clip{0.0f, 0.0f, 1000.0f, 1000.0f};
    const evk::ui::Color white = evk::ui::Color::rgba(0xFFFFFFFF);
    const evk::ui::Color cyan = evk::ui::Color::rgba(0x22D3EEFF);

    canvas.clear();
    size_t n = 0;

    // 矩形：6 顶点。
    canvas.drawRect({0.0f, 0.0f, 10.0f, 10.0f}, clip, white);
    n = canvas.vertices().size();
    assert(n == 6);

    // 线段：一个四边形 = 6 顶点；零长度线静默跳过。
    canvas.drawLine(0.0f, 0.0f, 100.0f, 0.0f, 4.0f, clip, white);
    assert(canvas.vertices().size() - n == 6);
    n = canvas.vertices().size();
    canvas.drawLine(5.0f, 5.0f, 5.0f, 5.0f, 4.0f, clip, white);
    assert(canvas.vertices().size() == n);

    // 圆：segments 个三角形 = 3*segments 顶点。
    canvas.drawCircle(50.0f, 50.0f, 30.0f, clip, white, 16);
    assert(canvas.vertices().size() - n == 16 * 3);
    n = canvas.vertices().size();

    // 椭圆：同理。
    canvas.drawEllipse(50.0f, 50.0f, 30.0f, 20.0f, clip, white, 8);
    assert(canvas.vertices().size() - n == 8 * 3);
    n = canvas.vertices().size();

    // 圆角矩形：3 个矩形（18）+ 4 角 × segments 个三角形。
    canvas.drawRoundRect({0.0f, 0.0f, 100.0f, 60.0f}, 12.0f, clip, white, 6);
    assert(canvas.vertices().size() - n == 3 * 6 + 4 * 6 * 3);
    n = canvas.vertices().size();

    // 弧环：segments 个四边形 = 6*segments；零扫角跳过。
    canvas.drawArc(50.0f, 50.0f, 30.0f, 8.0f, 0.0f, 1.5f, clip, white, 9);
    assert(canvas.vertices().size() - n == 9 * 6);
    n = canvas.vertices().size();
    canvas.drawArc(50.0f, 50.0f, 30.0f, 8.0f, 0.0f, 0.0f, clip, white, 9);
    assert(canvas.vertices().size() == n);

    // 凸多边形：count-2 个三角形。
    const float pentagon[10] = {0, -30, 28, -9, 17, 24, -17, 24, -28, -9};
    canvas.drawConvexPolygon(pentagon, 5, clip, white);
    assert(canvas.vertices().size() - n == 3 * 3);
    n = canvas.vertices().size();

    // 矩形描边：4 个矩形；圆角描边：4 矩形 + 4 角弧。
    canvas.strokeRect({0.0f, 0.0f, 100.0f, 60.0f}, 3.0f, clip, white);
    assert(canvas.vertices().size() - n == 4 * 6);
    n = canvas.vertices().size();
    canvas.strokeRoundRect({0.0f, 0.0f, 100.0f, 60.0f}, 16.0f, 4.0f, clip, white, 5);
    assert(canvas.vertices().size() - n == 4 * 6 + 4 * 5 * 6);
    n = canvas.vertices().size();

    // 渐变矩形：6 顶点，左上角取 c0、右下角取 c1。
    canvas.drawRectGradient({0.0f, 0.0f, 80.0f, 40.0f}, cyan, white, true, clip);
    assert(canvas.vertices().size() - n == 6);
    const auto& v0 = canvas.vertices()[n];
    const auto& v2 = canvas.vertices()[n + 2];
    assert(v0.r == cyan.r && v0.g == cyan.g && v0.b == cyan.b);
    assert(v2.r == white.r && v2.g == white.g && v2.b == white.b);
    n = canvas.vertices().size();

    // 图像：登记 2x2 纹理后 drawImage 发射 6 顶点、批次纹理号 = 句柄；
    // 无效句柄与子区域贴图各自行为正确。
    const uint32_t pixels[4] = {0xFFFFFFFFu, 0x22D3EEFFu, 0x6366F1FFu, 0x00000000u};
    const evk::ui::TextureId tex = store.addTexture(2, 2, pixels);
    assert(tex != evk::ui::kInvalidTexture);
    // 0xRRGGBBAA 必须显式导出为 R,G,B,A 字节，不能受 CPU 端序影响。
    uint8_t rgbaBytes[16] = {};
    assert(store.copyRgbaBytes(tex, rgbaBytes, sizeof(rgbaBytes)));
    assert(rgbaBytes[0] == 0xFF && rgbaBytes[1] == 0xFF &&
           rgbaBytes[2] == 0xFF && rgbaBytes[3] == 0xFF);
    assert(rgbaBytes[4] == 0x22 && rgbaBytes[5] == 0xD3 &&
           rgbaBytes[6] == 0xEE && rgbaBytes[7] == 0xFF);
    assert(rgbaBytes[12] == 0x00 && rgbaBytes[13] == 0x00 &&
           rgbaBytes[14] == 0x00 && rgbaBytes[15] == 0x00);
    assert(!store.copyRgbaBytes(tex, rgbaBytes, sizeof(rgbaBytes) - 1));

    // mip 链：2x2 位图应有 2 级（2x2 → 1x1），链总长 16 + 4 = 20 字节；
    // level 1 是四个源像素的「按 alpha 加权预乘平均」。
    assert(store.mipmapped(tex));
    assert(store.mipLevelCount(tex) == 2);
    assert(store.mipChainBytes(tex) == 20);
    uint8_t mipChain[20] = {};
    assert(store.copyMipChain(tex, mipChain, sizeof(mipChain)));
    // level 0 与 copyRgbaBytes 导出完全一致。
    assert(std::memcmp(mipChain, rgbaBytes, 16) == 0);
    // level 1：sumA = 765 → a = (765+2)/4 = 191；
    // r = (255*(255+34+99) + 765/2)/765 = 129，g = 189，b = 245。
    assert(mipChain[16] == 129 && mipChain[17] == 189 &&
           mipChain[18] == 245 && mipChain[19] == 191);
    assert(!store.copyMipChain(tex, mipChain, sizeof(mipChain) - 1));

    // 非 mipmapped 纹理（字形 atlas 页的方式）：恒 1 级，链 = 原图本身。
    const evk::ui::TextureId flat = store.addTexture(2, 2, pixels, false);
    assert(!store.mipmapped(flat));
    assert(store.mipLevelCount(flat) == 1);
    assert(store.mipChainBytes(flat) == 16);
    uint8_t flatBytes[16] = {};
    assert(store.copyMipChain(flat, flatBytes, sizeof(flatBytes)));
    assert(std::memcmp(flatBytes, rgbaBytes, 16) == 0);
    canvas.drawImage(tex, {0.0f, 0.0f, 20.0f, 20.0f}, clip);
    assert(canvas.vertices().size() - n == 6);
    assert(canvas.batches().back().textureId == tex);
    n = canvas.vertices().size();
    canvas.drawImage(evk::ui::kInvalidTexture, {0.0f, 0.0f, 20.0f, 20.0f}, clip);
    assert(canvas.vertices().size() == n);
    canvas.drawImageRect(tex, {0.0f, 0.0f, 10.0f, 10.0f}, 0.0f, 0.0f, 0.5f, 0.5f, clip);
    assert(canvas.vertices().size() - n == 6);
    assert(canvas.vertices()[n].u == 0.0f && canvas.vertices()[n + 1].u == 0.5f);

    // 合批：同 clip + 同纹理的连续几何合并成一个批次。
    canvas.clear();
    canvas.drawRect({0.0f, 0.0f, 10.0f, 10.0f}, clip, white);
    canvas.drawCircle(50.0f, 50.0f, 30.0f, clip, white, 8);
    canvas.drawLine(0.0f, 0.0f, 30.0f, 30.0f, 2.0f, clip, white);
    assert(canvas.batches().size() == 1);
    // 纹理不同则拆批。
    canvas.drawImage(tex, {0.0f, 0.0f, 20.0f, 20.0f}, clip);
    assert(canvas.batches().size() == 2);
    assert(canvas.batches()[1].textureId == tex);

    // Container 圆角/描边与 ImageWidget 挂树渲染不崩。
    auto card = std::make_unique<evk::ui::Container>(0x1E293BFF);
    card->cornerRadius = 16.0f;
    card->borderColor = 0x22D3EEFF;
    card->borderWidth = 3.0f;
    evk::ui::runApp(
        evk::ui::column(evk::ui::image(tex), std::move(card)), {});
    evk::ui::setViewportSize(400.0f, 800.0f);
    evk::beginFrame(ms(10));
    evk::ui::shutdownApp();

    store.reset();
}

} // namespace

int main(int argc, char** argv) {
    testAppLifecycleCallback();
    testDirtyFramesAreCoalesced();
    testViewTreePaintingAndInput();
    testButtonStateMachine();
    testFlexLayout();
    testScrollViewDragAndClamp();
    testListViewLayoutAndScroll();
    testScrollAxisLock();
    testEventBusAndUiQueue();
    testStateAndNavigatorLifecycle();
    testAnimatedNavigationAndBackEvent();
    testSafeAreaInsets();

    testCanvas2dPrimitives();

    // 字体测试需要两个字体文件路径（拉丁 + 中文）。
    if (argc >= 3) {
        testFontEngine(argv[1], argv[2]);
        testTextLayout(argv[1], argv[2]);
    } else {
        std::printf("font tests skipped (pass latin.ttf cjk.ttf as args)\n");
    }
    return 0;
}
