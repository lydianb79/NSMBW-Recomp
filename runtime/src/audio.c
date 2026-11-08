/*
 * audio.c - hand-off between the AX mixer (hle_ax.c, game thread, 3 ms frames
 * at 32 kHz) and the host audio device (pulls whenever it needs samples).
 *
 * A lock-free single-producer/single-consumer ring of interleaved stereo
 * int16. If the host falls behind, the oldest audio is dropped; if the game
 * falls behind, the host gets silence. RT_WAV=<file> also records the mix.
 */
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rt_internal.h"

#define RING (32000 / 2 * 2)            /* ~0.5 s of stereo frames, power of two not required */
static int16_t g_ring[RING * 2];
static _Atomic uint32_t g_wr, g_rd;     /* frame counters */
static FILE *g_wav; static uint32_t g_wav_frames;

static void wav_header(FILE *f, uint32_t frames) {
    uint32_t data = frames * 4, rate = 32000, byterate = rate * 4;
    fseek(f, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, f); uint32_t v = 36 + data; fwrite(&v, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f); v = 16; fwrite(&v, 4, 1, f);
    uint16_t h[2] = { 1, 2 }; fwrite(h, 2, 2, f);
    fwrite(&rate, 4, 1, f); fwrite(&byterate, 4, 1, f);
    uint16_t h2[2] = { 4, 16 }; fwrite(h2, 2, 2, f);
    fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
    fseek(f, 0, SEEK_END);
}

void rt_audio_submit(const int32_t *mix, int frames) {
    int vol = rt_settings.audio_mute ? 0 : rt_settings.audio_volume;   /* 0..100 */
    int16_t tmp[2 * 128];
    if (frames > 128) frames = 128;
    for (int i = 0; i < frames * 2; i++) {
        int32_t s = (int32_t)(((int64_t)mix[i] * vol) / 100);
        tmp[i] = (int16_t)(s > 32767 ? 32767 : s < -32768 ? -32768 : s);
    }
    uint32_t wr = atomic_load_explicit(&g_wr, memory_order_relaxed);
    uint32_t rd = atomic_load_explicit(&g_rd, memory_order_acquire);
    if (wr - rd + (uint32_t)frames > RING) atomic_store_explicit(&g_rd, wr + (uint32_t)frames - RING, memory_order_release);
    for (int i = 0; i < frames; i++) {
        uint32_t slot = (wr + (uint32_t)i) % RING;
        g_ring[slot * 2] = tmp[i * 2]; g_ring[slot * 2 + 1] = tmp[i * 2 + 1];
    }
    atomic_store_explicit(&g_wr, wr + (uint32_t)frames, memory_order_release);

    static int wav_init;
    if (!wav_init) { wav_init = 1; const char *p = getenv("RT_WAV"); if (p && (g_wav = fopen(p, "wb"))) wav_header(g_wav, 0); }
    if (g_wav) {
        fwrite(tmp, 4, (size_t)frames, g_wav);
        g_wav_frames += (uint32_t)frames;
        if ((g_wav_frames % 32000) < (uint32_t)frames) { wav_header(g_wav, g_wav_frames); fflush(g_wav); }
    }
}

int rt_audio_read(int16_t *dst, int frames) {
    uint32_t rd = atomic_load_explicit(&g_rd, memory_order_relaxed);
    uint32_t wr = atomic_load_explicit(&g_wr, memory_order_acquire);
    uint32_t avail = wr - rd;
    int n = (uint32_t)frames < avail ? frames : (int)avail;
    for (int i = 0; i < n; i++) {
        uint32_t slot = (rd + (uint32_t)i) % RING;
        dst[i * 2] = g_ring[slot * 2]; dst[i * 2 + 1] = g_ring[slot * 2 + 1];
    }
    memset(dst + n * 2, 0, (size_t)(frames - n) * 4);
    atomic_store_explicit(&g_rd, rd + (uint32_t)n, memory_order_release);
    return n;
}
