// iOS sample 入口。Storyboard 不用，窗口与视图层级全代码搭建（见 ZyAppDelegate）。
#import <UIKit/UIKit.h>

#import "ZyAppDelegate.h"

int main(int argc, char* argv[]) {
    @autoreleasepool {
        return UIApplicationMain(argc, argv, nil,
                                 NSStringFromClass([ZyAppDelegate class]));
    }
}
