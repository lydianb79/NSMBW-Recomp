// AppDelegate.m - window, menus, settings persistence and the game thread.
#import "MetalGX.h"
#import "AudioOutput.h"
#import "AppDelegate.h"
#import "GameView.h"
#import "InputManager.h"
#import "PreferencesWindowController.h"
#include "rt.h"

#include "rt_gx.h"
#include <stdio.h>

/* Runtime log goes to Console and to recomp_log.txt next to the game folder. */
static FILE *g_logfile;
static void app_log(int level, const char *msg) {
    NSLog(@"[rt%d] %s", level, msg);
    if (g_logfile) { fprintf(g_logfile, "[rt%d] %s\n", level, msg); fflush(g_logfile); }
}
static void open_logfile(void) {
    if (g_logfile) return;
    /* next to the dist/ (or build/cmake/) folder that holds the app: the project folder */
    NSString *dir = [[[NSBundle mainBundle] bundlePath] stringByDeletingLastPathComponent];
    NSString *path = [[dir stringByAppendingPathComponent:@".."] stringByAppendingPathComponent:@"recomp_log.txt"];
    g_logfile = fopen(path.fileSystemRepresentation, "w");
    if (!g_logfile) g_logfile = fopen([[AppDelegate supportDirectory] stringByAppendingPathComponent:@"recomp_log.txt"].fileSystemRepresentation, "w");
    if (g_logfile) fprintf(g_logfile, "game_dir = %s\n", rt_settings.game_dir);
}

static void app_fatal(const char *msg) {
    NSString *text = [NSString stringWithUTF8String:msg];
    dispatch_sync(dispatch_get_main_queue(), ^{
        NSAlert *a = [[NSAlert alloc] init];
        a.messageText = @"The recompiled game stopped";
        a.informativeText = [text stringByAppendingString:
            @"\n\nThis usually means a hardware or SDK function still needs an HLE implementation. "
             "Details were written to the system log (Console.app, process \"NSMBW Recomp\")."];
        a.alertStyle = NSAlertStyleCritical;
        [a runModal];
    });
}

@implementation AppDelegate {
    PreferencesWindowController *_prefs;
    NSThread *_gameThread;
    int _slot;                     /* save-state slot 0..2 */
}

+ (NSString *)supportDirectory {
    NSString *base = NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory, NSUserDomainMask, YES).firstObject;
    NSString *dir = [base stringByAppendingPathComponent:@"NSMBWRecomp"];
    [[NSFileManager defaultManager] createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
    return dir;
}

+ (NSString *)settingsPath { return [[self supportDirectory] stringByAppendingPathComponent:@"settings.ini"]; }

- (void)saveSettings { rt_settings_save(&rt_settings, [AppDelegate settingsPath].fileSystemRepresentation); }

- (void)applySettings {
    [self.gameView settingsChanged];
    [self applyAppIcon];
    BOOL fs = (self.window.styleMask & NSWindowStyleMaskFullScreen) != 0;
    if (fs != rt_settings.fullscreen) [self.window toggleFullScreen:nil];
}

#pragma mark - launch

/* Dock icon from the selected variant (all variants are in the bundle's Resources). */
- (void)applyAppIcon {
    static NSString *const names[] = { @"day", @"sunset", @"night", @"winter", @"autumn",
                                       @"arcade_chip", @"pixel_leap", @"neon", @"paper", @"minimal" };
    int i = rt_settings.app_icon;
    if (i < 0 || i >= (int)(sizeof names / sizeof names[0])) i = 0;
    NSString *path = [[NSBundle mainBundle] pathForResource:[@"AppIcon-" stringByAppendingString:names[i]] ofType:@"icns"];
    NSImage *img = path ? [[NSImage alloc] initWithContentsOfFile:path] : nil;
    if (img) NSApp.applicationIconImage = img;
}

