#pragma once

namespace evk {

// 平台壳注册的"画一帧"实现（例如 Android bridge 的 renderFrame）。
using FrameFunc = void (*)();

// 注册帧绘制实现，由平台壳在初始化完成后调用。
void setFrameFunc(FrameFunc func);

// App 请求重绘。当前是按需模型：直接同步调用已注册的 frame func；
// 将来换成 VSync + dirty 模型时只需改这里的实现，App 侧不用变。
// 未注册 frame func 时静默忽略。
void requestRender();

} // namespace evk
