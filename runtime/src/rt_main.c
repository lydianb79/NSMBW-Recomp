/*
 * rt_main.c - runtime lifecycle: init, run, logging, fatal errors and the
 * VI frame pacer that drives retrace interrupts.
 */
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "rt_internal.h"

RtStats rt_stats;
PPCContext *rt_main_ctx;
int rt_log_level = 1;
static RtLogFn g_log;
static volatile int g_quit;
static uint32_t g_entry;

void rt_threads_init_main(PPCContext *ctx);
PPCContext *rt_thread_main_ctx(void);
void rt_irq_vi_tick(void);
void rt_threads_bind_current(void);

void rt_set_log(RtLogFn fn) { g_log = fn; }
static void (*g_fatal_hook)(const char *);
void rt_set_fatal_hook(void (*hook)(const char *)) { g_fatal_hook = hook; }

void rt_log(int level, const char *fmt, ...) {
    if (level > rt_log_level) return;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (g_log) g_log(level, buf);
    else fprintf(stderr, "[rt] %s\n", buf);
}

uint32_t rt_last_mmio_addr, rt_last_mmio_value;

void rt_dump_state(PPCContext *ctx) {
    if (ctx) {
        rt_log(0, "  lr=%08X ctr=%08X r1=%08X r2=%08X r13=%08X msr=%08X", ctx->lr, ctx->ctr, ctx->r[1], ctx->r[2], ctx->r[13], ctx->msr);
        for (int i = 0; i < 32; i += 8)
            rt_log(0, "  r%-2d %08X %08X %08X %08X %08X %08X %08X %08X", i, ctx->r[i], ctx->r[i + 1], ctx->r[i + 2],
                   ctx->r[i + 3], ctx->r[i + 4], ctx->r[i + 5], ctx->r[i + 6], ctx->r[i + 7]);
        /* walk the guest back-chain: [r1] = prev sp, [sp+4] = saved lr */
        uint32_t sp = ctx->r[1];
        for (int d = 0; d < 16 && sp >= 0x80000000u && sp < 0x94000000u; d++) {
            uint32_t prev = ld32(rt_base, sp);
            if (prev <= sp || prev >= 0x94000000u) break;
            rt_log(0, "  #%-2d called from %08X", d, ld32(rt_base, prev + 4));
            sp = prev;
        }
    }
    rt_log(0, "  last mmio %08X = %08X", rt_last_mmio_addr, rt_last_mmio_value);
    rt_log(0, "stats: calls(indirect)=%llu unknown=%llu mmio r/w=%llu/%llu gx bytes=%llu draws=%llu hle=%llu vi=%llu",
           (unsigned long long)rt_stats.indirect_calls, (unsigned long long)rt_stats.unknown_calls,
           (unsigned long long)rt_stats.mmio_reads, (unsigned long long)rt_stats.mmio_writes,
           (unsigned long long)rt_stats.gx_fifo_bytes, (unsigned long long)rt_stats.gx_draws,
           (unsigned long long)rt_stats.hle_hits, (unsigned long long)rt_stats.vi_frames);
}

_Noreturn void rt_fatal(PPCContext *ctx, const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    rt_log(0, "FATAL: %s", buf);
    rt_dump_state(ctx);
    fflush(stderr);
    if (g_fatal_hook) g_fatal_hook(buf);
    exit(2);
}

uint8_t *rt_mem_base(void) { return rt_base; }
void rt_request_quit(void) { g_quit = 1; }
bool rt_quit_requested(void) { return g_quit != 0; }

static uint32_t json_entry(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    char buf[256] = { 0 };
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    (void)n;
    fclose(f);
    char *e = strstr(buf, "\"entry\"");
    return e ? (uint32_t)strtoul(strchr(e, ':') + 1, NULL, 0) : 0;
}

bool rt_init(const char *image_path, const char *image_json_path) {
    if (!rt_mem_init()) return false;
    if (!rt_image_load(image_path)) return false;
    rt_modules_load(image_json_path);
    g_entry = json_entry(image_json_path);
    rt_dispatch_init();
    rt_mmio_init();
    rt_ios_init();
    rt_gx_init();
    if (!getenv("RT_NO_RENDER")) {
        /* With 4+ cores: CPU thread + GPU thread + raster workers on the rest.
         * On small machines everything stays on the CPU thread. */
        long cores = sysconf(_SC_NPROCESSORS_ONLN);
        int async = (cores >= 4 || getenv("RT_ASYNC_GPU")) && !getenv("RT_SYNC_GPU");
        rt_gx_soft_init(async ? (int)cores - 1 : (int)cores);
        if (async) rt_gx_start_async();
    }
    rt_hle_init();
    rt_main_ctx = rt_thread_main_ctx();
    rt_boot_setup(rt_main_ctx);
    rt_threads_init_main(rt_main_ctx);
    rt_log(1, "runtime ready: entry %08X, game dir '%s'", g_entry, rt_settings.game_dir);
    return g_entry != 0;
}