- (void)applicationDidFinishLaunching:(NSNotification *)note {
    rt_set_log(app_log);
    rt_set_fatal_hook(app_fatal);
    rt_settings_defaults(&rt_settings);
    rt_settings_load(&rt_settings, [AppDelegate settingsPath].fileSystemRepresentation);
    [self applyAppIcon];

    [self buildMenu];
    NSRect frame = NSMakeRect(0, 0, 1280, 720);
    self.window = [[NSWindow alloc] initWithContentRect:frame
                                              styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                        NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                                                backing:NSBackingStoreBuffered defer:NO];
    self.window.title = @"NSMBW Recomp";
    self.window.delegate = self;
    self.window.collectionBehavior = NSWindowCollectionBehaviorFullScreenPrimary;
    self.window.contentMinSize = NSMakeSize(320, 240);
    [self.window center];
    [self.window setFrameAutosaveName:@"MainWindow"];

    self.gameView = [[GameView alloc] initWithFrame:frame];
    self.window.contentView = self.gameView;
    [self.window makeKeyAndOrderFront:nil];
    [self.window makeFirstResponder:self.gameView];
    [NSApp activateIgnoringOtherApps:YES];
    if (rt_settings.fullscreen) [self.window toggleFullScreen:nil];

    [[InputManager shared] start];

    if (![self ensureGameDirectory]) return;
    [self startGame];
}

- (BOOL)gameDirectoryValid:(NSString *)dir {
    NSString *fst = [dir stringByAppendingPathComponent:@"sys/fst.bin"];
    NSString *rels = [dir stringByAppendingPathComponent:@"files/rels"];
    return [[NSFileManager defaultManager] fileExistsAtPath:fst] && [[NSFileManager defaultManager] fileExistsAtPath:rels];
}

- (BOOL)ensureGameDirectory {
    NSString *dir = [NSString stringWithUTF8String:rt_settings.game_dir];
    while (![self gameDirectoryValid:dir]) {
        NSAlert *a = [[NSAlert alloc] init];
        a.messageText = @"Choose your extracted game folder";
        a.informativeText = @"Select the folder produced by `tools/rvz.py extract` from your own New Super Mario Bros. Wii disc. "
                             "It must contain sys/ and files/.";
        [a addButtonWithTitle:@"Choose…"];
        [a addButtonWithTitle:@"Quit"];
        if ([a runModal] != NSAlertFirstButtonReturn) { [NSApp terminate:nil]; return NO; }
        NSOpenPanel *p = [NSOpenPanel openPanel];
        p.canChooseDirectories = YES;
        p.canChooseFiles = NO;
        p.prompt = @"Use Folder";
        if ([p runModal] == NSModalResponseOK) dir = p.URL.path;
    }
    snprintf(rt_settings.game_dir, sizeof rt_settings.game_dir, "%s", dir.fileSystemRepresentation);
    [self saveSettings];
    return YES;
}

- (void)startGame {
    NSBundle *b = [NSBundle mainBundle];
    NSString *img = [b pathForResource:@"image" ofType:@"bin"];
    NSString *js = [b pathForResource:@"image" ofType:@"json"];
    if (!img || !js) {
        NSAlert *a = [[NSAlert alloc] init];
        a.messageText = @"Recompiled image missing";
        a.informativeText = @"image.bin / image.json were not copied into the app bundle. Rebuild with scripts/build.sh.";
        [a runModal];
        return;
    }
    open_logfile();
    if (!rt_init(img.fileSystemRepresentation, js.fileSystemRepresentation)) {
        NSAlert *a = [[NSAlert alloc] init];
        a.messageText = @"Runtime failed to initialise";
        [a runModal];
        return;
    }
    open_logfile();
    if (rt_settings.renderer == 0 && !getenv("RT_SOFTWARE_RENDER")) rt_metal_gx_install();
    else rt_log(1, "renderer: software (Preferences > Graphics > Renderer)");
    if (!getenv("RT_NO_AUDIO")) rt_audio_output_start();
    rt_log(1, "app: starting game thread");
    /* once a second: progress line, so a black screen can be diagnosed */
    [NSTimer scheduledTimerWithTimeInterval:1.0 repeats:YES block:^(NSTimer *t) {
        RtFrame fr;
        bool have = rt_video_frame(&fr);
        rt_log(1, "status: vi %llu, gx %llu bytes / %llu draws / %llu copies, ipc+irq %llu, frame %s %ux%u #%llu",
               (unsigned long long)rt_stats.vi_frames, (unsigned long long)rt_stats.gx_fifo_bytes,
               (unsigned long long)rt_gx.draws, (unsigned long long)rt_gx.efb_copies,
               (unsigned long long)rt_stats.hle_hits, have ? "yes" : "no", have ? fr.width : 0, have ? fr.height : 0,
               have ? (unsigned long long)fr.frame : 0ull);
    }];
    _gameThread = [[NSThread alloc] initWithBlock:^{ rt_run(); }];
    _gameThread.stackSize = 64 << 20;          // recompiled code recurses deeply
    _gameThread.name = @"Broadway";
    _gameThread.qualityOfService = NSQualityOfServiceUserInteractive;
    [_gameThread start];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)app { return YES; }

