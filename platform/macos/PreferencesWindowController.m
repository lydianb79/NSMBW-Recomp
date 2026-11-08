// PreferencesWindowController.m - Graphics / Input / Audio / Game tabs, built in code.
// Every change is applied to rt_settings immediately and saved.
#import "PreferencesWindowController.h"
#import "AppDelegate.h"
#import "InputManager.h"
#include <string.h>
#include "rt.h"

static NSString *const kButtonLabels[RT_BTN_COUNT] = {
    @"D-pad Up", @"D-pad Down", @"D-pad Left", @"D-pad Right", @"A", @"B", @"1 (run / fire)", @"2 (jump)",
    @"+ (pause)", @"− (minus)", @"HOME", @"Shake (spin / pick up)", @"Tilt left", @"Tilt right",
    @"Nunchuk C", @"Nunchuk Z",
};

@interface PreferencesWindowController () <NSWindowDelegate>
@end

@implementation PreferencesWindowController {
    __weak AppDelegate *_owner;
    NSPopUpButton *_player, *_padSelect;
    NSMutableArray<NSButton *> *_keyButtons, *_padButtons;
    id _keyMonitor;
    NSTimer *_padTimer;
    int _captureButton;          /* RtButton being captured, -1 none */
    BOOL _capturePad;
}

- (instancetype)initWithOwner:(AppDelegate *)owner {
    NSWindow *w = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 620, 716)
                                              styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                                                backing:NSBackingStoreBuffered defer:NO];
    w.title = @"Settings";
    if ((self = [super initWithWindow:w])) {
        _owner = owner;
        _captureButton = -1;
        w.delegate = self;
        [self build];
        [w center];
    }
    return self;
}

- (void)changed {
    [_owner saveSettings];
    [_owner applySettings];
}

#pragma mark - helpers

static NSTextField *label(NSString *s, NSRect r) {
    NSTextField *t = [NSTextField labelWithString:s];
    t.frame = r;
    t.alignment = NSTextAlignmentRight;
    return t;
}

- (NSPopUpButton *)popup:(NSArray<NSString *> *)items at:(NSRect)r selected:(NSInteger)sel tag:(NSInteger)tag in:(NSView *)v {
    NSPopUpButton *p = [[NSPopUpButton alloc] initWithFrame:r pullsDown:NO];
    [p addItemsWithTitles:items];
    [p selectItemAtIndex:MAX(0, MIN(sel, (NSInteger)items.count - 1))];
    p.tag = tag;
    p.target = self;
    p.action = @selector(graphicsPopup:);
    [v addSubview:p];
    return p;
}

- (NSButton *)check:(NSString *)title at:(NSRect)r on:(BOOL)on tag:(NSInteger)tag in:(NSView *)v {
    NSButton *b = [NSButton checkboxWithTitle:title target:self action:@selector(graphicsCheck:)];
    b.frame = r;
    b.state = on ? NSControlStateValueOn : NSControlStateValueOff;
    b.tag = tag;
    [v addSubview:b];
    return b;
}

- (NSSlider *)slider:(double)val at:(NSRect)r tag:(NSInteger)tag in:(NSView *)v {
    NSSlider *s = [NSSlider sliderWithValue:val minValue:0 maxValue:1 target:self action:@selector(graphicsSlider:)];
    s.frame = r;
    s.tag = tag;
    s.continuous = YES;
    [v addSubview:s];
    return s;
}

#pragma mark - build

- (void)build {
    NSTabView *tabs = [[NSTabView alloc] initWithFrame:NSMakeRect(10, 10, 600, 620)];
    NSTabViewItem *g = [[NSTabViewItem alloc] initWithIdentifier:@"gfx"];
    g.label = @"Graphics";
    g.view = [self graphicsView];
    NSTabViewItem *i = [[NSTabViewItem alloc] initWithIdentifier:@"input"];
    i.label = @"Input";
    i.view = [self inputView];
    NSTabViewItem *a = [[NSTabViewItem alloc] initWithIdentifier:@"audio"];
    a.label = @"Audio";
    a.view = [self audioView];
    NSTabViewItem *p = [[NSTabViewItem alloc] initWithIdentifier:@"game"];
    p.label = @"Game";
    p.view = [self gameTabView];
    [tabs addTabViewItem:g];
    [tabs addTabViewItem:i];
    [tabs addTabViewItem:a];
    [tabs addTabViewItem:p];
    self.window.contentView = tabs;
}

