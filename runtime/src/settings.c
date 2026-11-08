/*
 * settings.c - graphics / input / path settings, stored as a simple
 * `key = value` text file (~/Library/Application Support/NSMBWRecomp/settings.ini).
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rt_internal.h"

RtSettings rt_settings;

static const char *k_button_names[RT_BTN_COUNT] = {
    "up", "down", "left", "right", "a", "b", "one", "two", "plus", "minus", "home",
    "shake", "tilt_left", "tilt_right", "nunchuk_c", "nunchuk_z",
};
const char *rt_button_name(RtButton b) { return b < RT_BTN_COUNT ? k_button_names[b] : "?"; }

/* Default keyboard layout uses macOS virtual key codes (kVK_*):
 * NSMBW holds the Wii Remote sideways: d-pad moves, 2 = jump, 1 = run/fire. */
enum { KVK_A = 0x00, KVK_S = 0x01, KVK_D = 0x02, KVK_W = 0x0D, KVK_Q = 0x0C, KVK_E = 0x0E,
       KVK_J = 0x26, KVK_K = 0x28, KVK_L = 0x25, KVK_I = 0x22, KVK_RETURN = 0x24, KVK_TAB = 0x30,
       KVK_SPACE = 0x31, KVK_ESC = 0x35, KVK_LEFT = 0x7B, KVK_RIGHT = 0x7C, KVK_DOWN = 0x7D, KVK_UP = 0x7E,
       KVK_Z = 0x06, KVK_X = 0x07, KVK_C = 0x08, KVK_SHIFT = 0x38, KVK_H = 0x04 };

/* Gamepad element codes (front-end maps GCExtendedGamepad elements to these) */
enum { PAD_DPAD_UP = 1, PAD_DPAD_DOWN, PAD_DPAD_LEFT, PAD_DPAD_RIGHT, PAD_A, PAD_B, PAD_X, PAD_Y,
       PAD_L1, PAD_R1, PAD_L2, PAD_R2, PAD_MENU, PAD_OPTIONS, PAD_HOME, PAD_L3, PAD_R3 };

void rt_settings_defaults(RtSettings *s) {
    memset(s, 0, sizeof *s);
    s->internal_res = 2;
    s->output_filter = RT_FILTER_SHARP_BILINEAR;
    s->scale_mode = RT_SCALE_FIT;
    s->aspect = RT_ASPECT_AUTO;
    s->texture_filter = RT_TEXFILTER_GAME;
    s->audio_volume = 80;
    s->audio_mute = false;
    s->audio_reverb = true;
    s->remote_speaker = true;
    s->remote_volume = 80;
    s->audio_latency_ms = 60;
    s->app_icon = 6;                    /* Pixel Leap */
    s->anisotropy = 4;
    s->msaa = 1;
    s->widescreen = true;
    s->vsync = true;
    s->fps_limit = 60;
    s->crt_scanline = 0.35f;
    s->sharpness = 0.5f;
    s->stick_deadzone = 0.25f;
    s->stick_as_dpad = true;
    s->rumble = true;
    for (int p = 0; p < 4; p++) {
        for (int b = 0; b < RT_BTN_COUNT; b++) { s->key[p][b] = -1; s->pad[p][b] = 0; }
        s->pad_index[p] = p;
    }
    int *k = s->key[0];
    k[RT_BTN_UP] = KVK_UP; k[RT_BTN_DOWN] = KVK_DOWN; k[RT_BTN_LEFT] = KVK_LEFT; k[RT_BTN_RIGHT] = KVK_RIGHT;
    k[RT_BTN_TWO] = KVK_X; k[RT_BTN_ONE] = KVK_Z; k[RT_BTN_A] = KVK_C; k[RT_BTN_B] = KVK_SHIFT;
    k[RT_BTN_PLUS] = KVK_RETURN; k[RT_BTN_MINUS] = KVK_TAB; k[RT_BTN_HOME] = KVK_H;
    k[RT_BTN_SHAKE] = KVK_SPACE; k[RT_BTN_TILT_LEFT] = KVK_Q; k[RT_BTN_TILT_RIGHT] = KVK_E;
    for (int p = 0; p < 4; p++) {
        int *g = s->pad[p];
        g[RT_BTN_UP] = PAD_DPAD_UP; g[RT_BTN_DOWN] = PAD_DPAD_DOWN; g[RT_BTN_LEFT] = PAD_DPAD_LEFT; g[RT_BTN_RIGHT] = PAD_DPAD_RIGHT;
        g[RT_BTN_TWO] = PAD_A; g[RT_BTN_ONE] = PAD_X; g[RT_BTN_A] = PAD_B; g[RT_BTN_B] = PAD_R2;
        g[RT_BTN_PLUS] = PAD_MENU; g[RT_BTN_MINUS] = PAD_OPTIONS; g[RT_BTN_HOME] = PAD_HOME;
        g[RT_BTN_SHAKE] = PAD_Y; g[RT_BTN_TILT_LEFT] = PAD_L1; g[RT_BTN_TILT_RIGHT] = PAD_R1;
    }
}

