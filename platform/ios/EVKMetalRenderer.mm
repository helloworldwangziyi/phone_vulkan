// iOS Metal 渲染器实现，逐项对齐 core/src/renderer.cpp 的 Vulkan 版本：
// 同一 UiVertex 交错布局（float2 像素坐标 + float4 顶点色）、同一清屏色、
// 同一 SRC_ALPHA 混合、同一"每批次一个 scissor"的裁剪模型。

#import "EVKMetalRenderer.h"

#import <Metal/Metal.h>

#include <algorithm>

#include "evk/log.h"
#include "evk/ui/canvas.h"

// 与 Vulkan 版一致：NDC z 取 [0,1]（Metal 与 Vulkan 相同，区别于 GL 的 [-1,1]）。
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

// 内嵌 MSL 着色器，等价于 core/shaders/ui.vert + ui.frag。
// 运行时编译，Xcode 工程无需配置 .metal 构建规则。
static NSString* const kShaderSource = @R"(
#include <metal_stdlib>
using namespace metal;

struct VertexIn {
    float2 pos   [[attribute(0)]];
    float4 color [[attribute(1)]];
};

struct VertexOut {
    float4 position [[position]];
    float4 color;
};

struct Uniforms {
    float4x4 mvp;
};

vertex VertexOut ui_vert(VertexIn in [[stage_in]], constant Uniforms& u [[buffer(1)]]) {
    VertexOut out;
    out.position = u.mvp * float4(in.pos, 0.0, 1.0);
    out.color = in.color;
    return out;
}

fragment float4 ui_frag(VertexOut in [[stage_in]]) {
    return in.color;
}
)";

@implementation EVKMetalRenderer {
    CAMetalLayer* __strong _layer;
    id<MTLDevice> _device;
    id<MTLCommandQueue> _queue;
    id<MTLRenderPipelineState> _pipeline;
}

- (instancetype)initWithLayer:(CAMetalLayer*)layer {
    self = [super init];
    if (!self) {
        return nil;
    }

    _device = MTLCreateSystemDefaultDevice();
    if (!_device) {
        EVK_LOGE("MTLCreateSystemDefaultDevice failed");
        return nil;
    }
    _layer = layer;
    _layer.device = _device;
    _layer.pixelFormat = MTLPixelFormatBGRA8Unorm;

    NSError* error = nil;
    id<MTLLibrary> library = [_device newLibraryWithSource:kShaderSource options:nil error:&error];
    if (!library) {
        EVK_LOGE("MSL compile failed: {}", error.localizedDescription.UTF8String);
        return nil;
    }

    // 顶点布局与 Vulkan 管线一致：UiVertex = float2 像素坐标 + float4 顶点色。
    MTLVertexDescriptor* vertexDesc = [MTLVertexDescriptor vertexDescriptor];
    vertexDesc.attributes[0].format = MTLVertexFormatFloat2;
    vertexDesc.attributes[0].offset = 0;
    vertexDesc.attributes[0].bufferIndex = 0;
    vertexDesc.attributes[1].format = MTLVertexFormatFloat4;
    vertexDesc.attributes[1].offset = sizeof(float) * 2;
    vertexDesc.attributes[1].bufferIndex = 0;
    vertexDesc.layouts[0].stride = sizeof(evk::ui::UiVertex);
    vertexDesc.layouts[0].stepRate = 1;
    vertexDesc.layouts[0].stepFunction = MTLVertexStepFunctionPerVertex;

    MTLRenderPipelineDescriptor* desc = [[MTLRenderPipelineDescriptor alloc] init];
    desc.vertexFunction = [library newFunctionWithName:@"ui_vert"];
    desc.fragmentFunction = [library newFunctionWithName:@"ui_frag"];
    desc.vertexDescriptor = vertexDesc;
    desc.colorAttachments[0].pixelFormat = _layer.pixelFormat;
    // 与 Vulkan 版相同的普通 alpha 混合，UI 半透明背景需要。
    desc.colorAttachments[0].blendingEnabled = YES;
    desc.colorAttachments[0].rgbBlendOperation = MTLBlendOperationAdd;
    desc.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
    desc.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    desc.colorAttachments[0].alphaBlendOperation = MTLBlendOperationAdd;
    desc.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
    desc.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;

    _pipeline = [_device newRenderPipelineStateWithDescriptor:desc error:&error];
    if (!_pipeline) {
        EVK_LOGE("create pipeline failed: {}", error.localizedDescription.UTF8String);
        return nil;
    }

    _queue = [_device newCommandQueue];
    if (!_queue) {
        EVK_LOGE("newCommandQueue failed");
        return nil;
    }
    return self;
}