enum { G_RENDERER = 30, G_ICON = 31, G_RES = 1, G_FILTER, G_SCALE, G_ASPECT, G_TEX, G_ANISO, G_MSAA, G_FPS,
       G_VSYNC = 20, G_WIDE, G_SHOWFPS, G_FULL, S_SCAN = 40, S_SHARP, S_VOLUME,
       A_MUTE = 50, A_REVERB, A_REMOTE, A_LATENCY, S_RVOLUME = 60 };

#pragma mark - audio

- (NSView *)audioView {
    NSView *v = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 580, 580)];
    CGFloat y = 530, L = 20, W = 190, X = 220, PW = 240, dy = 36;
    [v addSubview:label(@"Volume:", NSMakeRect(L, y, W, 20))];
    [self slider:rt_settings.audio_volume / 100.0 at:NSMakeRect(X, y, PW, 20) tag:S_VOLUME in:v]; y -= 30;
    [self check:@"Mute" at:NSMakeRect(X, y, PW, 20) on:rt_settings.audio_mute tag:A_MUTE in:v]; y -= dy + 6;
    [self check:@"Reverb and effects (as the game sets them)" at:NSMakeRect(X, y, PW + 120, 20)
             on:rt_settings.audio_reverb tag:A_REVERB in:v]; y -= dy;
    [self check:@"Wii Remote speaker sounds" at:NSMakeRect(X, y, PW + 60, 20) on:rt_settings.remote_speaker tag:A_REMOTE in:v]; y -= 30;
    [v addSubview:label(@"Remote speaker volume:", NSMakeRect(L, y, W, 20))];
    [self slider:rt_settings.remote_volume / 100.0 at:NSMakeRect(X, y, PW, 20) tag:S_RVOLUME in:v]; y -= dy + 6;
    [v addSubview:label(@"Latency:", NSMakeRect(L, y, W, 20))];
    static const int lat[] = { 30, 45, 60, 90, 120, 200 };
    NSInteger li = 2;
    for (int k = 0; k < 6; k++) if (rt_settings.audio_latency_ms >= lat[k]) li = k;
    NSPopUpButton *lp = [self popup:@[ @"30 ms (lowest)", @"45 ms", @"60 ms (default)", @"90 ms", @"120 ms", @"200 ms (safest)" ]
                                 at:NSMakeRect(X, y - 4, PW, 26) selected:li tag:A_LATENCY in:v];
    (void)lp; y -= dy + 10;
    NSTextField *note = [NSTextField wrappingLabelWithString:
        @"Output is 32 kHz stereo, mixed from the game's own voices. Wii Remote speaker sounds play through the "
         "computer, filtered to sound like the remote's small speaker. Raise the latency if you hear crackles."];
    note.frame = NSMakeRect(L, y - 30, 540, 50);
    note.textColor = NSColor.secondaryLabelColor;
    note.font = [NSFont systemFontOfSize:11];
    [v addSubview:note];
    return v;
}

