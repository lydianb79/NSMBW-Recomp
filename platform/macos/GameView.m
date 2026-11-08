// GameView.m - Metal-backed view: forwards keyboard to InputManager and
// drives the Renderer once per display refresh.
#import "GameView.h"
#import <QuartzCore/CAMetalLayer.h>
#import <CoreVideo/CoreVideo.h>
#include <mach/mach_time.h>
#import "InputManager.h"
#import "Renderer.h"
#import "MetalGX.h"
#include "rt.h"

/* Display refresh -> runtime, so the game's 60 Hz clock locks to the screen
 * (runs on CoreVideo's display-link thread). */
static CVReturn display_tick(CVDisplayLinkRef link, const CVTimeStamp *now, const CVTimeStamp *out,
                             CVOptionFlags flags, CVOptionFlags *flags_out, void *ctx) {
    (void)now; (void)flags; (void)flags_out; (void)ctx;
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    double period = CVDisplayLinkGetActualOutputVideoRefreshPeriod(link);
    if (period <= 0 && out->videoTimeScale) period = (double)out->videoRefreshPeriod / out->videoTimeScale;
    if (period <= 0) return kCVReturnSuccess;
    int64_t delta_ticks = (int64_t)(out->hostTime - mach_absolute_time());
    int64_t delta_ns = delta_ticks * (int64_t)tb.numer / (int64_t)tb.denom;
    rt_vi_display_sync(delta_ns, (uint64_t)(period * 1e9));
    return kCVReturnSuccess;
}

@implementation GameView {
    CVDisplayLinkRef _link;
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
        if (CVDisplayLinkCreateWithActiveCGDisplays(&_link) == kCVReturnSuccess) {
            CVDisplayLinkSetOutputCallback(_link, display_tick, NULL);
            CVDisplayLinkStart(_link);
        }
    }
    return self;
}

- (void)settingsChanged {
    self.preferredFramesPerSecond = 120;     // present as soon as the game produces a frame
    ((CAMetalLayer *)self.layer).displaySyncEnabled = rt_settings.vsync;
    [_renderer settingsChanged];
}

- (void)dealloc {
    if (_link) { CVDisplayLinkStop(_link); CVDisplayLinkRelease(_link); }
}

/* follow the window to the screen it is on (refresh rates can differ) */
- (void)viewDidMoveToWindow {
    [super viewDidMoveToWindow];
    [[NSNotificationCenter defaultCenter] removeObserver:self name:NSWindowDidChangeScreenNotification object:nil];
    if (self.window) [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(screenChanged:)
                                                                  name:NSWindowDidChangeScreenNotification object:self.window];
    [self screenChanged:nil];
}
- (void)screenChanged:(NSNotification *)n {
    (void)n;
    NSNumber *sid = self.window.screen.deviceDescription[@"NSScreenNumber"];
    if (_link && sid) CVDisplayLinkSetCurrentCGDisplay(_link, (CGDirectDisplayID)sid.unsignedIntValue);
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
        NSString *title = @"NSMBW Recomp";
        if (rt_settings.show_fps) {
            /* with the Metal renderer: frames the game finished (not screen refreshes), and where the time goes */
            static uint64_t last_frames;
            double gpu = 0, gx = 0, wait = 0; uint64_t frames = 0;
            if (rt_metal_gx_stats(&gpu, &gx, &wait, &frames)) {
                if (last_frames) fps = (double)(frames - last_frames) / (now - _fpsStart);
                last_frames = frames;
                title = [NSString stringWithFormat:@"NSMBW Recomp — %.0f fps · GPU %.1f ms · GX %.1f ms · wait %.1f ms",
                                                   fps, gpu, gx, wait];
            } else {
                title = [NSString stringWithFormat:@"NSMBW Recomp — %.0f fps", fps];
            }
        }
        self.window.title = title;
        _fpsFrames = 0;
        _fpsStart = now;
    }
}

@end
