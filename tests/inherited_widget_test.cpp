/**
 * @file inherited_widget_test.cpp
 * @brief 树内依赖下发（InheritedWidget）的运行时测试。
 *
 * 覆盖：最近祖先查找（嵌套同名内层遮蔽外层）、数据变更后读者读到新值、
 * 通知纪元去重（每次变更读者恰好重建一次，不重复构建）、依赖方 unmount
 * 后摘除登记（变更不再触及已死节点）、无祖先时 dependOn 返回 nullptr、
 * find 只读版同样取到最近祖先。
 *
 * 与 ui_runtime_test 同式：assert 断言 + runApp 挂载 + State::latest 取
 * 页面状态。setState 同步重建，断言无需泵帧。
 */

#include <cassert>
#include <cstdio>
#include <memory>
#include <utility>

#include "evk/app_lifecycle.h"
#include "evk/frame_scheduler.h"
#include "evk/ui/animation_scheduler.h"
#include "evk/ui/event_bus.h"
#include "evk/ui/pointer_input.h"
#include "evk/ui/ui_application.h"
#include "evk/ui/widgets.h"

namespace {

int g_builds[8] = {};
int g_values[8] = {};

void resetRuntime() {
    evk::ui::shutdownApp();
    evk::ui::cancelAllPointerEvents();
    evk::ui::stopAllAnimations();
    evk::ui::EventBus::instance().clear();
    evk::cancelPendingFrame();
    for (int i = 0; i < 8; ++i) {
        g_builds[i] = 0;
        g_values[i] = 0;
    }
}

/// 下发一个 int 的测试主题。
class TestTheme final : public evk::ui::InheritedWidget {
public:
    TestTheme(int value, std::unique_ptr<evk::ui::Widget> child)
        : InheritedWidget(std::move(child)), value_(value) {}

    int value() const { return value_; }

    bool updateShouldNotify(const InheritedWidget& oldWidget) const override {
        return value_ != static_cast<const TestTheme&>(oldWidget).value_;
    }

private:
    int value_;
};

/// build 时 dependOn 登记依赖并读值（无祖先时记 -1）。
class ReaderWidget final : public evk::ui::StatelessWidget {
public:
    explicit ReaderWidget(int id) : id_(id) {}

    std::unique_ptr<evk::ui::Widget> build(evk::ui::BuildContext& context) const override {
        ++g_builds[id_];
        const TestTheme* theme = context.dependOnInheritedWidgetOfExactType<TestTheme>();
        g_values[id_] = theme ? theme->value() : -1;
        return evk::ui::container(theme ? 0x00FF00FF : 0xFF0000FF);
    }

private:
    int id_;
};

/// 只读版：find 不登记依赖。
class FinderWidget final : public evk::ui::StatelessWidget {
public:
    explicit FinderWidget(int id) : id_(id) {}

    std::unique_ptr<evk::ui::Widget> build(evk::ui::BuildContext& context) const override {
        ++g_builds[id_];
        const TestTheme* theme = context.findInheritedWidgetOfExactType<TestTheme>();
        g_values[id_] = theme ? theme->value() : -1;
        return evk::ui::container(0xFFFFFFFF);
    }

private:
    int id_;
};

class HostState;

class HostPage final : public evk::ui::StatefulWidget {
public:
    std::unique_ptr<evk::ui::State> createState() const override;
};

/// 树：TestTheme(outer) > Column[Reader0?, TestTheme(inner) > [Reader1, Finder2]]
class HostState final : public evk::ui::State {
public:
    static HostState* latest;

    HostState() { latest = this; }
    ~HostState() override {
        if (latest == this) {
            latest = nullptr;
        }
    }

    std::unique_ptr<evk::ui::Widget> build(evk::ui::BuildContext&) override {
        std::vector<std::unique_ptr<evk::ui::Widget>> innerKids;
        innerKids.push_back(evk::ui::makeWidget<ReaderWidget>(1));
        innerKids.push_back(evk::ui::makeWidget<FinderWidget>(2));

        std::vector<std::unique_ptr<evk::ui::Widget>> outerKids;
        if (outerReaderMounted) {
            outerKids.push_back(evk::ui::makeWidget<ReaderWidget>(0));
        }
        outerKids.push_back(
            evk::ui::makeWidget<TestTheme>(innerValue, evk::ui::column(std::move(innerKids))));
        return evk::ui::makeWidget<TestTheme>(outerValue, evk::ui::column(std::move(outerKids)));
    }