- (NSView *)graphicsView {
    NSView *v = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 580, 688)];
    CGFloat y = 638, L = 20, W = 190, X = 220, PW = 240, dy = 36;
    [v addSubview:label(@"Renderer (needs restart):", NSMakeRect(L, y, W, 20))];
    [self popup:@[ @"Metal (GPU)", @"Software (CPU)" ] at:NSMakeRect(X, y - 4, PW, 26)
       selected:rt_settings.renderer == 1 ? 1 : 0 tag:G_RENDERER in:v]; y -= dy;
    [v addSubview:label(@"App icon:", NSMakeRect(L, y, W, 20))];
    [self popup:@[ @"Islands: Day", @"Islands: Sunset", @"Islands: Night", @"Islands: Winter", @"Islands: Autumn",
                       @"Arcade Chip", @"Pixel Leap", @"Neon", @"Paper Craft", @"Minimal Bounce" ] at:NSMakeRect(X, y - 4, PW, 26)
       selected:rt_settings.app_icon tag:G_ICON in:v]; y -= dy;
    NSArray *res = @[ @"1× native (640×528)", @"2× (720p)", @"3× (1080p)", @"4× (1440p)", @"5×", @"6× (4K)", @"7×", @"8× (5K)" ];
    [v addSubview:label(@"Internal resolution:", NSMakeRect(L, y, W, 20))];
    [self popup:res at:NSMakeRect(X, y - 4, PW, 26) selected:rt_settings.internal_res - 1 tag:G_RES in:v]; y -= dy;
    [v addSubview:label(@"Output filter:", NSMakeRect(L, y, W, 20))];
    [self popup:@[ @"Nearest neighbour", @"Bilinear", @"Sharp bilinear", @"Scale2x (EPX)", @"CRT (scanlines + mask)", @"LCD grid" ]
             at:NSMakeRect(X, y - 4, PW, 26) selected:rt_settings.output_filter tag:G_FILTER in:v]; y -= dy;
    [v addSubview:label(@"Scaling:", NSMakeRect(L, y, W, 20))];
    [self popup:@[ @"Fit to window (keep aspect)", @"Integer scaling", @"Stretch to window" ]
             at:NSMakeRect(X, y - 4, PW, 26) selected:rt_settings.scale_mode tag:G_SCALE in:v]; y -= dy;
    [v addSubview:label(@"Aspect ratio:", NSMakeRect(L, y, W, 20))];
    [self popup:@[ @"Auto (follow game)", @"Force 4:3", @"Force 16:9" ]
             at:NSMakeRect(X, y - 4, PW, 26) selected:rt_settings.aspect tag:G_ASPECT in:v]; y -= dy;
    [v addSubview:label(@"Texture filtering:", NSMakeRect(L, y, W, 20))];
    [self popup:@[ @"As the game sets it", @"Force nearest", @"Force linear" ]
             at:NSMakeRect(X, y - 4, PW, 26) selected:rt_settings.texture_filter tag:G_TEX in:v]; y -= dy;
    NSArray *an = @[ @"1×", @"2×", @"4×", @"8×", @"16×" ];
    NSInteger ai = rt_settings.anisotropy >= 16 ? 4 : rt_settings.anisotropy >= 8 ? 3 : rt_settings.anisotropy >= 4 ? 2 : rt_settings.anisotropy >= 2 ? 1 : 0;
    [v addSubview:label(@"Anisotropic filtering:", NSMakeRect(L, y, W, 20))];
    [self popup:an at:NSMakeRect(X, y - 4, PW, 26) selected:ai tag:G_ANISO in:v]; y -= dy;
    [v addSubview:label(@"Anti-aliasing:", NSMakeRect(L, y, W, 20))];
    [self popup:@[ @"Off", @"2× MSAA", @"4× MSAA" ] at:NSMakeRect(X, y - 4, PW, 26)
       selected:rt_settings.msaa >= 4 ? 2 : rt_settings.msaa >= 2 ? 1 : 0 tag:G_MSAA in:v]; y -= dy;
    [v addSubview:label(@"Frame rate limit:", NSMakeRect(L, y, W, 20))];
    NSInteger fi = rt_settings.fps_limit == 50 ? 1 : rt_settings.fps_limit == 0 ? 3 : rt_settings.fps_limit == 120 ? 2 : 0;
    [self popup:@[ @"60 Hz (native)", @"50 Hz (PAL)", @"120 Hz (2× speed)", @"Unlimited" ] at:NSMakeRect(X, y - 4, PW, 26)
       selected:fi tag:G_FPS in:v]; y -= dy + 4;
    [v addSubview:label(@"CRT scanline strength:", NSMakeRect(L, y, W, 20))];
    [self slider:rt_settings.crt_scanline at:NSMakeRect(X, y, PW, 20) tag:S_SCAN in:v]; y -= dy;
    [v addSubview:label(@"Sharpness:", NSMakeRect(L, y, W, 20))];
    [self slider:rt_settings.sharpness at:NSMakeRect(X, y, PW, 20) tag:S_SHARP in:v]; y -= dy;
    [self check:@"V-Sync" at:NSMakeRect(X, y, PW, 20) on:rt_settings.vsync tag:G_VSYNC in:v]; y -= 26;
    [self check:@"Widescreen 16:9 (needs restart)" at:NSMakeRect(X, y, PW + 60, 20) on:rt_settings.widescreen tag:G_WIDE in:v]; y -= 26;
    [self check:@"Show FPS in title bar" at:NSMakeRect(X, y, PW, 20) on:rt_settings.show_fps tag:G_SHOWFPS in:v]; y -= 26;
    [self check:@"Full screen" at:NSMakeRect(X, y, PW, 20) on:rt_settings.fullscreen tag:G_FULL in:v]; y -= 34;
    NSTextField *note = [NSTextField wrappingLabelWithString:
        @"Changes apply immediately, except the renderer and widescreen (next start). Internal resolution, "
         "anti-aliasing and anisotropic filtering need the Metal renderer; Scale2x, CRT and LCD use native pixels."];
    note.frame = NSMakeRect(L, y - 20, 540, 40);
    note.textColor = NSColor.secondaryLabelColor;
    note.font = [NSFont systemFontOfSize:11];
    [v addSubview:note];
    return v;
}

