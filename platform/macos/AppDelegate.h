#import <Cocoa/Cocoa.h>

@class GameView;

@interface AppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
@property(nonatomic, strong) NSWindow *window;
@property(nonatomic, strong) GameView *gameView;
+ (NSString *)supportDirectory;
+ (NSString *)settingsPath;
- (void)saveSettings;
- (void)applySettings;
- (void)applyAppIcon;
@end
