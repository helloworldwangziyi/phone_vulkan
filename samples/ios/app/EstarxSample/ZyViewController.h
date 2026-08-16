#import <UIKit/UIKit.h>

// 薄壳 ZyViewController：把 UIKit 的视图生命周期/触摸/VSync 翻译成
// zy_ios_shell_bridge.h 的 C 调用。对照 Android 侧 ZyVulkanSurfaceView：
//   surfaceCreated/surfaceChanged/surfaceDestroyed
//     ≈ viewDidAppear / layoutSubviews / viewDidDisappear + 前后台通知
//   onTouchEvent ≈ touchesBegan/Moved/Ended/Cancelled
//   Choreographer.doFrame ≈ CADisplayLink
@interface ZyViewController : UIViewController
@end
