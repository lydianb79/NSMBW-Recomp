/*
 * rt.h - public interface between the recompiled game runtime and a host
 * front-end (the macOS app in platform/macos, or the headless runner).
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "ppc_context.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------ settings */
typedef enum { RT_FILTER_NEAREST, RT_FILTER_BILINEAR, RT_FILTER_SHARP_BILINEAR,
               RT_FILTER_SCALE2X, RT_FILTER_CRT, RT_FILTER_LCD, RT_FILTER_COUNT } RtFilter;
typedef enum { RT_SCALE_FIT, RT_SCALE_INTEGER, RT_SCALE_STRETCH, RT_SCALE_COUNT } RtScaleMode;
typedef enum { RT_ASPECT_AUTO, RT_ASPECT_4_3, RT_ASPECT_16_9, RT_ASPECT_COUNT } RtAspect;
typedef enum { RT_TEXFILTER_GAME, RT_TEXFILTER_NEAREST, RT_TEXFILTER_LINEAR, RT_TEXFILTER_COUNT } RtTexFilter;

/* Wii Remote (sideways, as NSMBW uses it) + Nunchuk + system buttons */
typedef enum {
    RT_BTN_UP, RT_BTN_DOWN, RT_BTN_LEFT, RT_BTN_RIGHT,
    RT_BTN_A, RT_BTN_B, RT_BTN_ONE, RT_BTN_TWO,
    RT_BTN_PLUS, RT_BTN_MINUS, RT_BTN_HOME,
    RT_BTN_SHAKE,                 /* spin jump / pick up (accelerometer shake) */
    RT_BTN_TILT_LEFT, RT_BTN_TILT_RIGHT,   /* tilt platforms / Wiimote roll */
    RT_BTN_NUNCHUK_C, RT_BTN_NUNCHUK_Z,
    RT_BTN_COUNT
} RtButton;

typedef struct {
    /* graphics */
    int         internal_res;       /* 1..8 x native 640x528 EFB */
    RtFilter    output_filter;
    RtScaleMode scale_mode;
    RtAspect    aspect;
    RtTexFilter texture_filter;
    int         renderer;           /* 0 = Metal (GPU), 1 = software (CPU); applied at start-up */
    int         audio_volume;       /* 0..100 */
    int         app_icon;           /* 0 day, 1 sunset, 2 night, 3 winter, 4 autumn */
    int         anisotropy;         /* 1,2,4,8,16 */
    int         msaa;               /* 1,2,4 */
    bool        widescreen_hack;
    bool        vsync;
    bool        fullscreen;
    int         fps_limit;          /* 0 = unlimited, 60 = native */
    float       crt_scanline;       /* 0..1 strength */
    float       sharpness;          /* sharp-bilinear blend 0..1 */
    bool        show_fps;
    /* input: per player, a key code (host specific) and a gamepad element */
    int         key[4][RT_BTN_COUNT];
    int         pad[4][RT_BTN_COUNT];
    int         pad_index[4];       /* which connected controller drives player n (-1 none) */
    float       stick_deadzone;
    bool        stick_as_dpad;
    /* paths */
    char        game_dir[1024];     /* extracted disc: <dir>/sys, <dir>/files */
} RtSettings;

extern RtSettings rt_settings;
void rt_settings_defaults(RtSettings *s);
bool rt_settings_load(RtSettings *s, const char *path);
bool rt_settings_save(const RtSettings *s, const char *path);
const char *rt_button_name(RtButton b);

/* ------------------------------------------------------------ input */
typedef struct {
    uint32_t held;          /* bit (1 << RtButton) */
    float    stick_x, stick_y;    /* nunchuk stick -1..1 */
    float    tilt;          /* -1..1 roll */
    bool     connected;
} RtPlayerInput;
void rt_input_set(int player, const RtPlayerInput *in);
void rt_input_get(int player, RtPlayerInput *out);

/* ------------------------------------------------------------ video */
typedef struct {
    uint32_t width, height;   /* of the image in `rgba` */
    const uint32_t *rgba;     /* 0xAABBGGRR little-endian */
    uint64_t frame;           /* increments on every VI retrace with a new image */
    bool     widescreen;      /* game asked for 16:9 (SYSCONF / VI) */
} RtFrame;
/* Returns false while no frame is available yet. */
bool rt_video_frame(RtFrame *out);

/* ------------------------------------------------------------ audio */
/* 32 kHz interleaved stereo int16. Fills `frames` frames (silence when the game
 * has produced nothing yet) and returns how many were real audio. */
int rt_audio_read(int16_t *dst, int frames);

/* ------------------------------------------------------------ lifecycle */
typedef void (*RtLogFn)(int level, const char *msg);
void rt_set_log(RtLogFn fn);
/* called (on the game thread) with the message before the runtime exits on a fatal error */
void rt_set_fatal_hook(void (*hook)(const char *msg));
void rt_log(int level, const char *fmt, ...);

bool     rt_init(const char *image_path, const char *image_json_path);
void     rt_run(void);             /* runs the game on the calling thread (does not return normally) */
void     rt_request_quit(void);
bool     rt_quit_requested(void);
uint8_t *rt_mem_base(void);
uint64_t rt_instr_budget_exhausted(void);

/* trace / statistics for the headless runner */
typedef struct {
    uint64_t calls, indirect_calls, unknown_calls, mmio_reads, mmio_writes, gx_fifo_bytes,
             gx_draws, illegal, hle_hits, vi_frames;
} RtStats;
extern RtStats rt_stats;
void rt_dump_state(PPCContext *ctx);       /* registers, guest call stack, last MMIO */
extern PPCContext *rt_main_ctx;

#ifdef __cplusplus
}
#endif
