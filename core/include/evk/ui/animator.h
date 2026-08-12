#pragma once

#include <cstdint>

namespace evk::ui {

// 逐帧动画驱动。动画在每次 beginFrame 时被 tick（先于 dirty 检查），
// tick 内修改视图会 requestRender() 置 dirty，动画因此自持续逐帧推进，
// 无需平台侧额外调度。
//
// 生命周期约定：tick 内必须用 esxViewFromHandle 重新解析视图句柄
// （视图可能已被销毁，返回 nullptr 时应结束动画）；持有控件状态时
// 只能存 std::weak_ptr，lock 失败同样结束动画。控件无需析构钩子。

// 每帧回调；返回 true 表示动画结束并从列表移除，false 继续下一帧。
using AnimationTick = bool (*)(int64_t frameTimeNanos, void* userData);

// 动画移除时调用（tick 返回 true 或 stopAllAnimations），释放 userData。
using AnimationCleanup = void (*)(void* userData);

// 注册动画并 requestRender()。cleanup 可为 nullptr（userData 无需释放）。
void startAnimation(AnimationTick tick, void* userData, AnimationCleanup cleanup);

// 由 beginFrame 每帧调用；App/平台不直接使用。
void tickAnimations(int64_t frameTimeNanos);

// Surface 销毁时丢弃全部动画（对每个动画调用 cleanup）。
void stopAllAnimations();

// 供控件动画使用的缓动函数，t ∈ [0,1]。
float easeOutCubic(float t);
float easeInOutCubic(float t);

} // namespace evk::ui
