#import "EVKRenderView.h"

#import "cpp/bridge.h"

@implementation EVKRenderView

// UIView 的绘制层换成 CAMetalLayer：Metal 渲染的目标 surface。
+ (Class)layerClass {
    return [CAMetalLayer class];
}

// 构造函数①：代码里 new 时走这个（本项目 AppDelegate 走的就是它）。
- (instancetype)initWithFrame:(CGRect)frame {
    self = [super initWithFrame:frame];
    if (self) {
        [self commonInit];
    }
    return self;
}

// 构造函数②：Storyboard/XIB 反序列化创建时走这个。两个构造都必须提供。
- (instancetype)initWithCoder:(NSCoder*)coder {
    self = [super initWithCoder:coder];
    if (self) {
        [self commonInit];
    }
    return self;
}

- (void)commonInit {
    // contentsScale 对齐屏幕像素密度，后面所有坐标换算（点 → 像素）都靠它。
    self.layer.contentsScale = UIScreen.mainScreen.nativeScale;
    self.opaque = YES;
}

// 挂上/摘除 window 时回调，对应 Android 的 surfaceCreated / surfaceDestroyed。
- (void)didMoveToWindow {
    [super didMoveToWindow];
    if (self.window) {
        evkIOSInit((CAMetalLayer*)self.layer); // 幂等，重复挂载不会重建
        [self syncSurfaceAndRender];
    } else {
        evkIOSDestroy();
    }
}

// 布局确定或变化时回调（首次布局、旋转屏幕等），对应 surfaceChanged。
- (void)layoutSubviews {
    [super layoutSubviews];
    [self syncSurfaceAndRender];
}

// 同步 drawable 尺寸（像素 = 点 * contentsScale），然后按新尺寸画一帧。
- (void)syncSurfaceAndRender {
    if (!self.window) {
        return;
    }
    CAMetalLayer* metalLayer = (CAMetalLayer*)self.layer;
    const CGFloat scale = UIScreen.mainScreen.nativeScale;
    metalLayer.contentsScale = scale;
    const CGSize size = self.bounds.size;
    metalLayer.drawableSize = CGSizeMake(size.width * scale, size.height * scale);
    evkIOSResize((int32_t)metalLayer.drawableSize.width,
                 (int32_t)metalLayer.drawableSize.height);
    evkIOSRender();
}

// 触摸转发：action 对齐 Android MotionEvent 常量（0=按下 1=抬起 2=移动 3=取消），
// 坐标转成像素（UIKit 给的是点坐标，core 的视图树用像素）。
- (void)touchesBegan:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    [self forwardTouches:touches action:0];
}

- (void)touchesMoved:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    [self forwardTouches:touches action:2];
}

- (void)touchesEnded:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    [self forwardTouches:touches action:1];
}

- (void)touchesCancelled:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    [self forwardTouches:touches action:3];
}

- (void)forwardTouches:(NSSet<UITouch*>*)touches action:(int32_t)action {
    UITouch* touch = touches.anyObject;
    if (!touch) {
        return;
    }
    const CGPoint point = [touch locationInView:self];
    const CGFloat scale = self.layer.contentsScale;
    evkIOSTouch(action, point.x * scale, point.y * scale);
}

@end