- (void)applicationWillTerminate:(NSNotification *)note {
    rt_request_quit();
    [self saveSettings];
}

- (void)windowDidEnterFullScreen:(NSNotification *)n { rt_settings.fullscreen = true; [self saveSettings]; }
- (void)windowDidExitFullScreen:(NSNotification *)n { rt_settings.fullscreen = false; [self saveSettings]; }

#pragma mark - menu

- (void)buildMenu {
    NSMenu *bar = [[NSMenu alloc] init];
    NSMenuItem *appItem = [[NSMenuItem alloc] init];
    [bar addItem:appItem];
    NSMenu *appMenu = [[NSMenu alloc] init];
    [appMenu addItemWithTitle:@"About NSMBW Recomp" action:@selector(orderFrontStandardAboutPanel:) keyEquivalent:@""];
    [appMenu addItem:[NSMenuItem separatorItem]];
    [appMenu addItemWithTitle:@"Settings…" action:@selector(showPreferences:) keyEquivalent:@","];
    [appMenu addItem:[NSMenuItem separatorItem]];
    [appMenu addItemWithTitle:@"Hide NSMBW Recomp" action:@selector(hide:) keyEquivalent:@"h"];
    [appMenu addItemWithTitle:@"Quit NSMBW Recomp" action:@selector(terminate:) keyEquivalent:@"q"];
    appItem.submenu = appMenu;

    NSMenuItem *viewItem = [[NSMenuItem alloc] init];
    [bar addItem:viewItem];
    NSMenu *view = [[NSMenu alloc] initWithTitle:@"View"];
    NSMenuItem *fs = [view addItemWithTitle:@"Toggle Full Screen" action:@selector(toggleFullScreen:) keyEquivalent:@"f"];
    fs.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagControl;
    [view addItemWithTitle:@"Show FPS" action:@selector(toggleFPS:) keyEquivalent:@""];
    [view addItem:[NSMenuItem separatorItem]];
    NSMenu *filters = [[NSMenu alloc] initWithTitle:@"Output Filter"];
    NSArray *names = @[ @"Nearest", @"Bilinear", @"Sharp Bilinear", @"Scale2x", @"CRT", @"LCD Grid" ];
    for (NSUInteger i = 0; i < names.count; i++) {
        NSMenuItem *it = [filters addItemWithTitle:names[i] action:@selector(pickFilter:) keyEquivalent:@""];
        it.tag = (NSInteger)i;
        if (i < 6) it.keyEquivalent = [NSString stringWithFormat:@"%lu", (unsigned long)i + 1];
    }
    NSMenuItem *filterItem = [view addItemWithTitle:@"Output Filter" action:nil keyEquivalent:@""];
    filterItem.submenu = filters;
    NSMenu *scales = [[NSMenu alloc] initWithTitle:@"Scaling"];
    NSArray *snames = @[ @"Fit (keep aspect)", @"Integer", @"Stretch" ];
    for (NSUInteger i = 0; i < snames.count; i++) [[scales addItemWithTitle:snames[i] action:@selector(pickScale:) keyEquivalent:@""] setTag:(NSInteger)i];
    NSMenuItem *scaleItem = [view addItemWithTitle:@"Scaling" action:nil keyEquivalent:@""];
    scaleItem.submenu = scales;
    viewItem.submenu = view;

    /* Game: save states (in memory, this session) */
    NSMenuItem *gameItem = [[NSMenuItem alloc] init];
    [bar addItem:gameItem];
    NSMenu *game = [[NSMenu alloc] initWithTitle:@"Game"];
    [game addItemWithTitle:@"Save State" action:@selector(saveState:) keyEquivalent:@"s"];
    [game addItemWithTitle:@"Load State" action:@selector(loadState:) keyEquivalent:@"l"];
    [game addItem:[NSMenuItem separatorItem]];
    for (int i = 0; i < 3; i++) {
        NSMenuItem *it = [game addItemWithTitle:[NSString stringWithFormat:@"Slot %d", i + 1] action:@selector(pickSlot:)
                                  keyEquivalent:[NSString stringWithFormat:@"%d", i + 7]];
        it.tag = i;
    }
    [game addItem:[NSMenuItem separatorItem]];
    NSMenuItem *note = [game addItemWithTitle:@"States are kept until the app quits" action:nil keyEquivalent:@""];
    note.enabled = NO;
    gameItem.submenu = game;

    /* no Window menu; keep ⌘M (minimise) as a hidden shortcut in the app menu */
    NSMenuItem *mini = [appMenu addItemWithTitle:@"Minimize" action:@selector(performMiniaturize:) keyEquivalent:@"m"];
    mini.hidden = YES;
    mini.allowsKeyEquivalentWhenHidden = YES;
    NSApp.mainMenu = bar;
}

