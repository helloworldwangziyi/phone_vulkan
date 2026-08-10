// Sample App 壳：只建一个全屏 EVKRenderView。
// 演示逻辑（视图树、事件响应）与 Android 端共用同一份
// samples/android/app/src/main/cpp/app_main.cpp：
// 它在二进制加载时经静态初始化注册 evk::setEventFunc，随后由
// bridge 的 AppStart / SurfaceChanged / Draw / UiClick 事件驱动。

#import "AppDelegate.h"

#import "EVKRenderView.h"

@implementation AppDelegate

- (BOOL)application:(UIApplication*)application
    didFinishLaunchingWithOptions:(NSDictionary*)launchOptions {
    self.window = [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];

    EVKRenderView* view = [[EVKRenderView alloc] initWithFrame:self.window.bounds];
    // 不用 Auto Layout，旋转/分屏时靠 autoresizing 跟随 window 尺寸。
    view.autoresizingMask =
        UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;

    UIViewController* controller = [[UIViewController alloc] init];
    controller.view = view;

    self.window.rootViewController = controller;
    [self.window makeKeyAndVisible];
    return YES;
}

@end