- (void)graphicsPopup:(NSPopUpButton *)p {
    NSInteger i = p.indexOfSelectedItem;
    switch (p.tag) {
        case G_RENDERER: rt_settings.renderer = (int)i; break;
        case G_ICON: rt_settings.app_icon = (int)i; break;
        case G_RES: rt_settings.internal_res = (int)i + 1; break;
        case G_FILTER: rt_settings.output_filter = (RtFilter)i; break;
        case G_SCALE: rt_settings.scale_mode = (RtScaleMode)i; break;
        case G_ASPECT: rt_settings.aspect = (RtAspect)i; break;
        case G_TEX: rt_settings.texture_filter = (RtTexFilter)i; break;
        case G_ANISO: rt_settings.anisotropy = 1 << i; break;
        case G_MSAA: rt_settings.msaa = i == 0 ? 1 : i == 1 ? 2 : 4; break;
        case G_FPS: rt_settings.fps_limit = i == 0 ? 60 : i == 1 ? 50 : i == 2 ? 120 : 0; break;
        case A_LATENCY: { static const int lat[] = { 30, 45, 60, 90, 120, 200 }; rt_settings.audio_latency_ms = lat[i < 0 ? 2 : i > 5 ? 5 : i]; break; }
    }
    [self changed];
}

- (void)graphicsCheck:(NSButton *)b {
    bool on = b.state == NSControlStateValueOn;
    switch (b.tag) {
        case G_VSYNC: rt_settings.vsync = on; break;
        case G_WIDE: rt_settings.widescreen = on; break;
        case G_SHOWFPS: rt_settings.show_fps = on; break;
        case G_FULL: rt_settings.fullscreen = on; break;
        case A_MUTE: rt_settings.audio_mute = on; break;
        case A_REVERB: rt_settings.audio_reverb = on; break;
        case A_REMOTE: rt_settings.remote_speaker = on; break;
    }
    [self changed];
}

- (void)graphicsSlider:(NSSlider *)s {
    if (s.tag == S_SCAN) rt_settings.crt_scanline = (float)s.doubleValue;
    if (s.tag == S_SHARP) rt_settings.sharpness = (float)s.doubleValue;
    if (s.tag == S_VOLUME) rt_settings.audio_volume = (int)(s.doubleValue * 100.0 + 0.5);
    if (s.tag == S_RVOLUME) rt_settings.remote_volume = (int)(s.doubleValue * 100.0 + 0.5);
    [self changed];
}

#pragma mark - input

