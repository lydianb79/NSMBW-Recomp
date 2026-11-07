// InputManager.m - keyboard + GameController (Xbox, PlayStation, Switch Pro,
// MFi) mapped onto the Wii Remote/Nunchuk buttons the game expects.
#import "InputManager.h"
#include <math.h>
#include "rt.h"

@implementation InputManager {
    bool _keys[256];
    NSLock *_lock;
}

+ (instancetype)shared {
    static InputManager *m;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ m = [[InputManager alloc] init]; });
    return m;
}

- (instancetype)init {
    if ((self = [super init])) _lock = [[NSLock alloc] init];
    return self;
}

- (void)start {
    [GCController startWirelessControllerDiscoveryWithCompletionHandler:nil];
    NSNotificationCenter *nc = [NSNotificationCenter defaultCenter];
    [nc addObserverForName:GCControllerDidConnectNotification object:nil queue:nil usingBlock:^(NSNotification *n) {
        GCController *c = n.object;
        NSLog(@"controller connected: %@", c.vendorName);
    }];
    [nc addObserverForName:GCControllerDidDisconnectNotification object:nil queue:nil usingBlock:^(NSNotification *n) {
        NSLog(@"controller disconnected");
    }];
}

- (void)key:(unsigned short)keyCode down:(BOOL)down {
    if (keyCode < 256) _keys[keyCode] = down;
}

- (NSArray<GCController *> *)controllers {
    NSMutableArray *a = [NSMutableArray array];
    for (GCController *c in GCController.controllers) if (c.extendedGamepad) [a addObject:c];
    return a;
}

static BOOL padPressed(GCExtendedGamepad *g, int el) {
    switch (el) {
        case PadDpadUp: return g.dpad.up.isPressed;
        case PadDpadDown: return g.dpad.down.isPressed;
        case PadDpadLeft: return g.dpad.left.isPressed;
        case PadDpadRight: return g.dpad.right.isPressed;
        case PadA: return g.buttonA.isPressed;
        case PadB: return g.buttonB.isPressed;
        case PadX: return g.buttonX.isPressed;
        case PadY: return g.buttonY.isPressed;
        case PadL1: return g.leftShoulder.isPressed;
        case PadR1: return g.rightShoulder.isPressed;
        case PadL2: return g.leftTrigger.isPressed;
        case PadR2: return g.rightTrigger.isPressed;
        case PadMenu: return g.buttonMenu.isPressed;
        case PadOptions: return g.buttonOptions.isPressed;
        case PadHome: return g.buttonHome.isPressed;
        case PadL3: return g.leftThumbstickButton.isPressed;
        case PadR3: return g.rightThumbstickButton.isPressed;
        default: return NO;
    }
}

- (PadElement)firstPressedElementOn:(GCController *)c {
    GCExtendedGamepad *g = c.extendedGamepad;
    if (!g) return PadNone;
    for (int el = 1; el < PadElementCount; el++) if (padPressed(g, el)) return (PadElement)el;
    return PadNone;
}

- (void)poll {
    NSArray<GCController *> *pads = [self controllers];
    float dz = rt_settings.stick_deadzone;
    for (int p = 0; p < 4; p++) {
        RtPlayerInput in = { 0 };
        for (int b = 0; b < RT_BTN_COUNT; b++) {
            int k = rt_settings.key[p][b];
            if (k >= 0 && k < 256 && _keys[k]) in.held |= 1u << b;
        }
        in.connected = (p == 0);
        int pi = rt_settings.pad_index[p];
        if (pi >= 0 && pi < (int)pads.count) {
            GCExtendedGamepad *g = pads[pi].extendedGamepad;
            in.connected = true;
            for (int b = 0; b < RT_BTN_COUNT; b++)
                if (rt_settings.pad[p][b] && padPressed(g, rt_settings.pad[p][b])) in.held |= 1u << b;
            float x = g.leftThumbstick.xAxis.value, y = g.leftThumbstick.yAxis.value;
            if (fabsf(x) < dz) x = 0;
            if (fabsf(y) < dz) y = 0;
            in.stick_x = x;
            in.stick_y = y;
            if (rt_settings.stick_as_dpad) {
                if (x < -0.5f) in.held |= 1u << RT_BTN_LEFT;
                if (x > 0.5f) in.held |= 1u << RT_BTN_RIGHT;
                if (y > 0.5f) in.held |= 1u << RT_BTN_UP;
                if (y < -0.5f) in.held |= 1u << RT_BTN_DOWN;
            }
            float rx = g.rightThumbstick.xAxis.value;
            in.tilt = fabsf(rx) < dz ? 0 : rx;
        }
        if (in.held & (1u << RT_BTN_TILT_LEFT)) in.tilt = -1;
        if (in.held & (1u << RT_BTN_TILT_RIGHT)) in.tilt = 1;
        rt_input_set(p, &in);
    }
}

+ (NSString *)nameForKey:(int)k {
    if (k < 0) return @"—";
    static NSDictionary *names;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        names = @{
            @0x00: @"A", @0x0B: @"B", @0x08: @"C", @0x02: @"D", @0x0E: @"E", @0x03: @"F", @0x05: @"G", @0x04: @"H",
            @0x22: @"I", @0x26: @"J", @0x28: @"K", @0x25: @"L", @0x2E: @"M", @0x2D: @"N", @0x1F: @"O", @0x23: @"P",
            @0x0C: @"Q", @0x0F: @"R", @0x01: @"S", @0x11: @"T", @0x20: @"U", @0x09: @"V", @0x0D: @"W", @0x07: @"X",
            @0x10: @"Y", @0x06: @"Z", @0x12: @"1", @0x13: @"2", @0x14: @"3", @0x15: @"4", @0x17: @"5", @0x16: @"6",
            @0x1A: @"7", @0x1C: @"8", @0x19: @"9", @0x1D: @"0", @0x24: @"Return", @0x30: @"Tab", @0x31: @"Space",
            @0x33: @"Delete", @0x35: @"Esc", @0x38: @"Shift", @0x3C: @"Right Shift", @0x3B: @"Control",
            @0x3A: @"Option", @0x39: @"Caps Lock", @0x7B: @"←", @0x7C: @"→", @0x7D: @"↓", @0x7E: @"↑",
            @0x2B: @",", @0x2F: @".", @0x2C: @"/", @0x29: @";", @0x27: @"'", @0x21: @"[", @0x1E: @"]",
            @0x32: @"`", @0x1B: @"-", @0x18: @"=",
        };
    });
    NSString *n = names[@(k)];
    return n ?: [NSString stringWithFormat:@"Key %02X", k];
}

+ (NSString *)nameForPadElement:(int)el {
    static NSArray *n;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        n = @[ @"—", @"D-pad Up", @"D-pad Down", @"D-pad Left", @"D-pad Right", @"A / ✕", @"B / ○", @"X / □", @"Y / △",
               @"L1 / LB", @"R1 / RB", @"L2 / LT", @"R2 / RT", @"Menu", @"Options", @"Home", @"L3", @"R3" ];
    });
    return (el >= 0 && el < (int)n.count) ? n[el] : @"?";
}

@end
