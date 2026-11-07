/*
 * video.c - presents the external framebuffer (XFB, YUYV 4:2:2 in guest RAM)
 * as an RGBA image for the host front-end.
 *
 * With a GX backend attached the backend renders the EFB at the configured
 * internal resolution and publishes it directly; this XFB path is the
 * universal fallback (and what the headless runner uses for screenshots).
 */
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "rt_internal.h"

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t *g_rgba[2];
static uint32_t g_w, g_h, g_front;
static uint64_t g_frame;
static uint32_t g_pending_xfb;

static inline uint8_t clamp8(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }

static void convert_xfb(uint32_t xfb, uint32_t w, uint32_t h, uint32_t stride, uint32_t *dst) {
    const uint8_t *src = ppc_host(rt_base, xfb | 0x80000000u);
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *row = src + (size_t)y * stride;
        uint32_t *out = dst + (size_t)y * w;
        for (uint32_t x = 0; x + 1 < w; x += 2) {
            int y0 = row[x * 2 + 0] - 16, u = row[x * 2 + 1] - 128;
            int y1 = row[x * 2 + 2] - 16, v = row[x * 2 + 3] - 128;
            int c0 = 298 * y0, c1 = 298 * y1;
            int rv = 409 * v, gu = -100 * u - 208 * v, bu = 516 * u;
            out[x]     = 0xFF000000u | (uint32_t)clamp8((c0 + bu + 128) >> 8) << 16 |
                         (uint32_t)clamp8((c0 + gu + 128) >> 8) << 8 | clamp8((c0 + rv + 128) >> 8);
            out[x + 1] = 0xFF000000u | (uint32_t)clamp8((c1 + bu + 128) >> 8) << 16 |
                         (uint32_t)clamp8((c1 + gu + 128) >> 8) << 8 | clamp8((c1 + rv + 128) >> 8);
        }
    }
}

void rt_gx_copy_to_xfb(uint32_t dest, uint32_t w, uint32_t h) {
    (void)w; (void)h;
    g_pending_xfb = dest;   /* the next retrace shows whatever VI points at */
}

void rt_video_vi_retrace(void) {
    uint32_t xfb = rt_vi_xfb_addr ? rt_vi_xfb_addr : g_pending_xfb;
    if (!xfb) return;
    uint32_t w = rt_vi_width, h = rt_vi_height, stride = rt_vi_stride;
    if (w == 0 || h == 0 || w > 1024 || h > 1024) return;
    if (stride < w * 2) stride = w * 2;
    /* Interlaced modes program the VI with a two-line stride (each field
     * reads every other line). The XFB itself is a progressive frame, so
     * present it with the single-line stride. */
    if (stride >= w * 4) stride /= 2;
    pthread_mutex_lock(&g_lock);
    uint32_t back = g_front ^ 1;
    if (g_w != w || g_h != h) {
        free(g_rgba[0]); free(g_rgba[1]);
        g_rgba[0] = calloc((size_t)w * h, 4);
        g_rgba[1] = calloc((size_t)w * h, 4);
        g_w = w; g_h = h;
    }
    pthread_mutex_unlock(&g_lock);
    convert_xfb(xfb, w, h, stride, g_rgba[back]);
    pthread_mutex_lock(&g_lock);
    g_front = back;
    g_frame++;
    pthread_mutex_unlock(&g_lock);
}

bool rt_video_frame(RtFrame *out) {
    pthread_mutex_lock(&g_lock);
    bool ok = g_frame > 0 && g_rgba[g_front];
    if (ok) {
        out->width = g_w; out->height = g_h;
        out->rgba = g_rgba[g_front];
        out->frame = g_frame;
        out->widescreen = rt_settings.aspect == RT_ASPECT_16_9 ||
                          (rt_settings.aspect == RT_ASPECT_AUTO && rt_settings.widescreen_hack);
    }
    pthread_mutex_unlock(&g_lock);
    return ok;
}