- (NSView *)inputView {
    NSView *v = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 580, 580)];
    CGFloat top = 540;
    [v addSubview:label(@"Player:", NSMakeRect(10, top, 70, 20))];
    _player = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(85, top - 4, 110, 26) pullsDown:NO];
    [_player addItemsWithTitles:@[ @"Player 1", @"Player 2", @"Player 3", @"Player 4" ]];
    _player.target = self;
    _player.action = @selector(playerChanged:);
    [v addSubview:_player];
    [v addSubview:label(@"Controller:", NSMakeRect(205, top, 80, 20))];
    _padSelect = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(290, top - 4, 270, 26) pullsDown:NO];
    _padSelect.target = self;
    _padSelect.action = @selector(padChanged:);
    [v addSubview:_padSelect];

    NSTextField *hk = [NSTextField labelWithString:@"Keyboard"]; hk.frame = NSMakeRect(230, top - 34, 140, 18); hk.font = [NSFont boldSystemFontOfSize:12];
    NSTextField *hp = [NSTextField labelWithString:@"Gamepad"]; hp.frame = NSMakeRect(390, top - 34, 140, 18); hp.font = [NSFont boldSystemFontOfSize:12];
    [v addSubview:hk];
    [v addSubview:hp];

    _keyButtons = [NSMutableArray array];
    _padButtons = [NSMutableArray array];
    CGFloat y = top - 62;
    for (int b = 0; b < RT_BTN_COUNT; b++) {
        [v addSubview:label(kButtonLabels[b], NSMakeRect(10, y, 205, 20))];
        NSButton *kb = [NSButton buttonWithTitle:@"" target:self action:@selector(captureKey:)];
        kb.frame = NSMakeRect(225, y - 4, 150, 26);
        kb.tag = b;
        [v addSubview:kb];
        [_keyButtons addObject:kb];
        NSButton *pb = [NSButton buttonWithTitle:@"" target:self action:@selector(capturePad:)];
        pb.frame = NSMakeRect(385, y - 4, 150, 26);
        pb.tag = b;
        [v addSubview:pb];
        [_padButtons addObject:pb];
        y -= 27;
    }
    y -= 6;
    NSButton *dpad = [NSButton checkboxWithTitle:@"Left stick also drives the D-pad" target:self action:@selector(stickToggle:)];
    dpad.frame = NSMakeRect(20, y, 300, 20);
    dpad.state = rt_settings.stick_as_dpad ? NSControlStateValueOn : NSControlStateValueOff;
    [v addSubview:dpad];
    [v addSubview:label(@"Dead zone:", NSMakeRect(310, y, 80, 20))];
    NSSlider *dz = [NSSlider sliderWithValue:rt_settings.stick_deadzone minValue:0 maxValue:0.9 target:self action:@selector(deadzone:)];
    dz.frame = NSMakeRect(395, y, 140, 20);
    [v addSubview:dz];
    NSButton *rum = [NSButton checkboxWithTitle:@"Rumble (controllers with haptics)" target:self action:@selector(rumbleToggle:)];
    rum.frame = NSMakeRect(20, y - 32, 300, 20);
    rum.state = rt_settings.rumble ? NSControlStateValueOn : NSControlStateValueOff;
    [v addSubview:rum];
    NSButton *reset = [NSButton buttonWithTitle:@"Restore Defaults" target:self action:@selector(resetInput:)];
    reset.frame = NSMakeRect(395, y - 36, 140, 28);
    [v addSubview:reset];
    [self refreshInput];
    return v;
}

- (int)currentPlayer { return (int)_player.indexOfSelectedItem; }

- (void)refreshInput {
    int p = [self currentPlayer];
    for (int b = 0; b < RT_BTN_COUNT; b++) {
        _keyButtons[b].title = (_captureButton == b && !_capturePad) ? @"Press a key…" : [InputManager nameForKey:rt_settings.key[p][b]];
        _padButtons[b].title = (_captureButton == b && _capturePad) ? @"Press a button…" : [InputManager nameForPadElement:rt_settings.pad[p][b]];
    }
    [_padSelect removeAllItems];
    [_padSelect addItemWithTitle:@"None (keyboard only)"];
    NSArray<GCController *> *pads = [[InputManager shared] controllers];
    for (GCController *c in pads) [_padSelect addItemWithTitle:c.vendorName ?: @"Controller"];
    int pi = rt_settings.pad_index[p];
    [_padSelect selectItemAtIndex:(pi >= 0 && pi < (int)pads.count) ? pi + 1 : 0];
}

- (void)playerChanged:(id)s { [self cancelCapture]; [self refreshInput]; }
- (void)padChanged:(NSPopUpButton *)s {
    rt_settings.pad_index[[self currentPlayer]] = (int)s.indexOfSelectedItem - 1;
    [self changed];
}
- (void)rumbleToggle:(NSButton *)b { rt_settings.rumble = b.state == NSControlStateValueOn; [self changed]; }
- (void)stickToggle:(NSButton *)b { rt_settings.stick_as_dpad = b.state == NSControlStateValueOn; [self changed]; }
- (void)deadzone:(NSSlider *)s { rt_settings.stick_deadzone = (float)s.doubleValue; [self changed]; }

- (void)resetInput:(id)s {
    RtSettings d;
    rt_settings_defaults(&d);
    memcpy(rt_settings.key, d.key, sizeof d.key);
    memcpy(rt_settings.pad, d.pad, sizeof d.pad);
    [self changed];
    [self refreshInput];
}

