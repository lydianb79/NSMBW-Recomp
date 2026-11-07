#import <Foundation/Foundation.h>
#import <GameController/GameController.h>

/* Gamepad element codes stored in settings (must match runtime/src/settings.c) */
typedef NS_ENUM(int, PadElement) {
    PadNone = 0, PadDpadUp, PadDpadDown, PadDpadLeft, PadDpadRight, PadA, PadB, PadX, PadY,
    PadL1, PadR1, PadL2, PadR2, PadMenu, PadOptions, PadHome, PadL3, PadR3,
    PadElementCount
};

@interface InputManager : NSObject
+ (instancetype)shared;
- (void)start;
- (void)key:(unsigned short)keyCode down:(BOOL)down;
- (void)poll;                                   /* build RtPlayerInput for all players */
- (NSArray<GCController *> *)controllers;
- (PadElement)firstPressedElementOn:(GCController *)c;
+ (NSString *)nameForKey:(int)keyCode;
+ (NSString *)nameForPadElement:(int)el;
@end
