#import "ZyAppDelegate.h"

#import "ZyViewController.h"

@implementation ZyAppDelegate

- (BOOL)application:(UIApplication*)application
    didFinishLaunchingWithOptions:(NSDictionary*)launchOptions {
    self.window = [[UIWindow alloc] initWithFrame:[UIScreen mainScreen].bounds];
    self.window.rootViewController = [[ZyViewController alloc] init];
    [self.window makeKeyAndVisible];
    return YES;
}

@end
