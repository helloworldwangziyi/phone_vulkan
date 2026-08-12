#include <cassert>
#include <cstdint>

#include "evk/esx_view.h"
#include "evk/render_loop.h"
#include "evk/ui/animator.h"
#include "evk/ui/controls/button.h"
#include "evk/ui/controls/navigation.h"
#include "evk/ui/controls/scroll_view.h"
#include "evk/ui/canvas.h"
#include "evk/ui/input.h"

namespace {

// 毫秒 → 纳秒（PointerEvent 时间戳，供速度计算）。
int64_t ms(int64_t value) {
    return value * 1'000'000;
}

int g_frameCount = 0;
bool g_requestAnotherFrame = false;

void renderFrame(int64_t /*frameTimeNanos*/) {
    ++g_frameCount;
    if (g_requestAnotherFrame) {
        g_requestAnotherFrame = false;
        evk::requestRender();
    }
}

void testDirtyFramesAreCoalesced() {
    evk::setFrameFunc(renderFrame);
    evk::cancelPendingFrame();
    g_frameCount = 0;

    evk::requestRender();
    evk::requestRender();
    assert(evk::beginFrame(1));
    assert(g_frameCount == 1);
    assert(!evk::beginFrame(2));

    g_requestAnotherFrame = true;
    evk::requestRender();
    assert(evk::beginFrame(3));
    assert(evk::beginFrame(4));
    assert(g_frameCount == 3);
}

void countViewClick(esx_view /*view*/, const esx_view_click_event* /*event*/,
                    void* userData) {
    ++*static_cast<int*>(userData);
}

void testViewClickAndDragCancellation() {
    int clicks = 0;
    const esx_view root = esx_create_view(0, 0, 200, 200, 0);
    const esx_view target = esx_create_view(10, 10, 100, 100, root);
    esx_set_root_view(root);
    esx_view_set_click_callback(target, countViewClick, &clicks);

    using evk::ui::PointerAction;
    using evk::ui::PointerEvent;
    evk::ui::dispatchPointerEvent({PointerAction::Down, 0, 20, 20});
    evk::ui::dispatchPointerEvent({PointerAction::Up, 0, 20, 20});
    assert(clicks == 1);

    evk::ui::dispatchPointerEvent({PointerAction::Down, 0, 20, 20});
    evk::ui::dispatchPointerEvent({PointerAction::Move, 0, 50, 20});
    evk::ui::dispatchPointerEvent({PointerAction::Up, 0, 50, 20});
    assert(clicks == 1);

    evk::ui::dispatchPointerEvent({PointerAction::Down, 0, 20, 20});
    esx_view_set_bounds(target, 100, 100, 50, 50);
    evk::ui::dispatchPointerEvent({PointerAction::Up, 0, 20, 20});
    assert(clicks == 1);

    esx_destroy_view(root);
}

void countButtonClick(esx_view /*button*/, void* userData) {
    ++*static_cast<int*>(userData);
}

struct DestroyOnClickState {
    esx_view root = 0;
    int clicks = 0;
};

struct DestroyOnCancelState {
    esx_view viewToDestroy = 0;
    int cancels = 0;
};

void destroyRootOnClick(esx_view /*button*/, void* userData) {
    auto* state = static_cast<DestroyOnClickState*>(userData);
    ++state->clicks;
    esx_destroy_view(state->root);
}

void destroyViewOnPanCancel(esx_view /*view*/, const esx_view_pan_event* event,
                            void* userData) {
    auto* state = static_cast<DestroyOnCancelState*>(userData);
    if (event->state == ESX_VIEW_PAN_CANCEL) {
        ++state->cancels;
        esx_destroy_view(state->viewToDestroy);
    }
}

void testScrollViewTakesDragFromChildButton() {
    int clicks = 0;
    const esx_view root = esx_create_view(0, 0, 200, 200, 0);
    esx_set_root_view(root);
    const esx_view scroll = esx_scroll_view_create(0, 0, 100, 100, 100, 300, root);
    const esx_view content = esx_scroll_view_get_content(scroll);
    const esx_view button = esx_button_create(0, 20, 100, 50, content, nullptr,
                                              countButtonClick, &clicks);
    assert(button != 0);

    using evk::ui::PointerAction;
    evk::ui::dispatchPointerEvent({PointerAction::Down, 0, 20, 30});
    evk::ui::dispatchPointerEvent({PointerAction::Up, 0, 20, 30});
    assert(clicks == 1);

    evk::ui::dispatchPointerEvent({PointerAction::Down, 0, 20, 30, ms(1000)});
    evk::ui::dispatchPointerEvent({PointerAction::Move, 0, 20, 5, ms(1020)});
    evk::ui::dispatchPointerEvent({PointerAction::Up, 0, 20, 5, ms(1200)});
    assert(clicks == 1);

    float offsetX = 0;
    float offsetY = 0;
    esx_scroll_view_get_offset(scroll, &offsetX, &offsetY);
    assert(offsetX == 0);
    assert(offsetY == 25);

    esx_scroll_view_set_offset(scroll, 1000, 1000);
    esx_scroll_view_get_offset(scroll, &offsetX, &offsetY);
    assert(offsetX == 0);
    assert(offsetY == 200);

    esx_destroy_view(root);
}

void testScrollViewReclampsOffsetAfterResize() {
    const esx_view root = esx_create_view(0, 0, 200, 300, 0);
    esx_set_root_view(root);
    const esx_view scroll = esx_scroll_view_create(0, 0, 100, 100, 100, 300, root);

    esx_scroll_view_set_offset(scroll, 0, 200);
    esx_view_set_bounds(scroll, 0, 0, 100, 200);

    float offsetY = 0;
    esx_scroll_view_get_offset(scroll, nullptr, &offsetY);
    assert(offsetY == 100);

    esx_destroy_view(root);

    const esx_view smallerRoot = esx_create_view(0, 0, 100, 100, 0);
    esx_set_root_view(smallerRoot);
    const esx_view smallerContent =
        esx_scroll_view_create(0, 0, 100, 100, 50, 50, smallerRoot);
    esx_view_set_bounds(smallerContent, 0, 0, 50, 50);
    esx_scroll_view_set_offset(smallerContent, 100, 100);
    float offsetX = -1;
    offsetY = -1;
    esx_scroll_view_get_offset(smallerContent, &offsetX, &offsetY);
    assert(offsetX == 0);
    assert(offsetY == 0);

    esx_destroy_view(smallerRoot);
}

void testClickRespectsParentClip() {
    const esx_view root = esx_create_view(0, 0, 200, 100, 0);
    esx_set_root_view(root);
    const esx_view parent = esx_create_view(0, 0, 100, 100, root);
    int clicks = 0;
    const esx_view button = esx_button_create(95, 0, 50, 50, parent, nullptr,
                                              countButtonClick, &clicks);
    assert(button != 0);

    using evk::ui::PointerAction;
    evk::ui::dispatchPointerEvent({PointerAction::Down, 0, 99, 20});
    evk::ui::dispatchPointerEvent({PointerAction::Up, 0, 105, 20});
    assert(clicks == 0);

    const esx_view plainView = esx_create_view(95, 50, 50, 50, parent);
    esx_view_set_click_callback(plainView, countViewClick, &clicks);
    evk::ui::dispatchPointerEvent({PointerAction::Down, 0, 99, 70});
    evk::ui::dispatchPointerEvent({PointerAction::Up, 0, 105, 70});
    assert(clicks == 0);

    esx_destroy_view(root);
}

struct DrawMutationState {
    esx_view root = 0;
    esx_view attemptedChild = 0;
};

void mutateTreeDuringDraw(esx_view /*view*/, void* userData) {
    auto* state = static_cast<DrawMutationState*>(userData);
    state->attemptedChild = esx_create_view(0, 0, 10, 10, state->root);
    esx_destroy_view(state->root);
}

void testTreeMutationDuringDrawIsRejected() {
    DrawMutationState state;
    state.root = esx_create_view(0, 0, 100, 100, 0);
    esx_set_root_view(state.root);
    esx_view_set_draw_callback(state.root, mutateTreeDuringDraw, &state);

    evk::ui::Canvas canvas;
    esxBuildFrame(canvas);

    assert(state.attemptedChild == 0);
    assert(esxViewFromHandle(state.root) != nullptr);
    assert(esxRootView() == esxViewFromHandle(state.root));

    esx_destroy_view(state.root);
}

void testInputLifecycleCleanup() {
    using evk::ui::PointerAction;

    DestroyOnClickState state;
    state.root = esx_create_view(0, 0, 200, 200, 0);
    esx_set_root_view(state.root);
    const esx_view button = esx_button_create(0, 0, 100, 100, state.root, nullptr,
                                              destroyRootOnClick, &state);
    assert(button != 0);
    evk::ui::dispatchPointerEvent({PointerAction::Down, 0, 20, 20});
    evk::ui::dispatchPointerEvent({PointerAction::Up, 0, 20, 20});
    assert(state.clicks == 1);
    assert(esxRootView() == nullptr);

    const esx_view root = esx_create_view(0, 0, 200, 200, 0);
    esx_set_root_view(root);
    int clicks = 0;
    const esx_view hiddenButton = esx_button_create(0, 0, 100, 100, root, nullptr,
                                                    countButtonClick, &clicks);
    evk::ui::dispatchPointerEvent({PointerAction::Down, 0, 20, 20});
    esx_view_set_visible(hiddenButton, 0);
    evk::ui::dispatchPointerEvent({PointerAction::Up, 0, 20, 20});
    assert(clicks == 0);

    esx_view_set_visible(hiddenButton, 1);
    evk::ui::dispatchPointerEvent({PointerAction::Down, 0, 20, 20});
    evk::ui::dispatchPointerEvent({PointerAction::Up, 0, 20, 20});
    assert(clicks == 1);

    evk::ui::dispatchPointerEvent({PointerAction::Down, 0, 20, 20});
    esx_button_set_enabled(hiddenButton, 0);
    esx_button_set_enabled(hiddenButton, 1);
    evk::ui::dispatchPointerEvent({PointerAction::Up, 0, 20, 20});
    assert(clicks == 1);

    const esx_view scroll = esx_scroll_view_create(100, 0, 100, 100, 100, 300, root);
    evk::ui::dispatchPointerEvent({PointerAction::Down, 0, 120, 50});
    evk::ui::dispatchPointerEvent({PointerAction::Move, 0, 120, 20});
    esx_destroy_view(scroll);
    evk::ui::dispatchPointerEvent({PointerAction::Up, 0, 120, 20});

    esx_destroy_view(root);
}

void testCancellationCallbacksMayDestroyViews() {
    using evk::ui::PointerAction;

    DestroyOnCancelState hideState;
    hideState.viewToDestroy = esx_create_view(0, 0, 200, 200, 0);
    esx_set_root_view(hideState.viewToDestroy);
    esx_view_set_pan_callback(hideState.viewToDestroy, destroyViewOnPanCancel,
                              &hideState);
    evk::ui::dispatchPointerEvent({PointerAction::Down, 0, 20, 20});
    evk::ui::dispatchPointerEvent({PointerAction::Move, 0, 20, 0});
    esx_view_set_visible(hideState.viewToDestroy, 0);
    assert(hideState.cancels == 1);
    assert(esxRootView() == nullptr);

    const esx_view currentRoot = esx_create_view(0, 0, 200, 200, 0);
    const esx_view candidateRoot = esx_create_view(0, 0, 200, 200, 0);
    esx_set_root_view(currentRoot);
    DestroyOnCancelState switchState{candidateRoot, 0};
    esx_view_set_pan_callback(currentRoot, destroyViewOnPanCancel, &switchState);
    evk::ui::dispatchPointerEvent({PointerAction::Down, 0, 20, 20});
    evk::ui::dispatchPointerEvent({PointerAction::Move, 0, 20, 0});
    esx_set_root_view(candidateRoot);
    assert(switchState.cancels == 1);
    assert(esxViewFromHandle(candidateRoot) == nullptr);
    assert(esxRootView() == esxViewFromHandle(currentRoot));

    esx_destroy_view(currentRoot);
}

// ---- 动画基础设施 ----

struct CountingAnim {
    int ticks = 0;
};

bool countingAnimTick(int64_t /*frameTimeNanos*/, void* userData) {
    auto* anim = static_cast<CountingAnim*>(userData);
    return ++anim->ticks >= 3;
}

void testAnimatorTicksAndFinishes() {
    evk::setFrameFunc(renderFrame);
    evk::cancelPendingFrame();
    evk::ui::stopAllAnimations();
    g_frameCount = 0;

    CountingAnim anim;
    evk::ui::startAnimation(countingAnimTick, &anim, nullptr);
    // startAnimation 自带一次 requestRender，首帧绘制。
    assert(evk::beginFrame(100));
    assert(anim.ticks == 1);
    assert(g_frameCount == 1);
    // 动画不改视图时不 dirty，但 tick 仍每帧走。
    assert(!evk::beginFrame(200));
    assert(anim.ticks == 2);
    // 第三帧 tick 返回 true：动画结束并移除。
    assert(!evk::beginFrame(300));
    assert(anim.ticks == 3);
    // 已移除，不再 tick。
    assert(!evk::beginFrame(400));
    assert(anim.ticks == 3);
    assert(g_frameCount == 1);
}

// ---- 滑动速度 ----

struct PanCapture {
    int ends = 0;
    float endVelocityX = 0.0f;
    float endVelocityY = 0.0f;
};

void capturePan(esx_view /*view*/, const esx_view_pan_event* event, void* userData) {
    auto* capture = static_cast<PanCapture*>(userData);
    if (event->state == ESX_VIEW_PAN_END) {
        ++capture->ends;
        capture->endVelocityX = event->velocity_x;
        capture->endVelocityY = event->velocity_y;
    }
}

void testPanCarriesVelocity() {
    const esx_view root = esx_create_view(0, 0, 400, 400, 0);
    esx_set_root_view(root);
    PanCapture capture;
    esx_view_set_pan_callback(root, capturePan, &capture);

    using evk::ui::PointerAction;
    // 快速上滑：60ms 内 y 从 380 → 260，vy 应约 -2000 px/s。
    evk::ui::dispatchPointerEvent({PointerAction::Down, 0, 200, 380, ms(1000)});
    evk::ui::dispatchPointerEvent({PointerAction::Move, 0, 200, 340, ms(1020)});
    evk::ui::dispatchPointerEvent({PointerAction::Move, 0, 200, 300, ms(1040)});
    evk::ui::dispatchPointerEvent({PointerAction::Move, 0, 200, 260, ms(1060)});
    evk::ui::dispatchPointerEvent({PointerAction::Up, 0, 200, 260, ms(1070)});
    assert(capture.ends == 1);
    assert(capture.endVelocityY < -1500.0f && capture.endVelocityY > -2500.0f);
    assert(capture.endVelocityX == 0.0f);

    // 滑完停住 100ms 以上再抬起：速度为 0。
    evk::ui::dispatchPointerEvent({PointerAction::Down, 0, 200, 380, ms(2000)});
    evk::ui::dispatchPointerEvent({PointerAction::Move, 0, 200, 340, ms(2020)});
    evk::ui::dispatchPointerEvent({PointerAction::Up, 0, 200, 340, ms(2200)});
    assert(capture.ends == 2);
    assert(capture.endVelocityY == 0.0f);

    esx_destroy_view(root);
}

// ---- ScrollView 惯性/回弹 ----

void testScrollViewFlingAndClamp() {
    evk::setFrameFunc(renderFrame);
    evk::ui::stopAllAnimations();
    const esx_view root = esx_create_view(0, 0, 400, 400, 0);
    esx_set_root_view(root);
    const esx_view scroll = esx_scroll_view_create(0, 0, 200, 200, 200, 600, root);
    // 可滚动范围：600-200=400。

    using evk::ui::PointerAction;
    // 快速上滑甩出惯性（vy≈-1500 → 内容 offset 正向 fling）。
    evk::ui::dispatchPointerEvent({PointerAction::Down, 0, 100, 180, ms(1000)});
    evk::ui::dispatchPointerEvent({PointerAction::Move, 0, 100, 150, ms(1020)});
    evk::ui::dispatchPointerEvent({PointerAction::Move, 0, 100, 120, ms(1040)});
    evk::ui::dispatchPointerEvent({PointerAction::Up, 0, 100, 120, ms(1050)});

    float offsetY = 0.0f;
    esx_scroll_view_get_offset(scroll, nullptr, &offsetY);
    const float offsetAtRelease = offsetY;
    assert(offsetAtRelease == 60.0f);

    // 惯性继续推动 offset 增长。
    int64_t t = ms(2000);
    for (int i = 0; i < 10; ++i) {
        t += ms(16);
        evk::beginFrame(t);
    }
    esx_scroll_view_get_offset(scroll, nullptr, &offsetY);
    assert(offsetY > offsetAtRelease);

    // 冲出边界后回弹，最终停在 clamp 边界 400。
    for (int i = 0; i < 200; ++i) {
        t += ms(16);
        evk::beginFrame(t);
    }
    esx_scroll_view_get_offset(scroll, nullptr, &offsetY);
    assert(offsetY == 400.0f);

    esx_destroy_view(root);
    evk::ui::stopAllAnimations();
}

void testScrollViewRubberBandAndSpringBack() {
    evk::setFrameFunc(renderFrame);
    evk::ui::stopAllAnimations();
    const esx_view root = esx_create_view(0, 0, 400, 400, 0);
    esx_set_root_view(root);
    const esx_view scroll = esx_scroll_view_create(0, 0, 200, 200, 200, 600, root);
    esx_scroll_view_set_offset(scroll, 0, 400); // 先拉到底部

    using evk::ui::PointerAction;
    // 底部继续缓慢上拖 100px：raw=500 越界，显示 400+100*0.45=445（橡皮筋）。
    evk::ui::dispatchPointerEvent({PointerAction::Down, 0, 100, 180, ms(1000)});
    evk::ui::dispatchPointerEvent({PointerAction::Move, 0, 100, 160, ms(1020)});
    evk::ui::dispatchPointerEvent({PointerAction::Move, 0, 100, 140, ms(1040)});
    evk::ui::dispatchPointerEvent({PointerAction::Move, 0, 100, 120, ms(1060)});
    evk::ui::dispatchPointerEvent({PointerAction::Move, 0, 100, 100, ms(1080)});
    evk::ui::dispatchPointerEvent({PointerAction::Move, 0, 100, 80, ms(1100)});

    float offsetY = 0.0f;
    esx_scroll_view_get_offset(scroll, nullptr, &offsetY);
    assert(offsetY > 444.0f && offsetY < 446.0f);

    // 停住后松手（速度 0）：回弹到 400。
    evk::ui::dispatchPointerEvent({PointerAction::Up, 0, 100, 80, ms(1300)});
    int64_t t = ms(2000);
    for (int i = 0; i < 40; ++i) {
        t += ms(16);
        evk::beginFrame(t);
    }
    esx_scroll_view_get_offset(scroll, nullptr, &offsetY);
    assert(offsetY == 400.0f);

    // 顶部下拉越界：raw=-100，显示 -45；松手回弹到 0。
    esx_scroll_view_set_offset(scroll, 0, 0);
    evk::ui::dispatchPointerEvent({PointerAction::Down, 0, 100, 20, ms(2000)});
    evk::ui::dispatchPointerEvent({PointerAction::Move, 0, 100, 120, ms(2120)});
    esx_scroll_view_get_offset(scroll, nullptr, &offsetY);
    assert(offsetY > -46.0f && offsetY < -44.0f);
    evk::ui::dispatchPointerEvent({PointerAction::Up, 0, 100, 120, ms(2300)});
    for (int i = 0; i < 40; ++i) {
        t += ms(16);
        evk::beginFrame(t);
    }
    esx_scroll_view_get_offset(scroll, nullptr, &offsetY);
    assert(offsetY == 0.0f);

    esx_destroy_view(root);
    evk::ui::stopAllAnimations();
}

// 垂直列表（maxX=0）竖滑带水平抖动：水平方向全程冻结，
// 竖直 fling 不被水平抖动误判成越界而封堵（真机回归）。
void testScrollViewVerticalOnlyIgnoresHorizontal() {
    evk::setFrameFunc(renderFrame);
    evk::ui::stopAllAnimations();
    const esx_view root = esx_create_view(0, 0, 400, 400, 0);
    esx_set_root_view(root);
    const esx_view scroll = esx_scroll_view_create(0, 0, 200, 200, 200, 600, root);
    esx_scroll_view_set_offset(scroll, 0, 200); // 中部

    using evk::ui::PointerAction;
    // 快速上滑，带明显水平抖动。
    evk::ui::dispatchPointerEvent({PointerAction::Down, 0, 100, 180, ms(1000)});
    evk::ui::dispatchPointerEvent({PointerAction::Move, 0, 80, 150, ms(1020)});
    evk::ui::dispatchPointerEvent({PointerAction::Move, 0, 115, 120, ms(1040)});
    evk::ui::dispatchPointerEvent({PointerAction::Up, 0, 115, 120, ms(1050)});

    float offsetX = -1.0f;
    float offsetY = 0.0f;
    esx_scroll_view_get_offset(scroll, &offsetX, &offsetY);
    assert(offsetX == 0.0f);
    const float offsetAtRelease = offsetY;
    assert(offsetAtRelease == 260.0f); // 200+60，水平分量被忽略

    // 竖直 fling 生效：offset 继续增长。
    int64_t t = ms(2000);
    for (int i = 0; i < 10; ++i) {
        t += ms(16);
        evk::beginFrame(t);
    }
    esx_scroll_view_get_offset(scroll, &offsetX, &offsetY);
    assert(offsetX == 0.0f);
    assert(offsetY > offsetAtRelease);

    // 底部越界竖拖带水平抖动：只有垂直橡皮筋，offsetX 恒 0。
    esx_scroll_view_set_offset(scroll, 0, 400);
    evk::ui::dispatchPointerEvent({PointerAction::Down, 0, 100, 180, ms(3000)});
    evk::ui::dispatchPointerEvent({PointerAction::Move, 0, 70, 140, ms(3020)});
    evk::ui::dispatchPointerEvent({PointerAction::Move, 0, 120, 100, ms(3040)});
    esx_scroll_view_get_offset(scroll, &offsetX, &offsetY);
    assert(offsetX == 0.0f);
    assert(offsetY > 435.0f && offsetY < 437.0f); // 400+80*0.45=436
    evk::ui::dispatchPointerEvent({PointerAction::Up, 0, 120, 100, ms(3200)});
    for (int i = 0; i < 40; ++i) {
        t += ms(16);
        evk::beginFrame(t);
    }
    esx_scroll_view_get_offset(scroll, &offsetX, &offsetY);
    assert(offsetX == 0.0f);
    assert(offsetY == 400.0f);

    esx_destroy_view(root);
    evk::ui::stopAllAnimations();
}

void testScrollViewFlingInterruptedByDown() {
    evk::setFrameFunc(renderFrame);
    evk::ui::stopAllAnimations();
    const esx_view root = esx_create_view(0, 0, 400, 400, 0);
    esx_set_root_view(root);
    const esx_view scroll = esx_scroll_view_create(0, 0, 200, 200, 200, 600, root);

    using evk::ui::PointerAction;
    // 甩出惯性。
    evk::ui::dispatchPointerEvent({PointerAction::Down, 0, 100, 180, ms(1000)});
    evk::ui::dispatchPointerEvent({PointerAction::Move, 0, 100, 150, ms(1020)});
    evk::ui::dispatchPointerEvent({PointerAction::Move, 0, 100, 120, ms(1040)});
    evk::ui::dispatchPointerEvent({PointerAction::Up, 0, 100, 120, ms(1050)});

    int64_t t = ms(2000);
    for (int i = 0; i < 5; ++i) {
        t += ms(16);
        evk::beginFrame(t);
    }

    // 惯性中按下：动画被打断，offset 冻结。
    evk::ui::dispatchPointerEvent({PointerAction::Down, 0, 100, 100, ms(3000)});
    float frozen = 0.0f;
    esx_scroll_view_get_offset(scroll, nullptr, &frozen);
    for (int i = 0; i < 10; ++i) {
        t += ms(16);
        evk::beginFrame(t);
    }
    float offsetY = 0.0f;
    esx_scroll_view_get_offset(scroll, nullptr, &offsetY);
    assert(offsetY > frozen - 0.5f && offsetY < frozen + 0.5f);

    // 继续拖动正常跟随。
    evk::ui::dispatchPointerEvent({PointerAction::Move, 0, 100, 80, ms(3020)});
    evk::ui::dispatchPointerEvent({PointerAction::Up, 0, 100, 80, ms(3200)});
    esx_scroll_view_get_offset(scroll, nullptr, &offsetY);
    assert(offsetY > frozen + 19.0f && offsetY < frozen + 21.0f);

    esx_destroy_view(root);
    evk::ui::stopAllAnimations();
}

// ---- Navigation ----

struct PopCapture {
    int calls = 0;
    esx_view popped = 0;
};

void capturePop(esx_view /*nav*/, esx_view page, void* userData) {
    auto* capture = static_cast<PopCapture*>(userData);
    ++capture->calls;
    capture->popped = page;
}

void testNavigationPushPop() {
    evk::setFrameFunc(renderFrame);
    evk::ui::stopAllAnimations();
    const esx_view nav = esx_navigation_create(0, 0, 400, 800, 0, 60, nullptr);
    esx_set_root_view(nav);
    PopCapture popCapture;
    esx_navigation_set_on_pop(nav, capturePop, &popCapture);
    assert(esx_navigation_depth(nav) == 0);

    const esx_view page1 = esx_create_view(0, 0, 0, 0, 0);
    esx_navigation_push(nav, page1, 0);
    assert(esx_navigation_depth(nav) == 1);
    assert(esx_navigation_top_page(nav) == page1);
    evk::ui::View* p1 = esxViewFromHandle(page1);
    assert(p1 && p1->rect.x == 0.0f && p1->rect.w == 400.0f && p1->rect.h == 740.0f);
    assert(p1->visible);

    // 最后一页不能 pop。
    esx_navigation_pop(nav, 0);
    assert(esx_navigation_depth(nav) == 1);

    // 动画 push：page2 从屏右滑入。
    const esx_view page2 = esx_create_view(0, 0, 0, 0, 0);
    esx_navigation_push(nav, page2, 1);
    assert(esx_navigation_depth(nav) == 2);
    evk::ui::View* p2 = esxViewFromHandle(page2);
    assert(p2 && p2->rect.x == 400.0f);

    // 转场进行中 push 被忽略。
    const esx_view pageX = esx_create_view(0, 0, 0, 0, 0);
    esx_navigation_push(nav, pageX, 0);
    assert(esx_navigation_depth(nav) == 2);
    esx_destroy_view(pageX);

    int64_t t = ms(1000);
    for (int i = 0; i < 30; ++i) {
        t += ms(16);
        evk::beginFrame(t);
    }
    assert(p2->rect.x == 0.0f);
    assert(!p1->visible);

    // 动画 pop：page2 滑出后销毁。
    esx_navigation_pop(nav, 1);
    for (int i = 0; i < 30; ++i) {
        t += ms(16);
        evk::beginFrame(t);
    }
    assert(popCapture.calls == 1);
    assert(popCapture.popped == page2);
    assert(esxViewFromHandle(page2) == nullptr);
    assert(esx_navigation_depth(nav) == 1);
    assert(p1->visible && p1->rect.x == 0.0f);

    esx_destroy_view(nav);
    evk::ui::stopAllAnimations();
}

void testNavigationEdgeSwipe() {
    evk::setFrameFunc(renderFrame);
    evk::ui::stopAllAnimations();
    const esx_view nav = esx_navigation_create(0, 0, 400, 800, 0, 60, nullptr);
    esx_set_root_view(nav);
    const esx_view page1 = esx_create_view(0, 0, 0, 0, 0);
    esx_navigation_push(nav, page1, 0);
    const esx_view page2 = esx_create_view(0, 0, 0, 0, 0);
    esx_navigation_push(nav, page2, 0);
    evk::ui::View* p1 = esxViewFromHandle(page1);
    evk::ui::View* p2 = esxViewFromHandle(page2);
    assert(!p1->visible && p2->rect.x == 0.0f);

    using evk::ui::PointerAction;
    int64_t t = ms(500);

    // 左缘短滑（进度 0.25 且停住）→ 取消返回，回弹归位。
    evk::ui::dispatchPointerEvent({PointerAction::Down, 0, 10, 300, ms(1000)});
    evk::ui::dispatchPointerEvent({PointerAction::Move, 0, 60, 300, ms(1020)});
    assert(p1->visible); // 下层页面已随手势露出
    evk::ui::dispatchPointerEvent({PointerAction::Move, 0, 110, 300, ms(1040)});
    assert(p2->rect.x == 100.0f);
    evk::ui::dispatchPointerEvent({PointerAction::Up, 0, 110, 300, ms(1200)});
    for (int i = 0; i < 30; ++i) {
        t += ms(16);
        evk::beginFrame(t);
    }
    assert(p2->rect.x == 0.0f);
    assert(!p1->visible);
    assert(esx_navigation_depth(nav) == 2);

    // 左缘长滑（进度 0.625，速度 0）→ 完成返回。
    evk::ui::dispatchPointerEvent({PointerAction::Down, 0, 10, 300, ms(2000)});
    evk::ui::dispatchPointerEvent({PointerAction::Move, 0, 60, 300, ms(2020)});
    evk::ui::dispatchPointerEvent({PointerAction::Move, 0, 260, 300, ms(2060)});
    evk::ui::dispatchPointerEvent({PointerAction::Up, 0, 260, 300, ms(2200)});
    for (int i = 0; i < 30; ++i) {
        t += ms(16);
        evk::beginFrame(t);
    }
    assert(esxViewFromHandle(page2) == nullptr);
    assert(esx_navigation_depth(nav) == 1);
    assert(p1->visible && p1->rect.x == 0.0f);

    // 热区外的滑动不触发返回。
    const esx_view page3 = esx_create_view(0, 0, 0, 0, 0);
    esx_navigation_push(nav, page3, 0);
    evk::ui::View* p3 = esxViewFromHandle(page3);
    evk::ui::dispatchPointerEvent({PointerAction::Down, 0, 200, 300, ms(3000)});
    evk::ui::dispatchPointerEvent({PointerAction::Move, 0, 380, 300, ms(3020)});
    evk::ui::dispatchPointerEvent({PointerAction::Up, 0, 380, 300, ms(3040)});
    assert(esx_navigation_depth(nav) == 2);
    assert(p3->rect.x == 0.0f);

    // 快速甩动（进度不足但速度 >400px/s）→ 完成返回。
    evk::ui::dispatchPointerEvent({PointerAction::Down, 0, 10, 300, ms(4000)});
    evk::ui::dispatchPointerEvent({PointerAction::Move, 0, 60, 300, ms(4020)});
    evk::ui::dispatchPointerEvent({PointerAction::Move, 0, 100, 300, ms(4040)});
    evk::ui::dispatchPointerEvent({PointerAction::Up, 0, 100, 300, ms(4050)});
    for (int i = 0; i < 30; ++i) {
        t += ms(16);
        evk::beginFrame(t);
    }
    assert(esxViewFromHandle(page3) == nullptr);
    assert(esx_navigation_depth(nav) == 1);

    esx_destroy_view(nav);
    evk::ui::stopAllAnimations();
}

} // namespace

int main() {
    testDirtyFramesAreCoalesced();
    testViewClickAndDragCancellation();
    testScrollViewTakesDragFromChildButton();
    testScrollViewReclampsOffsetAfterResize();
    testClickRespectsParentClip();
    testTreeMutationDuringDrawIsRejected();
    testInputLifecycleCleanup();
    testCancellationCallbacksMayDestroyViews();
    testAnimatorTicksAndFinishes();
    testPanCarriesVelocity();
    testScrollViewFlingAndClamp();
    testScrollViewRubberBandAndSpringBack();
    testScrollViewVerticalOnlyIgnoresHorizontal();
    testScrollViewFlingInterruptedByDown();
    testNavigationPushPop();
    testNavigationEdgeSwipe();
    return 0;
}