/* Frame pacer: one VI retrace interrupt per field at the configured rate. */
/* Sleep until an absolute deadline. Relative sleeps drift: macOS adds timer
 * slack to every nanosleep, which turned 60 Hz into ~50 Hz. */
static uint64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}
static void sleep_until(uint64_t *deadline, uint64_t period) {
    *deadline += period;
    uint64_t now = now_ns();
    if (*deadline + 100000000ull < now) *deadline = now;          /* fell far behind: resync */
    while ((now = now_ns()) < *deadline) {
        uint64_t left = *deadline - now;
        if (left > 1500000ull) {                                     /* coarse sleep, then a short spin */
            struct timespec ts = { 0, (long)(left - 1000000ull) };
            nanosleep(&ts, NULL);
        } else if (left > 50000ull) {
            struct timespec ts = { 0, 20000L };
            nanosleep(&ts, NULL);
        }
    }
}

/* Display lock: the host reports the display's refresh (rt_vi_display_sync).
 * When the display runs at 60 Hz or a multiple (120, 180, 240), the VI clock
 * takes the display's exact period and is phase-locked half a refresh after
 * each vsync. The game then finishes every frame mid-interval and the
 * presenter shows each one for exactly one VI period - no beat between two
 * free-running clocks (a doubled or dropped frame every few seconds). */
static pthread_mutex_t g_vsync_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_vsync_ns, g_vsync_period_ns, g_vsync_seen_ns;

void rt_vi_display_sync(int64_t next_vsync_in_ns, uint64_t period_ns) {
    uint64_t now = now_ns();
    pthread_mutex_lock(&g_vsync_lock);
    g_vsync_ns = (uint64_t)((int64_t)now + next_vsync_in_ns);
    g_vsync_period_ns = period_ns;
    g_vsync_seen_ns = now;
    pthread_mutex_unlock(&g_vsync_lock);
}

static void *vi_thread(void *arg) {
    (void)arg;
    uint64_t deadline = now_ns();
    int locked = 0, no_lock = getenv("RT_NO_VSYNC_LOCK") != NULL;
    while (!g_quit) {
        int hz = rt_settings.fps_limit > 0 ? rt_settings.fps_limit : 60;
        uint64_t period = 1000000000ull / (uint64_t)hz;
        pthread_mutex_lock(&g_vsync_lock);
        uint64_t vs = g_vsync_ns, dp = g_vsync_period_ns, seen = g_vsync_seen_ns;
        pthread_mutex_unlock(&g_vsync_lock);
        int lock_now = 0;
        if (hz == 60 && dp > 2000000ull && now_ns() - seen < 250000000ull && !no_lock) {
            uint64_t k = (period + dp / 2) / dp;
            if (k < 1) k = 1;
            uint64_t p = dp * k;
            if (p > period - period / 40 && p < period + period / 40) {   /* within 2.5% of 60 Hz */
                period = p;
                lock_now = 1;
                /* pull the next deadline toward vsync + half a refresh (any refresh will do) */
                uint64_t next = deadline + period, target = vs + dp / 2;
                int64_t err = (int64_t)((next - target) % dp);
                if (next < target) err = -(int64_t)((target - next) % dp);
                if (err > (int64_t)dp / 2) err -= (int64_t)dp;
                if (err < -(int64_t)dp / 2) err += (int64_t)dp;
                deadline -= (uint64_t)(err / 4);
            }
        }
        if (lock_now != locked) {
            locked = lock_now;
            rt_log(1, "vi: %s", locked ? "locked to the display refresh" : "free-running timer");
        }
        sleep_until(&deadline, period);
        rt_irq_vi_tick();
    }
    return NULL;
}

/* Audio DMA pacer: one AI DMA completion per buffer period (AX: 3 ms). */
extern volatile int rt_ai_dma_due;
uint32_t rt_ai_dma_period_us(void);
static void *ai_thread(void *arg) {
    (void)arg;
    uint64_t deadline = now_ns();
    while (!g_quit) {
        uint32_t us = rt_ai_dma_period_us();
        if (!us) { struct timespec ts = { 0, 2000000L }; nanosleep(&ts, NULL); deadline = now_ns(); continue; }
        sleep_until(&deadline, (uint64_t)us * 1000ull);
        rt_ai_dma_due = 1;
        ppc_irq_pending = 1;
    }
    return NULL;
}

void rt_run(void) {
    pthread_t vi, ai;
    pthread_create(&vi, NULL, vi_thread, NULL);
    if (!getenv("RT_NO_AUDIO_DMA")) pthread_create(&ai, NULL, ai_thread, NULL);
    rt_threads_bind_current();
    rt_log(1, "jumping to __start (%08X)", g_entry);
    ppc_call(rt_main_ctx, rt_base, g_entry);
    rt_log(0, "__start returned");
}
