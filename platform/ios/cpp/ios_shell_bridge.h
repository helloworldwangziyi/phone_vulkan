#pragma once

// ============================================================================
// iOS 壳层 ↔ core 的 C 桥接口。对照 Android 侧 NativeBridge 的 JNI 方法表：
//
//   NativeBridge.nativeInit(Surface)        → evkIosInit(CAMetalLayer*)
//   NativeBridge.nativeResize(w, h)         → evkIosResize(w, h)
//   NativeBridge.nativeBeginFrame(nanos)    → evkIosBeginFrame(nanos)
//   NativeBridge.nativeOnTouch(...)         → evkIosTouch(...)
//   NativeBridge.nativeDestroy()            → evkIosDestroy()
//
// 约定与 JNI 层相同：本层只做"解包参数 → 转发 core"，不含业务逻辑；
// 所有函数都在 iOS 主线程（= UI 线程）调用。
// ============================================================================

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// action 取值与 evk::ui::PointerAction 一致：0=Down 1=Up 2=Move 3=Cancel。
// layer 参数为 CAMetalLayer*（不透明指针，避免纯 C 头文件暴露 ObjC 类型）。
// x/y 与宽高均为像素（壳层已乘 contentsScale）。
void evkIosInit(const void* layer);
void evkIosResize(int32_t width, int32_t height);
void evkIosBeginFrame(int64_t frameTimeNanos);
void evkIosTouch(int32_t action, int32_t pointerId, float x, float y,
                 int64_t eventTimeNanos);
void evkIosDestroy(void);

#ifdef __cplusplus
} // extern "C"
#endif
