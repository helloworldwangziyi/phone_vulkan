#import "ViewController.h"

#import <QuartzCore/CAMetalLayer.h>

#include "ios_shell_bridge.h"

// action 编码与 ios_shell_bridge.h 的约定一致（0=Down 1=Up 2=Move 3=Cancel）。
static const int32_t kActionDown = 0;
static const int32_t kActionUp = 1;
static const int32_t kActionMove = 2;
static const int32_t kActionCancel = 3;

// ----------------------------------------------------------------------------
// EVKMetalView：backing layer 换成 CAMetalLayer 的 UIView。
// 只负责一件事：布局时同步 drawableSize（像素 = 点 × scale）并上报 resize。
// ----------------------------------------------------------------------------
@interface EVKMetalView : UIView
@end

@implementation EVKMetalView

// UIView 的 backing layer 类型挂钩：系统建 layer 时改建成 CAMetalLayer。
+ (Class)layerClass {
    return [CAMetalLayer class];
}

- (void)layoutSubviews {
    [super layoutSubviews];
    // contentsScale 必须在主线程、布局阶段设置；Retina 上 scale=2/3。
    const CGFloat scale = self.window.screen.nativeScale;
    self.layer.contentsScale = scale;
    CAMetalLayer* layer = (CAMetalLayer*)self.layer;
    const CGSize points = self.bounds.size;
    layer.drawableSize = CGSizeMake(points.width * scale, points.height * scale);
    if (layer.drawableSize.width > 0 && layer.drawableSize.height > 0) {
        evkIosResize((int32_t)layer.drawableSize.width,
                     (int32_t)layer.drawableSize.height);
    }
}

@end

// ----------------------------------------------------------------------------
@interface ViewController ()
@property(nonatomic, strong) CADisplayLink* displayLink;
// 多点触控跟踪表：UITouch* → pointerId（数组下标），抬指后槽位置空
// （NSNull）复用——对照 Android MotionEvent 的 pointerId，一次手势期间
// id 稳定，core 按它分指跟踪。
@property(nonatomic, strong) NSMutableArray<id>* activeTouches;
@property(nonatomic, assign) BOOL engineAlive;
@end

@implementation ViewController

- (void)loadView {
    self.view = [[EVKMetalView alloc] initWithFrame:[UIScreen mainScreen].bounds];
    self.view.backgroundColor = UIColor.blackColor;
    // 多点触控：全量触点转发给 core（pinch/rotate/双指手势需要）；
    // 边缘返回由壳层手势识别器转成 BackPressed 事件，不占用触摸通道。
    self.view.multipleTouchEnabled = YES;
    self.activeTouches = [NSMutableArray array];
}

- (void)viewDidLoad {
    [super viewDidLoad];
    // iOS 没有系统返回键：壳层用左边缘手势识别器模拟"系统返回"，
    // 一次性上报 BackPressed 事件（不做跟手动画，与 Android 语义对齐）。
    UIScreenEdgePanGestureRecognizer* edgeBack =
        [[UIScreenEdgePanGestureRecognizer alloc]
            initWithTarget:self action:@selector(onEdgeBack:)];
    edgeBack.edges = UIRectEdgeLeft;
    [self.view addGestureRecognizer:edgeBack];
    // 前后台切换 ≈ Android 的 surfaceDestroyed/surfaceCreated：
    // iOS 后台不允许碰 GPU，必须暂停 VSync 并释放渲染资源。
    [NSNotificationCenter.defaultCenter
        addObserver:self
           selector:@selector(onEnterBackground)
               name:UIApplicationDidEnterBackgroundNotification
             object:nil];
    [NSNotificationCenter.defaultCenter
        addObserver:self
           selector:@selector(onEnterForeground)
               name:UIApplicationWillEnterForegroundNotification
             object:nil];
}

- (void)viewDidAppear:(BOOL)animated {
    [super viewDidAppear:animated];
    [self startEngine];
}

- (void)viewWillDisappear:(BOOL)animated {
    [self stopEngine];
    [super viewWillDisappear:animated];
}

- (void)dealloc {
    [NSNotificationCenter.defaultCenter removeObserver:self];
}

// ---- 引擎启停（对照 surfaceCreated/surfaceDestroyed，幂等） ----