#define RT_SETTINGS_FIELDS(X) \
    X(internal_res, i) X(output_filter, e) X(scale_mode, e) X(aspect, e) X(texture_filter, e) X(renderer, i) X(audio_volume, i) X(audio_mute, b) X(audio_reverb, b) X(remote_speaker, b) X(remote_volume, i) X(audio_latency_ms, i) X(app_icon, i) \
    X(anisotropy, i) X(msaa, i) X(widescreen, b) X(vsync, b) X(fullscreen, b) X(fps_limit, i) \
    X(crt_scanline, f) X(sharpness, f) X(show_fps, b) X(stick_deadzone, f) X(stick_as_dpad, b) X(rumble, b)

bool rt_settings_save(const RtSettings *s, const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) return false;
    fprintf(f, "# NSMBW recomp settings\n");
#define W_i(n) fprintf(f, #n " = %d\n", (int)s->n);
#define W_e(n) fprintf(f, #n " = %d\n", (int)s->n);
#define W_b(n) fprintf(f, #n " = %d\n", s->n ? 1 : 0);
#define W_f(n) fprintf(f, #n " = %.3f\n", (double)s->n);
#define X(n, t) W_##t(n)
    RT_SETTINGS_FIELDS(X)
#undef X
    fprintf(f, "game_dir = %s\n", s->game_dir);
    for (int p = 0; p < 4; p++) {
        fprintf(f, "p%d.pad_index = %d\n", p + 1, s->pad_index[p]);
        for (int b = 0; b < RT_BTN_COUNT; b++) {
            fprintf(f, "p%d.key.%s = %d\n", p + 1, k_button_names[b], s->key[p][b]);
            fprintf(f, "p%d.pad.%s = %d\n", p + 1, k_button_names[b], s->pad[p][b]);
        }
    }
    fclose(f);
    return true;
}

bool rt_settings_load(RtSettings *s, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return false;
    char line[1400];
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (line[0] == '#' || !eq) continue;
        *eq = 0;
        char *key = line, *val = eq + 1;
        while (*key == ' ') key++;
        for (char *e = eq - 1; e >= key && *e == ' '; e--) *e = 0;
        while (*val == ' ') val++;
        val[strcspn(val, "\r\n")] = 0;
#define R_i(n) if (!strcmp(key, #n)) { s->n = atoi(val); continue; }
#define R_e(n) if (!strcmp(key, #n)) { s->n = atoi(val); continue; }
#define R_b(n) if (!strcmp(key, #n)) { s->n = atoi(val) != 0; continue; }
#define R_f(n) if (!strcmp(key, #n)) { s->n = (float)atof(val); continue; }
#define X(n, t) R_##t(n)
        RT_SETTINGS_FIELDS(X)
#undef X
        if (!strcmp(key, "game_dir")) { snprintf(s->game_dir, sizeof s->game_dir, "%s", val); continue; }
        int p;
        char kind[8], name[32];
        if (sscanf(key, "p%d.%7[^.].%31s", &p, kind, name) == 3 && p >= 1 && p <= 4) {
            for (int b = 0; b < RT_BTN_COUNT; b++)
                if (!strcmp(name, k_button_names[b])) {
                    if (!strcmp(kind, "key")) s->key[p - 1][b] = atoi(val);
                    else if (!strcmp(kind, "pad")) s->pad[p - 1][b] = atoi(val);
                }
        } else if (sscanf(key, "p%d.pad_index", &p) == 1 && p >= 1 && p <= 4) {
            s->pad_index[p - 1] = atoi(val);
        }
    }
    fclose(f);
    if (s->internal_res < 1) s->internal_res = 1;
    if (s->internal_res > 8) s->internal_res = 8;
    if ((unsigned)s->output_filter >= RT_FILTER_COUNT) s->output_filter = RT_FILTER_BILINEAR;
    if ((unsigned)s->scale_mode >= RT_SCALE_COUNT) s->scale_mode = RT_SCALE_FIT;
    if ((unsigned)s->aspect >= RT_ASPECT_COUNT) s->aspect = RT_ASPECT_AUTO;
    return true;
}

/* ------------------------------------------------------------ input */
static pthread_mutex_t g_in_lock = PTHREAD_MUTEX_INITIALIZER;
static RtPlayerInput g_in[4];

void rt_input_set(int player, const RtPlayerInput *in) {
    if (player < 0 || player > 3) return;
    pthread_mutex_lock(&g_in_lock);
    g_in[player] = *in;
    pthread_mutex_unlock(&g_in_lock);
}
void rt_input_get(int player, RtPlayerInput *out) {
    if (player < 0 || player > 3) { memset(out, 0, sizeof *out); return; }
    pthread_mutex_lock(&g_in_lock);
    *out = g_in[player];
    pthread_mutex_unlock(&g_in_lock);
}
