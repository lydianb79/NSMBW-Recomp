/*
 * headless.c - command-line runner (no window): boots the recompiled game,
 * prints runtime diagnostics, and can dump the current frame as a PPM.
 *
 *   nsmbw-headless <build-dir> <game-dir> [-v] [-t seconds] [-o frame.ppm]
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "rt.h"
#include "rt_gx.h"

static const char *g_ppm;
static int g_seconds = 10;

static void dump_ppm(const char *path) {
    RtFrame fr;
    if (!rt_video_frame(&fr)) { fprintf(stderr, "no frame yet\n"); return; }
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%u %u\n255\n", fr.width, fr.height);
    for (uint32_t i = 0; i < fr.width * fr.height; i++) {
        uint32_t p = fr.rgba[i];
        uint8_t rgb[3] = { (uint8_t)p, (uint8_t)(p >> 8), (uint8_t)(p >> 16) };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    fprintf(stderr, "wrote %s (%ux%u, frame %llu)\n", path, fr.width, fr.height, (unsigned long long)fr.frame);
}

/* RT_PRESS="a@4,two@9,right@12-15": press buttons at given seconds (player 1) */
static void scripted_input(int tenths) {
    RtPlayerInput in = { 0 };
    in.connected = true;
    const char *e = getenv("RT_PRESS");
    if (e) {
        char buf[512]; snprintf(buf, sizeof buf, "%s", e);
        for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
            char name[32]; float t0 = 0, t1 = -1;
            if (sscanf(tok, "%31[^@]@%f-%f", name, &t0, &t1) < 2) continue;
            if (t1 < 0) t1 = t0 + 0.3f;
            if (tenths >= (int)(t0 * 10) && tenths < (int)(t1 * 10))
                for (int b = 0; b < RT_BTN_COUNT; b++) if (!strcmp(name, rt_button_name((RtButton)b))) in.held |= 1u << b;
        }
    }
    rt_input_set(0, &in);
}

static void *watchdog(void *arg) {
    (void)arg;
    scripted_input(0);
    int render_from = getenv("RT_RENDER_FROM") ? atoi(getenv("RT_RENDER_FROM")) : 0;
    rt_gx_skip_draws = render_from > 0;
    for (int s = 0; s < g_seconds; s++) {
        if (render_from && s >= render_from) rt_gx_skip_draws = 0;
        for (int k = 0; k < 10; k++) { usleep(100000); scripted_input(s * 10 + k + 1); }
        fprintf(stderr, "[%2ds] mmio r/w %llu/%llu  gx %llu B, %llu draws, %llu copies  vi %llu  hle %llu\n", s + 1,
                (unsigned long long)rt_stats.mmio_reads, (unsigned long long)rt_stats.mmio_writes,
                (unsigned long long)rt_stats.gx_fifo_bytes, (unsigned long long)rt_gx.draws,
                (unsigned long long)rt_gx.efb_copies, (unsigned long long)rt_stats.vi_frames,
                (unsigned long long)rt_stats.hle_hits);
        const char *snap = getenv("RT_SNAP");   /* RT_SNAP="20,30,40": dump snap_<s>.ppm at those seconds */
        if (snap) {
            char buf[256]; snprintf(buf, sizeof buf, ",%s,", snap);
            char key[16]; snprintf(key, sizeof key, ",%d,", s + 1);
            if (strstr(buf, key)) { char path[64]; snprintf(path, sizeof path, "snap_%d.ppm", s + 1); dump_ppm(path); }
        }
    }
    if (g_ppm) dump_ppm(g_ppm);
    rt_request_quit();
    fprintf(stderr, "time limit reached; guest threads:\n");
    extern void rt_dump_threads(void);
    rt_dump_threads();
    exit(0);
}

extern int rt_log_level;

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <build-dir> <game-dir> [-v|-vv] [-t seconds] [-o frame.ppm]\n", argv[0]);
        return 1;
    }
    rt_settings_defaults(&rt_settings);
    snprintf(rt_settings.game_dir, sizeof rt_settings.game_dir, "%s", argv[2]);
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "-v")) rt_log_level = 2;
        else if (!strcmp(argv[i], "-vv")) rt_log_level = 3;
        else if (!strcmp(argv[i], "-t") && i + 1 < argc) g_seconds = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) g_ppm = argv[++i];
    }
    char img[1200], js[1200];
    snprintf(img, sizeof img, "%s/image.bin", argv[1]);
    snprintf(js, sizeof js, "%s/image.json", argv[1]);
    if (!rt_init(img, js)) return 1;
    pthread_t w;
    pthread_create(&w, NULL, watchdog, NULL);
    if (getenv("RT_RUN_ON_THREAD")) {          /* mimic the app: run the game on a separate thread */
        pthread_t g; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 64u << 20);
        pthread_create(&g, &at, (void *(*)(void *))rt_run, NULL);
        pthread_join(g, NULL);
        return 0;
    }
    rt_run();
    return 0;
}
