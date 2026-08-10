#pragma once

// iOS Metal 渲染器：与 core 的 evk::Renderer（Vulkan）同级，渲染同一份
// evk::ui::Canvas。Metal 是 Apple 平台 API，按"平台相关代码收敛在薄壳层"
// 的约定放在 platform/ios/，不进 core/。

#import <QuartzCore/CAMetalLayer.h>

#include <cstdint>

namespace evk::ui {
class Canvas;
}

@interface EVKMetalRenderer : NSObject

// 绑定一块 CAMetalLayer，创建 MTLDevice / 管线 / 命令队列。
// 任一环节失败返回 nil。
- (instancetype)initWithLayer:(CAMetalLayer*)layer;

// 画一帧：把 Canvas 的顶点/裁剪批次提交给 GPU 并 present。
- (BOOL)render:(const evk::ui::Canvas&)canvas;

@end
