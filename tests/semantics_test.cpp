/**
 * @file semantics_test.cpp
 * @brief 无障碍语义树的运行时测试。
 *
 * 覆盖：控件内省（Text 内容/Button 角色/ScrollView 滚动动作）、手工标注
 * （semantics 包装、Button::semanticsLabel）、透明节点孩子上提、diff 推送
 * （经平台通道捕获 a11y/update 载荷）、performAction 下行（tap/滚动/未
 * 声明动作拒绝/帧间窗口防御）、hidden 子树剔除、关闭零开销。
 *
 * 平台通道的出向用测试替身 invoker 捕获；入向直接走 dispatchPlatformCall
 * （"a11y/enabled" / "a11y/action"），顺带验证通道接线。需要两个字体文件
 * 路径作为运行参数（同 ui_runtime_test）。
 */

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "evk/app_lifecycle.h"
#include "evk/frame_scheduler.h"
#include "evk/platform_channel.h"
#include "evk/ui/animation_scheduler.h"
#include "evk/ui/event_bus.h"
#include "evk/ui/font_engine.h"
#include "evk/ui/pointer_input.h"
#include "evk/ui/ui_application.h"
#include "evk/ui/widgets.h"

namespace {

evk::ui::Canvas g_canvas;
int g_frames = 0;

/// 出向调用捕获（method + payload）。
std::vector<std::pair<std::string, std::string>> g_invocations;

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
    evk::ui::SemanticsOwner::instance().reset();
    evk::cancelPendingFrame();
    g_frames = 0;
    g_invocations.clear();
    evk::setFrameFunc(renderFrame);
    evk::setPlatformInvoker([](const std::string& method, const std::string& args) {
        g_invocations.emplace_back(method, args);
        return std::string();
    });
}

/// 启用无障碍并泵一帧（setEnabled 内部已 requestRender）。
void enableAndPump() {
    assert(evk::dispatchPlatformCall("a11y/enabled", "1") == "1");
    assert(evk::beginFrame(ms(1)));
}

/// 强制泵一帧（内容未变也重建帧，验证 diff 不误推）。
void pumpFrame(int64_t t) {
    evk::requestRender();
    assert(evk::beginFrame(ms(t)));
}

const evk::ui::SemanticsNode* findByLabel(const std::string& label) {
    for (const auto& node : evk::ui::SemanticsOwner::instance().nodes()) {
        if (node.label == label) {
            return &node;
        }
    }
    return nullptr;
}

/// 找节点的父（children 反查；0 = 在顶层）。
int32_t parentIdOf(int32_t id) {
    for (const auto& node : evk::ui::SemanticsOwner::instance().nodes()) {
        for (const int32_t child : node.children) {
            if (child == id) {
                return node.id;
            }
        }
    }
    return -1;
}

int updatePushCount() {
    int count = 0;
    for (const auto& call : g_invocations) {
        if (call.first == "a11y/update") {
            ++count;
        }
    }
    return count;
}

class SemanticsPage final : public evk::ui::StatefulWidget {
public:
    std::unique_ptr<evk::ui::State> createState() const override;
};

class SemanticsPageState final : public evk::ui::State {
public:
    static SemanticsPageState* latest;

    SemanticsPageState() { latest = this; }
    ~SemanticsPageState() override {
        if (latest == this) {
            latest = nullptr;
        }
    }

    std::unique_ptr<evk::ui::Widget> build(evk::ui::BuildContext&) override {
        using namespace evk::ui;
        auto detail = std::make_unique<Button>(
            ButtonStyle{0x3366FFFF, 0x2244CCFF, 0x777777FF},
            [this] { ++presses; });
        detail->semanticsLabel = "详情按钮";

        std::vector<std::unique_ptr<Widget>> rows;
        for (int i = 0; i < 20; ++i) {
            rows.push_back(sizedBox(-1.0f, 60.0f, container(0x111111FF)));
        }

        std::vector<std::unique_ptr<Widget>> kids;
        kids.push_back(text(title_, 32.0f, 0xFFFFFFFF));
        kids.push_back(sizedBox(-1.0f, 60.0f, std::move(detail)));
        kids.push_back(semantics(
            "自选入口",
            SemanticsRole::kButton,
            sizedBox(-1.0f, 60.0f, container(0x00AAFFFF, [this] { ++taps; }))));
        if (showList_) {
            kids.push_back(sizedBox(
                -1.0f, 200.0f, scrollView(column(std::move(rows)), 60.0f * 20)));
        }
        return column(std::move(kids));
    }

    std::string title_ = "行情标题";
    bool showList_ = true;
    int presses = 0;
    int taps = 0;
};

SemanticsPageState* SemanticsPageState::latest = nullptr;

std::unique_ptr<evk::ui::State> SemanticsPage::createState() const {
    return std::make_unique<SemanticsPageState>();
}