- (BOOL)validateMenuItem:(NSMenuItem *)item {
    if (item.action == @selector(pickFilter:)) item.state = item.tag == (NSInteger)rt_settings.output_filter ? NSControlStateValueOn : NSControlStateValueOff;
    if (item.action == @selector(pickScale:)) item.state = item.tag == (NSInteger)rt_settings.scale_mode ? NSControlStateValueOn : NSControlStateValueOff;
    if (item.action == @selector(toggleFPS:)) item.state = rt_settings.show_fps ? NSControlStateValueOn : NSControlStateValueOff;
    if (item.action == @selector(pickSlot:)) {
        long when = 0;
        bool used = rt_savestate_slot_info((int)item.tag, &when);
        NSString *t = [NSString stringWithFormat:@"Slot %ld", (long)item.tag + 1];
        if (used) {
            NSDateFormatter *f = [[NSDateFormatter alloc] init];
            f.timeStyle = NSDateFormatterMediumStyle; f.dateStyle = NSDateFormatterNoStyle;
            t = [t stringByAppendingFormat:@" — %@", [f stringFromDate:[NSDate dateWithTimeIntervalSince1970:when]]];
        } else t = [t stringByAppendingString:@" — empty"];
        item.title = t;
        item.state = item.tag == _slot ? NSControlStateValueOn : NSControlStateValueOff;
    }
    if (item.action == @selector(loadState:)) return rt_savestate_slot_info(_slot, NULL);
    return YES;
}

- (void)pickFilter:(NSMenuItem *)item { rt_settings.output_filter = (RtFilter)item.tag; [self saveSettings]; [self applySettings]; }
- (void)pickScale:(NSMenuItem *)item { rt_settings.scale_mode = (RtScaleMode)item.tag; [self saveSettings]; [self applySettings]; }
- (void)saveState:(id)sender { rt_savestate_request(true, _slot); }
- (void)loadState:(id)sender { rt_savestate_request(false, _slot); }
- (void)pickSlot:(NSMenuItem *)item { _slot = (int)item.tag; }
- (void)toggleFPS:(id)sender { rt_settings.show_fps = !rt_settings.show_fps; [self saveSettings]; }
- (void)toggleFullScreen:(id)sender { [self.window toggleFullScreen:sender]; }

- (void)showPreferences:(id)sender {
    if (!_prefs) _prefs = [[PreferencesWindowController alloc] initWithOwner:self];
    [_prefs showWindow:nil];
    [_prefs.window makeKeyAndOrderFront:nil];
}

@end