- (void)cancelCapture {
    if (_keyMonitor) { [NSEvent removeMonitor:_keyMonitor]; _keyMonitor = nil; }
    [_padTimer invalidate];
    _padTimer = nil;
    _captureButton = -1;
}

- (void)captureKey:(NSButton *)b {
    [self cancelCapture];
    _captureButton = (int)b.tag;
    _capturePad = NO;
    [self refreshInput];
    __weak typeof(self) weak = self;
    _keyMonitor = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown | NSEventMaskFlagsChanged
                                                        handler:^NSEvent *(NSEvent *e) {
        typeof(self) me = weak;
        if (!me || me->_captureButton < 0) return e;
        int p = [me currentPlayer];
        if (e.type == NSEventTypeKeyDown && e.keyCode == 0x33) rt_settings.key[p][me->_captureButton] = -1;   /* Delete clears */
        else rt_settings.key[p][me->_captureButton] = e.keyCode;
        [me cancelCapture];
        [me changed];
        [me refreshInput];
        return nil;
    }];
}

- (void)capturePad:(NSButton *)b {
    [self cancelCapture];
    int p = [self currentPlayer];
    NSArray<GCController *> *pads = [[InputManager shared] controllers];
    int pi = rt_settings.pad_index[p];
    if (pi < 0 || pi >= (int)pads.count) { NSBeep(); return; }
    GCController *c = pads[pi];
    _captureButton = (int)b.tag;
    _capturePad = YES;
    [self refreshInput];
    __block int ticks = 0;
    __weak typeof(self) weak = self;
    /* wait until all buttons are released, then take the first one pressed */
    __block BOOL armed = [[InputManager shared] firstPressedElementOn:c] == PadNone;
    _padTimer = [NSTimer scheduledTimerWithTimeInterval:1.0 / 60 repeats:YES block:^(NSTimer *t) {
        typeof(self) me = weak;
        if (!me) { [t invalidate]; return; }
        PadElement el = [[InputManager shared] firstPressedElementOn:c];
        if (!armed) { armed = el == PadNone; return; }
        if (el != PadNone) {
            rt_settings.pad[p][me->_captureButton] = el;
            [me cancelCapture];
            [me changed];
            [me refreshInput];
        } else if (++ticks > 60 * 6) {
            [me cancelCapture];
            [me refreshInput];
        }
    }];
}

#pragma mark - game

- (NSView *)gameTabView {
    NSView *v = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 580, 580)];
    [v addSubview:label(@"Game folder:", NSMakeRect(10, 530, 100, 20))];
    NSTextField *path = [NSTextField labelWithString:[NSString stringWithUTF8String:rt_settings.game_dir]];
    path.frame = NSMakeRect(120, 530, 340, 20);
    path.lineBreakMode = NSLineBreakByTruncatingMiddle;
    path.selectable = YES;
    path.tag = 99;
    [v addSubview:path];
    NSButton *choose = [NSButton buttonWithTitle:@"Choose…" target:self action:@selector(chooseFolder:)];
    choose.frame = NSMakeRect(465, 524, 100, 28);
    [v addSubview:choose];
    NSTextField *info = [NSTextField wrappingLabelWithString:
        @"The folder must be the output of `python3 tools/rvz.py extract <your disc>.rvz <folder>` "
         "(it contains sys/ and files/). A new folder is used the next time the app starts.\n\n"
         "Saves and the virtual Wii system settings (SYSCONF) live in "
         "~/Library/Application Support/NSMBWRecomp/nand."];
    info.frame = NSMakeRect(20, 400, 540, 110);
    info.textColor = NSColor.secondaryLabelColor;
    [v addSubview:info];
    return v;
}

- (void)chooseFolder:(NSButton *)b {
    NSOpenPanel *p = [NSOpenPanel openPanel];
    p.canChooseDirectories = YES;
    p.canChooseFiles = NO;
    if ([p runModal] != NSModalResponseOK) return;
    snprintf(rt_settings.game_dir, sizeof rt_settings.game_dir, "%s", p.URL.path.fileSystemRepresentation);
    NSTextField *path = [b.superview viewWithTag:99];
    path.stringValue = p.URL.path;
    [self changed];
}

- (void)windowWillClose:(NSNotification *)n { [self cancelCapture]; }

@end
