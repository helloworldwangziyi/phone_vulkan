// iOS sample 入口。Storyboard 不用，窗口与视图层级全代码搭建（见 AppDelegate）。
#import <UIKit/UIKit.h>

#import "AppDelegate.h"

int main(int argc, char* argv[]) {
    @autoreleasepool {
        return UIApplicationMain(argc, argv, nil,
                                 NSStringFromClass([AppDelegate class]));
    }
}
