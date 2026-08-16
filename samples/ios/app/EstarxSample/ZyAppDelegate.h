#import <UIKit/UIKit.h>

// 薄壳 ZyAppDelegate：只做"建窗口 + 挂 ZyViewController"，不含业务。
@interface ZyAppDelegate : UIResponder <UIApplicationDelegate>
@property(strong, nonatomic) UIWindow* window;
@end