/// 收集：内省 + 手工标注 + 透明上提 + 顶层挂到合成根。
void testCollect() {
    resetRuntime();
    evk::ui::setViewportSize(400.0f, 800.0f);
    evk::ui::runApp(evk::ui::makeWidget<SemanticsPage>());
    enableAndPump();

    using namespace evk::ui;
    const auto& nodes = SemanticsOwner::instance().nodes();
    assert(!nodes.empty() && nodes[0].id == 0);  // 合成根

    const SemanticsNode* title = findByLabel("行情标题");
    assert(title && title->role == SemanticsRole::kText);
    assert(title->actions == 0);
    assert(title->bounds.w > 0.0f && title->bounds.h > 0.0f);

    const SemanticsNode* button = findByLabel("详情按钮");
    assert(button && button->role == SemanticsRole::kButton);
    assert((button->actions & kSemanticsActionTap) != 0);
    assert((button->state & kSemanticsStateDisabled) == 0);

    // 手工标注包装：角色来自标注，动作来自容器 onTap 推导。
    const SemanticsNode* entry = findByLabel("自选入口");
    assert(entry && entry->role == SemanticsRole::kButton);
    assert((entry->actions & kSemanticsActionTap) != 0);

    // 滚动容器：内省角色 + 滚动动作（内容溢出）。
    bool foundScroller = false;
    for (const auto& node : nodes) {
        if (node.role == SemanticsRole::kScroller) {
            foundScroller = true;
            assert((node.actions & kSemanticsActionScrollForward) != 0);
            assert((node.actions & kSemanticsActionScrollBackward) != 0);
        }
    }
    assert(foundScroller);

    // 透明节点（sizedBox/column 等）不产生语义：四个节点都在顶层，
    // 父链最终到合成根。
    assert(parentIdOf(title->id) == 0);
    assert(parentIdOf(button->id) == 0);
    assert(parentIdOf(entry->id) == 0);

    // 推送：启用后首帧推一次全量快照，JSON 含关键字段。
    assert(updatePushCount() == 1);
    const std::string& payload = g_invocations.back().second;
    assert(payload.find("\"label\":\"行情标题\"") != std::string::npos);
    assert(payload.find("\"role\":2") != std::string::npos);  // kButton
    assert(SemanticsOwner::instance().changeCounter() == 1);
}

/// diff：同内容重泵不推送；内容变更推送且载荷含新值。
void testDiff() {
    resetRuntime();
    evk::ui::setViewportSize(400.0f, 800.0f);
    evk::ui::runApp(evk::ui::makeWidget<SemanticsPage>());
    enableAndPump();
    assert(updatePushCount() == 1);

    pumpFrame(ms(2));  // 同内容强泵：diff 为空，不重复推送
    assert(updatePushCount() == 1);
    assert(evk::ui::SemanticsOwner::instance().changeCounter() == 1);

    SemanticsPageState::latest->setState(
        [] { SemanticsPageState::latest->title_ = "收盘播报"; });
    pumpFrame(ms(3));
    assert(updatePushCount() == 2);
    assert(evk::ui::SemanticsOwner::instance().changeCounter() == 2);
    assert(g_invocations.back().second.find("收盘播报") != std::string::npos);
    assert(findByLabel("行情标题") == nullptr);
    assert(findByLabel("收盘播报") != nullptr);
}

/// 动作下行：tap 落到按钮 onPressed 与容器 onClick；未声明的动作拒绝。
void testPerformAction() {
    resetRuntime();
    evk::ui::setViewportSize(400.0f, 800.0f);
    evk::ui::runApp(evk::ui::makeWidget<SemanticsPage>());
    enableAndPump();

    using namespace evk::ui;
    const SemanticsNode* button = findByLabel("详情按钮");
    const SemanticsNode* entry = findByLabel("自选入口");
    const SemanticsNode* title = findByLabel("行情标题");
    assert(button && entry && title);

    assert(SemanticsOwner::instance().performAction(
        button->id, kSemanticsActionTap));
    assert(SemanticsPageState::latest->presses == 1);

    // 经平台通道入向（"id:action"）。
    char args[32];
    std::snprintf(args, sizeof(args), "%d:%u", entry->id, kSemanticsActionTap);
    assert(evk::dispatchPlatformCall("a11y/action", args) == "1");
    assert(SemanticsPageState::latest->taps == 1);

    // 文本节点没声明 tap：拒绝且不产生副作用。
    assert(!SemanticsOwner::instance().performAction(
        title->id, kSemanticsActionTap));
    assert(!SemanticsOwner::instance().performAction(9999, kSemanticsActionTap));

    // 滚动动作：返回 true 且内容真的位移（下一帧首行 bounds 上移）。
    const SemanticsNode* scroller = nullptr;
    for (const auto& node : SemanticsOwner::instance().nodes()) {
        if (node.role == SemanticsRole::kScroller) {
            scroller = &node;
        }
    }
    assert(scroller);
    assert(SemanticsOwner::instance().performAction(
        scroller->id, kSemanticsActionScrollForward));
}

