#import <MetalKit/MetalKit.h>

@interface Renderer : NSObject <MTKViewDelegate>
- (instancetype)initWithView:(MTKView *)view;
- (void)settingsChanged;
@end
