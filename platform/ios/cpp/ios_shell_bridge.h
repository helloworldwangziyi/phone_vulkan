#pragma once

// ============================================================================
// iOS 壳层 ↔ core 的 C 桥接口。对照 Android 侧 NativeBridge 的 JNI 方法表：
//
//   NativeBridge.nativeInit(Surface)        → evkIosInit(CAMetalLayer*)
//   NativeBridge.nativeResize(w, h)         → evkIosResize(w, h)
//   NativeBridge.nativeBeginFrame(nanos)    → evkIosBeginFrame(nanos)
//   NativeBridge.nativeOnTouch(...)         → evkIosTouch(...)
//   NativeBridge.nativeOnBackPressed()      → evkIosBackPressed()
//   NativeBridge.nativeSafeAreaChanged(...) → evkIosSafeArea(...)
//   NativeBridge.nativeDestroy()            → evkIosDestroy()
//   NativeBridge.nativeDispatchPlatformCall → evkIosDispatchPlatformCall(...)
//   NativeBridge.onPlatformInvoke（出向）   → evkIosSetPlatformInvoker(...)
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
// 系统返回入口：iOS 无系统返回键，壳层用左边缘手势识别器触发。
// 返回是否被 App 消费（导航栈 pop 成功）；栈底时返回 0，壳层无需收尾。
int32_t evkIosBackPressed(void);
// 安全区内边距（像素，壳层已乘 contentsScale）：
// viewSafeAreaInsetsDidChange 时上报，App 据此内缩布局避开刘海/手势条。
void evkIosSafeArea(float top, float bottom, float left, float right);
void evkIosDestroy(void);

// ---- 平台通道（MethodChannel 式，按方法名路由；同步、UI 线程）----

// 平台→引擎入向：按方法名路由到 core 注册的 handler；未注册方法返回空串。
// 返回串由引擎侧 static 持有，仅本次调用期间有效——调用方须立即拷贝。
const char* evkIosDispatchPlatformCall(const char* method, const char* args);

// 引擎→平台出向的实现注入：壳层/App 在启动时注册（对照 Java 侧
// NativeBridge.setPlatformHandler）。实现返回的串同样只须在调用期间
// 有效（core 立即拷贝成 std::string）；传 NULL 注销。
typedef const char* (*EVKIosPlatformInvokeFn)(const char* method, const char* args);
void evkIosSetPlatformInvoker(EVKIosPlatformInvokeFn fn);

#ifdef __cplusplus
} // extern "C"
#endif
