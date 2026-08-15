#pragma once

/**
 * @file animator.h
 * @brief 逐帧动画驱动。动画在每次 beginFrame 时被 tick（先于 dirty 检查），
 * tick 内修改视图会 requestRender() 置 dirty，动画因此自持续逐帧推进，
 * 无需平台侧额外调度。
 *
 * 生命周期约定：tick 内必须用 esxViewFromHandle 重新解析视图句柄
 * （视图可能已被销毁，返回 nullptr 时应结束动画）；持有控件状态时
 * 只能存 std::weak_ptr，lock 失败同样结束动画。控件无需析构钩子。
 */
#include <cstdint>

namespace evk::ui {

/**
 * @brief 每帧回调。
 * @param frameTimeNanos 帧时间（纳秒）
 * @param userData startAnimation 注册的用户数据
 * @return true 表示动画结束并从列表移除，false 继续下一帧
 */
using AnimationTick = bool (*)(int64_t frameTimeNanos, void* userData);

/**
 * @brief 动画移除时调用（tick 返回 true 或 stopAllAnimations），释放 userData。
 * @param userData startAnimation 注册的用户数据
 */
using AnimationCleanup = void (*)(void* userData);

/**
 * @brief 注册动画并 requestRender()。
 * @param tick 每帧回调
 * @param userData 动画上下文
 * @param cleanup 结束时释放 userData 的回调，可为 nullptr（userData 无需释放）
 */
void startAnimation(AnimationTick tick, void* userData, AnimationCleanup cleanup);

/**
 * @brief 由 beginFrame 每帧调用；App/平台不直接使用。
 * @param frameTimeNanos 帧时间（纳秒）
 */
void tickAnimations(int64_t frameTimeNanos);

/**
 * @brief Surface 销毁时丢弃全部动画（对每个动画调用 cleanup）。
 */
void stopAllAnimations();

/**
 * @brief 三次缓出，供控件动画使用。
 * @param t 进度，∈ [0,1]
 * @return 缓动后的值（∈ [0,1]）
 */
float easeOutCubic(float t);

/**
 * @brief 三次缓入缓出，供控件动画使用。
 * @param t 进度，∈ [0,1]
 * @return 缓动后的值（∈ [0,1]）
 */
float easeInOutCubic(float t);

} // namespace evk::ui
