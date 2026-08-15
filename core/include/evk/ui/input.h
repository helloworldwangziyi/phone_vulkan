#pragma once

/**
 * @file input.h
 * @brief 平台无关触摸事件结构与手势派发入口（单手势状态机）。
 */
#include <cstdint>

#include "evk/esx_view.h"

namespace evk::ui {

/**
 * @brief 平台层必须先把系统动作翻译成这组跨平台动作，core 不依赖 Android 常量。
 */
enum class PointerAction : int32_t {
    Down,   ///< 按下
    Move,   ///< 移动
    Up,     ///< 抬起
    Cancel, ///< 取消
};

/**
 * @brief 一条平台触摸事件（平台壳先翻译成此结构，core 不依赖系统常量）。
 *
 * x/y 为屏幕坐标；事件流经 dispatchPointerEvent 进入手势状态机。
 */
struct PointerEvent {
    PointerAction action; ///< Down/Move/Up/Cancel
    int32_t pointerId; ///< 手指 id；core 只跟踪一根，多指时第二根会取消第一根
    float x;
    float y;
    int64_t timestampNanos = 0; ///< 事件时间（纳秒，平台时钟基准不限，速度计算只用相对差值）；为 0 时由 core 以 steady_clock 兜底
};

/**
 * @brief UI 线程唯一的手势入口：把触摸流转成点击回调与 pan 事件。
 *
 * 机制：Down 时命中测试 + 锁定输入/滑动目标；Move 越过 12px 触控阈值
 * 即转滑动（点击取消，二者互斥）；Up 时合成点击或收尾 PAN_END（带速度）。
 * 实现见 input.cpp 的 dispatchPointerEvent。
 * @param event 平台壳翻译后的触摸事件
 */
void dispatchPointerEvent(const PointerEvent& event);

/**
 * @brief View 子树即将销毁时清除仍引用它的活动手势，不再回调正在销毁的对象。
 * @param view 即将销毁的子树根视图句柄
 */
void discardPointerForView(esx_view view);

/**
 * @brief View 子树隐藏时取消它的活动手势，并发送 Cancel 回调恢复控件状态。
 * @param view 被隐藏的子树根视图句柄
 */
void cancelPointerForView(esx_view view);

/**
 * @brief Surface/窗口失效时取消当前活动手势，恢复控件 pressed 状态。
 */
void cancelAllPointerEvents();

} // namespace evk::ui
