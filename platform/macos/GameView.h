#import <MetalKit/MetalKit.h>

@interface GameView : MTKView
- (instancetype)initWithFrame:(NSRect)frame;
- (void)settingsChanged;
- (void)didPresentGameFrame:(uint64_t)frame;
@end
