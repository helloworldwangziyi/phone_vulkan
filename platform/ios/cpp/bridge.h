#pragma once

// iOS 薄壳（EVKRenderView）与 core 之间的 C 转发层，对应 Android 的
// platform/android/cpp/bridge.cpp：只做"解包参数 → 转发 core"，无业务逻辑。

#import <QuartzCore/CAMetalLayer.h>

#ifdef __cplusplus
extern "C" {
#endif

// 渲染 surface 就绪（视图挂上 window）时调用。幂等：已初始化直接返回。
void evkIOSInit(CAMetalLayer* layer);

// 视图尺寸确定或变化时调用，w/h 为像素（drawableSize）。
void evkIOSResize(int32_t width, int32_t height);

// 画一帧。
void evkIOSRender(void);

// 触摸事件。action 对齐 Android MotionEvent 常量：
// 0=按下 1=抬起 2=移动 3=取消；x/y 为像素坐标。
void evkIOSTouch(int32_t action, float x, float y);

// 视图从 window 摘除时调用，释放渲染资源。
void evkIOSDestroy(void);

#ifdef __cplusplus
}
#endif