    int outerValue = 1;
    int innerValue = 2;
    bool outerReaderMounted = true;
};

HostState* HostState::latest = nullptr;

std::unique_ptr<evk::ui::State> HostPage::createState() const {
    return std::make_unique<HostState>();
}

/// 嵌套同名 InheritedWidget：读者命中最近祖先（内层遮蔽外层）。
void testNearestAncestorWins() {
    resetRuntime();
    evk::ui::setViewportSize(400.0f, 800.0f);
    evk::ui::runApp(evk::ui::makeWidget<HostPage>());

    assert(g_builds[0] == 1 && g_values[0] == 1);  // 外层读者读外层
    assert(g_builds[1] == 1 && g_values[1] == 2);  // 内层读者读内层
    assert(g_builds[2] == 1 && g_values[2] == 2);  // find 只读版同样命中内层
}

/// 外层数据变更：外层读者读到新值；内层读者值不变。同步全递归重建下
/// 每个读者恰好多构建一次（纪元去重，通知不产生重复构建）。
void testChangePropagatesExactlyOnce() {
    resetRuntime();
    evk::ui::setViewportSize(400.0f, 800.0f);
    evk::ui::runApp(evk::ui::makeWidget<HostPage>());

    HostState::latest->setState([] {});
    // updateShouldNotify 为 false：读者仍随递归重建（结构对比的自然结果），
    // 值不变；重建次数恰好 +1（通知未触发）。
    assert(g_builds[0] == 2 && g_values[0] == 1);
    assert(g_builds[1] == 2 && g_values[1] == 2);

    HostState::latest->setState([] { HostState::latest->outerValue = 3; });
    assert(g_values[0] == 3);            // 外层读者拿到新值
    assert(g_values[1] == 2);            // 内层读者不受外层变更影响
    assert(g_builds[0] == 3);            // 恰好 +1，不是 +2（无重复构建）
    assert(g_builds[1] == 3);

    HostState::latest->setState([] { HostState::latest->innerValue = 7; });
    assert(g_values[0] == 3);
    assert(g_values[1] == 7);
    assert(g_builds[1] == 4);
}

/// 依赖方 unmount 后摘除登记：再变更数据不触及已死节点（无悬空、不重建）。
void testUnmountRemovesDependency() {
    resetRuntime();
    evk::ui::setViewportSize(400.0f, 800.0f);
    evk::ui::runApp(evk::ui::makeWidget<HostPage>());
    assert(g_builds[0] == 1);

    // 摘掉外层读者：0 号槽位类型从 Reader 变 TestTheme，Reader0 子树销毁。
    HostState::latest->setState([] { HostState::latest->outerReaderMounted = false; });
    const int innerBuilds = g_builds[1];

    // 外层数据再变更：若登记未摘除，通知/递归会撞上已销毁的 Reader0。
    HostState::latest->setState([] { HostState::latest->outerValue = 9; });
    assert(g_builds[0] == 1);            // 死者不再重建
    assert(g_builds[1] == innerBuilds + 1);
    assert(g_values[1] == 2);
}

/// 树上没有对应 InheritedWidget 时 dependOn / find 返回 nullptr。
void testMissingAncestorReturnsNull() {
    resetRuntime();
    evk::ui::setViewportSize(400.0f, 800.0f);
    evk::ui::runApp(evk::ui::makeWidget<ReaderWidget>(5));
    assert(g_builds[5] == 1 && g_values[5] == -1);
}

} // namespace

int main() {
    testNearestAncestorWins();
    std::printf("ok: nearest ancestor wins\n");
    testChangePropagatesExactlyOnce();
    std::printf("ok: change propagates exactly once\n");
    testUnmountRemovesDependency();
    std::printf("ok: unmount removes dependency\n");
    testMissingAncestorReturnsNull();
    std::printf("ok: missing ancestor returns null\n");
    std::printf("inherited_widget_test: all passed\n");
    return 0;
}