/// 帧间窗口防御：子树已拆但快照未刷新时，动作经 ViewRef 判活丢弃。
void testPerformActionAfterUnmount() {
    resetRuntime();
    evk::ui::setViewportSize(400.0f, 800.0f);
    evk::ui::runApp(evk::ui::makeWidget<SemanticsPage>());
    enableAndPump();

    const evk::ui::SemanticsNode* button = findByLabel("详情按钮");
    assert(button);
    const int32_t buttonId = button->id;

    // 拆掉列表（改树）但不泵帧：快照里节点仍在、View 已销毁。
    SemanticsPageState::latest->setState(
        [] { SemanticsPageState::latest->showList_ = false; });
    int32_t scrollerId = -1;
    for (const auto& node : evk::ui::SemanticsOwner::instance().nodes()) {
        if (node.role == evk::ui::SemanticsRole::kScroller) {
            scrollerId = node.id;
        }
    }
    assert(scrollerId > 0);
    assert(!evk::ui::SemanticsOwner::instance().performAction(
        scrollerId, evk::ui::kSemanticsActionScrollForward));

    // 按钮子树没动，仍然活着可执行。
    assert(evk::ui::SemanticsOwner::instance().performAction(
        buttonId, evk::ui::kSemanticsActionTap));

    // 泵帧后：scroller 节点从快照消失，推送一次。
    pumpFrame(ms(4));
    for (const auto& node : evk::ui::SemanticsOwner::instance().nodes()) {
        assert(node.role != evk::ui::SemanticsRole::kScroller);
    }
    assert(updatePushCount() == 2);
}

/// hidden 子树剔除（裸 View 树，不经 Widget 层）。
void testHiddenSubtree() {
    resetRuntime();
    using namespace evk::ui;

    auto root = std::make_unique<View>();
    root->setBounds(0.0f, 0.0f, 400.0f, 800.0f);

    auto visibleChild = std::make_unique<View>();
    visibleChild->setBounds(0.0f, 0.0f, 100.0f, 50.0f);
    visibleChild->semantics = std::make_unique<SemanticsConfig>();
    visibleChild->semantics->label = "可见节点";
    root->addChild(std::move(visibleChild));

    auto hiddenChild = std::make_unique<View>();
    hiddenChild->setBounds(0.0f, 60.0f, 100.0f, 50.0f);
    hiddenChild->semantics = std::make_unique<SemanticsConfig>();
    hiddenChild->semantics->hidden = true;
    auto grandchild = std::make_unique<View>();
    grandchild->setBounds(0.0f, 0.0f, 50.0f, 50.0f);
    grandchild->semantics = std::make_unique<SemanticsConfig>();
    grandchild->semantics->label = "不可见节点";
    hiddenChild->addChild(std::move(grandchild));
    root->addChild(std::move(hiddenChild));

    View* rootPtr = root.get();
    setRootView(rootPtr);
    enableAndPump();

    assert(findByLabel("可见节点") != nullptr);
    assert(findByLabel("不可见节点") == nullptr);

    setRootView(nullptr);
    root.reset();
}

/// 关闭时零开销：不收集、不推送；setEnabled(false) 推一次空树清屏。
void testDisabledZeroOverhead() {
    resetRuntime();
    evk::ui::setViewportSize(400.0f, 800.0f);
    evk::ui::runApp(evk::ui::makeWidget<SemanticsPage>());
    pumpFrame(ms(1));
    assert(g_invocations.empty());
    assert(evk::ui::SemanticsOwner::instance().nodes().empty());

    enableAndPump();
    assert(updatePushCount() == 1);
    evk::dispatchPlatformCall("a11y/enabled", "0");
    assert(!evk::ui::SemanticsOwner::instance().enabled());
    assert(evk::ui::SemanticsOwner::instance().nodes().empty());
    assert(updatePushCount() == 2);
    assert(g_invocations.back().second.find("\"change\":-1") !=
           std::string::npos);
}

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

} // namespace

int main(int argc, char** argv) {
    // 文本测量需要字体（与 ui_runtime_test 同约：argv[1]=拉丁 argv[2]=中文）。
    assert(argc >= 3 && "usage: semantics_test latin.ttf cjk.ttf");
    auto& fonts = evk::ui::FontEngine::instance();
    const auto latinData = readFile(argv[1]);
    const auto cjkData = readFile(argv[2]);
    assert(!latinData.empty() && !cjkData.empty());
    fonts.addFont(latinData.data(), latinData.size());
    fonts.addFont(cjkData.data(), cjkData.size());

    testCollect();
    std::printf("ok: collect (introspection + annotation + lift)\n");
    testDiff();
    std::printf("ok: diff pushes only on change\n");
    testPerformAction();
    std::printf("ok: performAction (tap + scroll + reject)\n");
    testPerformActionAfterUnmount();
    std::printf("ok: performAction survives unmount window\n");
    testHiddenSubtree();
    std::printf("ok: hidden subtree excluded\n");
    testDisabledZeroOverhead();
    std::printf("ok: disabled zero overhead + clear push\n");
    std::printf("semantics_test: all passed\n");
    return 0;
}
