#pragma once

/**
 * @file zy_frame_scheduler.h
 * @brief 按需渲染循环：平台壳注册帧绘制实现，VSync 回调驱动"脏才画"。
 */
#include <cstdint>
#include <functional>

namespace evk {

/**
 * @brief 平台壳注册的"画一帧"实现（例如 Android bridge 的 renderFrame）。
 * @param frameTimeNanos 本帧 VSync 时间戳（纳秒）
 */
using FrameFunc = std::function<void(int64_t frameTimeNanos)>;

/**
 * @brief 注册帧绘制实现，由平台壳在初始化完成后调用。
 * @param func 帧绘制回调
 */
void setFrameFunc(FrameFunc func);

/**
 * @brief 引擎就绪状态：平台壳在渲染器初始化完成、可安全建视图时置 true，
 * Surface 销毁时置 false。App 应在 EngineReady 事件后创建视图。
 * @param ready 就绪标志
 */
void setEngineReady(bool ready);

/**
 * @brief 读取引擎就绪状态。
 * @return true 表示渲染器已初始化完成、可安全创建视图
 */
bool engineReady();

/**
 * @brief 标记下一次 VSync 需要绘制；不会在调用栈内立即提交 GPU。
 */
void requestRender();

/**
 * @brief 平台 VSync 回调入口。仅 dirty 时调用 FrameFunc 并清除本次 dirty；
 * 绘制过程中再次 requestRender 会保留到下一帧。
 * @param frameTimeNanos 本帧 VSync 时间戳（纳秒）
 * @return true 表示本帧实际执行了帧绘制
 */
bool beginFrame(int64_t frameTimeNanos);

/**
 * @brief Surface 销毁后清除未消费的帧请求。
 */
void cancelPendingFrame();

} // namespace evk
