#pragma once

#include <cstdint>

namespace evk {

// 平台壳注册的"画一帧"实现（例如 Android bridge 的 renderFrame）。
using FrameFunc = void (*)(int64_t frameTimeNanos);

// 注册帧绘制实现，由平台壳在初始化完成后调用。
void setFrameFunc(FrameFunc func);

// 标记下一次 VSync 需要绘制；不会在调用栈内立即提交 GPU。
void requestRender();

// 平台 VSync 回调入口。仅 dirty 时调用 FrameFunc 并清除本次 dirty；
// 绘制过程中再次 requestRender 会保留到下一帧。
bool beginFrame(int64_t frameTimeNanos);

// Surface 销毁后清除未消费的帧请求。
void cancelPendingFrame();

} // namespace evk
