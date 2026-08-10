#pragma once

// iOS 薄壳视图，对应 Android 的 VulkanSurfaceView：
// 只负责向系统拿绘制层（CAMetalLayer）、收触摸，然后原样转发给 bridge，
// 自身不含任何业务逻辑（"薄壳"的含义）。

#import <UIKit/UIKit.h>

@interface EVKRenderView : UIView
@end