- (void)startEngine {
    if (self.engineAlive || !self.view.window) {
        return;
    }
    self.engineAlive = YES;
    // layer 的 drawableSize 已在 layoutSubviews 里设好。
    evkIosInit((__bridge const void*)self.view.layer);
    self.displayLink = [CADisplayLink displayLinkWithTarget:self
                                                   selector:@selector(onVSync:)];
    [self.displayLink addToRunLoop:[NSRunLoop mainRunLoop]
                           forMode:NSRunLoopCommonModes];
}

- (void)stopEngine {
    self.engineAlive = NO;
    [self.displayLink invalidate];
    self.displayLink = nil;
    [self.activeTouches removeAllObjects];
    evkIosDestroy();
}

- (void)onEnterBackground {
    [self stopEngine];
}

- (void)onEnterForeground {
    [self startEngine];
}

// CADisplayLink.timestamp 是秒（与 UITouch.timestamp 同一时钟），
// 乘 1e9 转纳秒喂给 core（速度计算只依赖差值，单调即可）。
- (void)onVSync:(CADisplayLink*)link {
    evkIosBeginFrame((int64_t)(link.timestamp * 1e9));
}

// ---- 触摸转发（点 → 像素换算在壳层完成，core 只认像素） ----

// 左边缘手势一旦识别（开始拖动）就上报一次系统返回；
// 识别成功会取消在屏触摸（touchesCancelled），页面不会同时吃到这次拖动。
- (void)onEdgeBack:(UIScreenEdgePanGestureRecognizer*)recognizer {
    if (recognizer.state == UIGestureRecognizerStateBegan) {
        evkIosBackPressed();
    }
}

// 安全区（刘海/ Home 指示条）就绪或变化时系统回调（可能早于引擎就绪，
// core 事件通道会把值存住，建视图树时生效）。点 × scale 换算成像素上报。
- (void)viewSafeAreaInsetsDidChange {
    [super viewSafeAreaInsetsDidChange];
    const CGFloat scale = self.view.window.screen.nativeScale
        ?: UIScreen.mainScreen.nativeScale;
    const UIEdgeInsets insets = self.view.safeAreaInsets;
    evkIosSafeArea((float)(insets.top * scale), (float)(insets.bottom * scale),
                   (float)(insets.left * scale), (float)(insets.right * scale));
}

- (void)forwardTouch:(UITouch*)touch action:(int32_t)action pointerId:(int32_t)pointerId {
    const CGFloat scale = self.view.layer.contentsScale;
    const CGPoint p = [touch locationInView:self.view];
    evkIosTouch(action, pointerId, (float)(p.x * scale), (float)(p.y * scale),
                (int64_t)(touch.timestamp * 1e9));
}

// 已跟踪的 touch 返回其槽位 id，否则返回 -1。
- (int32_t)idForTouch:(UITouch*)touch {
    for (NSUInteger i = 0; i < self.activeTouches.count; ++i) {
        if (self.activeTouches[i] == touch) {
            return (int32_t)i;
        }
    }
    return -1;
}

// 新落屏手指分配槽位：优先复用 NSNull 空槽，否则追加。
- (int32_t)allocSlotForTouch:(UITouch*)touch {
    for (NSUInteger i = 0; i < self.activeTouches.count; ++i) {
        if (self.activeTouches[i] == (id)[NSNull null]) {
            self.activeTouches[i] = touch;
            return (int32_t)i;
        }
    }
    [self.activeTouches addObject:touch];
    return (int32_t)self.activeTouches.count - 1;
}

- (void)touchesBegan:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    for (UITouch* touch in touches) {
        const int32_t pointerId = [self allocSlotForTouch:touch];
        [self forwardTouch:touch action:kActionDown pointerId:pointerId];
    }
}

- (void)touchesMoved:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    for (UITouch* touch in touches) {
        const int32_t pointerId = [self idForTouch:touch];
        if (pointerId >= 0) {
            [self forwardTouch:touch action:kActionMove pointerId:pointerId];
        }
    }
}

- (void)touchesEnded:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    for (UITouch* touch in touches) {
        const int32_t pointerId = [self idForTouch:touch];
        if (pointerId >= 0) {
            [self forwardTouch:touch action:kActionUp pointerId:pointerId];
            self.activeTouches[(NSUInteger)pointerId] = [NSNull null];
        }
    }
}

- (void)touchesCancelled:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    for (UITouch* touch in touches) {
        const int32_t pointerId = [self idForTouch:touch];
        if (pointerId >= 0) {
            [self forwardTouch:touch action:kActionCancel pointerId:pointerId];
            self.activeTouches[(NSUInteger)pointerId] = [NSNull null];
        }
    }
}

@end
