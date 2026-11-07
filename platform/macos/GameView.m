// GameView.m - Metal-backed view: forwards keyboard to InputManager and
// drives the Renderer once per display refresh.
#import "GameView.h"
#import <QuartzCore/CAMetalLayer.h>
#import "InputManager.h"
#import "Renderer.h"
#include "rt.h"

@implementation GameView {
    Renderer *_renderer;
    CFTimeInterval _fpsStart;
    uint64_t _fpsFrames, _lastGameFrame;
}

- (instancetype)initWithFrame:(NSRect)frame {
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    self = [super initWithFrame:frame device:dev];
    if (self) {
        self.colorPixelFormat = MTLPixelFormatBGRA8Unorm;
        self.clearColor = MTLClearColorMake(0, 0, 0, 1);
        self.framebufferOnly = YES;
        _renderer = [[Renderer alloc] initWithView:self];
        self.delegate = _renderer;
        [self settingsChanged];
    }
    return self;
}

- (void)settingsChanged {
    self.preferredFramesPerSecond = 120;     // present as soon as the game produces a frame
    ((CAMetalLayer *)self.layer).displaySyncEnabled = rt_settings.vsync;
    [_renderer settingsChanged];
}

- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)canBecomeKeyView { return YES; }

- (void)keyDown:(NSEvent *)e {
    if (e.modifierFlags & NSEventModifierFlagCommand) { [super keyDown:e]; return; }
    if (!e.isARepeat) [[InputManager shared] key:e.keyCode down:YES];
}
- (void)keyUp:(NSEvent *)e { [[InputManager shared] key:e.keyCode down:NO]; }

- (void)flagsChanged:(NSEvent *)e {
    NSEventModifierFlags f = e.modifierFlags;
    BOOL down = NO;
    switch (e.keyCode) {
        case 0x38: case 0x3C: down = (f & NSEventModifierFlagShift) != 0; break;
        case 0x3B: case 0x3E: down = (f & NSEventModifierFlagControl) != 0; break;
        case 0x3A: case 0x3D: down = (f & NSEventModifierFlagOption) != 0; break;
        case 0x39: down = (f & NSEventModifierFlagCapsLock) != 0; break;
        default: return;
    }
    [[InputManager shared] key:e.keyCode down:down];
}

- (void)viewDidChangeBackingProperties {
    [super viewDidChangeBackingProperties];
    self.layer.contentsScale = self.window.backingScaleFactor;
}

/* called by Renderer after each present */
- (void)didPresentGameFrame:(uint64_t)frame {
    if (frame != _lastGameFrame) { _fpsFrames++; _lastGameFrame = frame; }
    CFTimeInterval now = CACurrentMediaTime();
    if (_fpsStart == 0) _fpsStart = now;
    if (now - _fpsStart >= 1.0) {
        double fps = _fpsFrames / (now - _fpsStart);
        self.window.title = rt_settings.show_fps ? [NSString stringWithFormat:@"NSMBW Recomp — %.0f fps", fps] : @"NSMBW Recomp";
        _fpsFrames = 0;
        _fpsStart = now;
    }
}

@end