- (BOOL)render:(const evk::ui::Canvas&)canvas {
    // drawableSize 由视图在 layoutSubviews 里同步（像素 = 点 * nativeScale）。
    const CGFloat width = _layer.drawableSize.width;
    const CGFloat height = _layer.drawableSize.height;
    if (width <= 0 || height <= 0) {
        return YES; // 尚无有效尺寸（如退后台），直接跳过本帧
    }

    id<CAMetalDrawable> drawable = [_layer nextDrawable];
    if (!drawable) {
        EVK_LOGE("nextDrawable failed");
        return NO;
    }

    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = drawable.texture;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0.06, 0.06, 0.09, 1.0); // 同 Vulkan 清屏色

    id<MTLCommandBuffer> commandBuffer = [_queue commandBuffer];
    id<MTLRenderCommandEncoder> encoder =
        [commandBuffer renderCommandEncoderWithDescriptor:pass];
    [encoder setRenderPipelineState:_pipeline];

    MTLViewport viewport = {0.0, 0.0, width, height, 0.0, 1.0};
    [encoder setViewport:viewport];

    // 每帧一个顶点缓冲：容量小且按需渲染，直接随用随建，
    // 由 commandBuffer 持有到 GPU 执行完，省去多帧复用同步。
    if (!canvas.vertices().empty()) {
        id<MTLBuffer> vertexBuffer =
            [_device newBufferWithBytes:canvas.vertices().data()
                                 length:canvas.vertices().size() * sizeof(evk::ui::UiVertex)
                                options:MTLResourceStorageModeShared];
        [encoder setVertexBuffer:vertexBuffer offset:0 atIndex:0];
    }

    // 像素坐标 → NDC 的正交投影（原点在左上角，y 向下）。
    // 注意与 Vulkan 版的差异：Metal NDC +y 朝上（Vulkan 正 viewport 高度下朝下），
    // 所以这里传 bottom=height、top=0，把像素 y=h 映到 NDC -1（底）、y=0 映到 +1（顶）。
    glm::mat4 mvp = glm::ortho(0.0f, static_cast<float>(width),
                               static_cast<float>(height), 0.0f,
                               -1.0f, 1.0f);

    // 逐批绘制：每批共享一个 clip，裁剪矩形取 clip 与 drawable 的交集。
    for (const auto& batch : canvas.batches()) {
        if (batch.vertexCount == 0) {
            continue;
        }
        [encoder setVertexBytes:&mvp length:sizeof(mvp) atIndex:1];

        const float sw = static_cast<float>(width);
        const float sh = static_cast<float>(height);
        evk::ui::Rect clip = evk::ui::Rect::intersect(batch.clip, {0.0f, 0.0f, sw, sh});

        // clamp：左上角不小于 0，右下角不超出 drawable。
        const NSUInteger ox = std::max(0, static_cast<int>(clip.x));
        const NSUInteger oy = std::max(0, static_cast<int>(clip.y));
        const int ex = std::min(static_cast<int>(clip.x + clip.w), static_cast<int>(width))
                     - static_cast<int>(ox);
        const int ey = std::min(static_cast<int>(clip.y + clip.h), static_cast<int>(height))
                     - static_cast<int>(oy);
        if (ex <= 0 || ey <= 0) {
            continue;
        }

        MTLScissorRect scissor{ox, oy,
                               static_cast<NSUInteger>(ex), static_cast<NSUInteger>(ey)};
        [encoder setScissorRect:scissor];

        [encoder drawPrimitives:MTLPrimitiveTypeTriangle
                    vertexStart:batch.firstVertex
                    vertexCount:batch.vertexCount];
    }

    [encoder endEncoding];
    [commandBuffer presentDrawable:drawable];
    [commandBuffer commit];
    return YES;
}

@end
