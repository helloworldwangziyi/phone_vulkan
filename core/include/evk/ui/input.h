#pragma once

#include <cstdint>

#include "evk/esx_view.h"

namespace evk::ui {

// 平台层必须先把系统动作翻译成这组跨平台动作，core 不依赖 Android 常量。
enum class PointerAction : int32_t {
    Down,
    Move,
    Up,
    Cancel,
};

struct PointerEvent {
    PointerAction action;
    int32_t pointerId;
    float x;
    float y;
    // 事件时间（纳秒，平台时钟基准不限，速度计算只用相对差值）。
    // 为 0 时由 core 以 steady_clock 兜底。
    int64_t timestampNanos = 0;
};

// UI 线程入口：命中测试、点击合成和滑动归属全部由 SDK 处理。
void dispatchPointerEvent(const PointerEvent& event);

// View 子树即将销毁时清除仍引用它的活动手势，不再回调正在销毁的对象。
void discardPointerForView(esx_view view);

// View 子树隐藏时取消它的活动手势，并发送 Cancel 回调恢复控件状态。
void cancelPointerForView(esx_view view);

// Surface/窗口失效时取消当前活动手势，恢复控件 pressed 状态。
void cancelAllPointerEvents();

} // namespace evk::ui
