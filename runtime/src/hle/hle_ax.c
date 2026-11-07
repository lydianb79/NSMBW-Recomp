/*
 * hle_ax.c - keeps the AX audio library ticking without a DSP.
 *
 * The SDK's AX output code is driven by the AI DMA interrupt (every 3 ms, one
 * 96-sample frame): __AXOutAiCallback builds the next command list, hands it
 * to the DSP and calls the game's frame callback (nw4r::snd), which runs the
 * sound engine - fades, sequence timing, streaming. Scene changes in the game
 * wait for those (e.g. the menu music fading out), so the frame pump has to
 * run even though nothing is mixed yet. There is no DSP here, so the "DSP has
 * finished the previous frame" state is set before each callback.
 */
#include <stdlib.h>
#include <string.h>
#include "../rt_internal.h"
#include "addrs_smnp01.h"

void __imp_f_801A1CE0(PPCContext *ctx, uint8_t *base);

/* What the DSP does to each voice every audio frame, minus the mixing:
 * advance the play position by 96 output samples at the voice's pitch,
 * follow loops, and stop one-shot voices at their end. The SDK copies the
 * DSP-side parameter blocks back to the CPU copies, so nw4r::snd sees voices
 * progress and finish (scenes wait for voice clips and music fades). */
#define AXPB_SIZE 0x140
static uint32_t pb32(uint8_t *base, uint32_t a) { return (uint32_t)ld16(base, a) << 16 | ld16(base, a + 2); }
static void pb32w(uint8_t *base, uint32_t a, uint32_t v) { st16(base, a, v >> 16); st16(base, a + 2, v & 0xFFFF); }

/* ------------------------------------------------------------ voice mixer */
/* Decodes one sample at the voice's current address and advances it,
 * following ADPCM frame headers, loops and the end address. */
typedef struct {
    uint8_t *base; uint32_t pb;
    int fmt, loop, is_stream, stopped;
    uint32_t loop_a, end_a, cur;
    int16_t coef[16]; int gain, ps, yn1, yn2;
} Voice;

static inline const uint8_t *phys(uint8_t *base, uint32_t byte_addr) {
    return base + (byte_addr & 0x3FFFFFFFu);
}

static int voice_next_sample(Voice *v) {
    if (v->stopped) return 0;
    int out;
    if (v->fmt == 0) {                                           /* DSP ADPCM, address in nibbles */
        if ((v->cur & 15) == 0) { v->ps = *phys(v->base, v->cur >> 1); v->cur += 2; }
        else if ((v->cur & 15) == 1) { v->cur += 1; }
        int nib = *phys(v->base, v->cur >> 1);
        nib = (v->cur & 1) ? (nib & 15) : (nib >> 4);
        if (nib >= 8) nib -= 16;
        int scale = 1 << (v->ps & 15), idx = (v->ps >> 4) & 7;
        int c1 = v->coef[idx * 2], c2 = v->coef[idx * 2 + 1];
        int val = ((nib * scale) << 11) + c1 * v->yn1 + c2 * v->yn2 + 1024;
        val >>= 11;
        if (val > 32767) val = 32767;
        if (val < -32768) val = -32768;
        v->yn2 = v->yn1; v->yn1 = val;
        out = val;
    } else if (v->fmt == 10) {                                   /* PCM16, address in samples */
        const uint8_t *p = phys(v->base, v->cur * 2);
        out = (int16_t)(p[0] << 8 | p[1]);
    } else {                                                     /* PCM8, address in bytes */
        out = (int8_t)*phys(v->base, v->cur) << 8;
    }
    if (v->cur == v->end_a) {
        if (v->loop) {
            v->cur = v->loop_a;
            if (v->fmt == 0) {                                   /* loop context */
                v->ps = ld16(v->base, v->pb + 0xB4);
                if (!v->is_stream) { v->yn1 = (int16_t)ld16(v->base, v->pb + 0xB6); v->yn2 = (int16_t)ld16(v->base, v->pb + 0xB8); }
                if ((v->cur & 15) < 2) v->cur = (v->cur & ~15u) + 2;
            }
        } else v->stopped = 1;
    } else v->cur++;
    return out;
}

#define AX_FRAME 96
static int32_t g_mix[AX_FRAME * 2];
static int32_t g_aux[2][3][AX_FRAME];      /* aux A / B sends: L, R, S */
static int32_t g_rmt[AX_FRAME];            /* Wii Remote speaker bus (mono, all remotes) */


/* ------------------------------------------------------------ aux buses */
/* The DSP's part of AUX: hand the summed sends to the game's aux callback
 * (AXFX reverb/chorus/delay, running as guest code) and add what it returns to
 * the main mix. AX's own call of the callback is suppressed in the frame hook,
 * so effect state advances exactly once per frame. */
static void guest_call2(PPCContext *ctx, uint8_t *base, uint32_t fn, uint32_t a0, uint32_t a1) {
    PPCContext save = *ctx;
    ctx->r[1] = (ctx->r[1] - 0x200) & ~15u;     /* fresh frame below the interrupted stack */
    st32(base, ctx->r[1], 0);
    ctx->r[3] = a0; ctx->r[4] = a1;
    ctx->lr = 0;
    ppc_call(ctx, base, fn);
    *ctx = save;
}

static void ax_run_aux(PPCContext *ctx, uint8_t *base) {
    static int off = -1;
    if (off < 0) off = getenv("RT_NO_REVERB") != NULL;
    if (off) return;
    const int32_t cb_sda[2] = { AX_AUXA_CB_SDA, AX_AUXB_CB_SDA }, cx_sda[2] = { AX_AUXA_CTX_SDA, AX_AUXB_CTX_SDA };
    int dpl2 = ld32(base, ctx->r[13] + (uint32_t)AX_OUTPUT_MODE_SDA) == 2;
    for (int bus = 0; bus < 2; bus++) {
        uint32_t cb = ld32(base, ctx->r[13] + (uint32_t)cb_sda[bus]);
        if (cb < 0x80000000u || cb >= 0x81800000u + 0x01000000u) continue;
        uint32_t buf = AX_AUX_SCRATCH + (uint32_t)bus * 0x1200;   /* slot 0 of the bus' own buffers */
        uint32_t ptrs = buf + 0x600;                               /* slot 1: pointer array */
        int nch = dpl2 ? 4 : 3;
        for (int c = 0; c < nch; c++) {
            uint32_t chbuf = buf + (uint32_t)c * 0x180;
            st32(base, ptrs + 4u * (uint32_t)c, chbuf);
            for (int k = 0; k < AX_FRAME; k++) st32(base, chbuf + 4u * (uint32_t)k, c < 3 ? (uint32_t)g_aux[bus][c][k] : 0);
        }
        guest_call2(ctx, base, cb, ptrs, ld32(base, ctx->r[13] + (uint32_t)cx_sda[bus]));
        for (int k = 0; k < AX_FRAME; k++) {
            int32_t l = (int32_t)ld32(base, buf + 4u * (uint32_t)k), r = (int32_t)ld32(base, buf + 0x180 + 4u * (uint32_t)k);
            int32_t sur = (int32_t)ld32(base, buf + 0x300 + 4u * (uint32_t)k);
            g_mix[k * 2] += l + sur / 2;
            g_mix[k * 2 + 1] += r + sur / 2;
        }
    }
}

/* Wii Remote speaker: the remote's small speaker, played on the host output
 * through a band-pass (~400 Hz - 3.5 kHz) so it sounds like it. */
static void ax_mix_remote(void) {
    static float hp_x, hp_y, lp_y;
    static int off = -1;
    if (off < 0) off = getenv("RT_NO_REMOTE_SPEAKER") != NULL;
    if (off) return;
    const float a_hp = 0.926f;          /* 1-pole high-pass, fc ~ 380 Hz at 32 kHz */
    const float a_lp = 0.497f;          /* 1-pole low-pass,  fc ~ 3.5 kHz */
    for (int k = 0; k < AX_FRAME; k++) {
        float x = (float)g_rmt[k];
        float y = a_hp * (hp_y + x - hp_x);
        hp_x = x; hp_y = y;
        lp_y += a_lp * (y - lp_y);
        int32_t o = (int32_t)(lp_y * 0.8f);
        g_mix[k * 2] += o; g_mix[k * 2 + 1] += o;
    }
}

static void ax_mix_voices(PPCContext *ctx, uint8_t *base) {
    memset(g_mix, 0, sizeof g_mix);
    memset(g_aux, 0, sizeof g_aux);
    memset(g_rmt, 0, sizeof g_rmt);
    uint32_t arr = ld32(base, ctx->r[13] + (uint32_t)AX_DSP_PB_ARRAY_SDA);
    uint32_t n = ld32(base, ctx->r[13] + (uint32_t)AX_NUM_VOICES_SDA);
    if (arr < 0x80000000u || arr >= 0x94000000u || n == 0 || n > 128) { rt_audio_submit(g_mix, AX_FRAME); return; }
    for (uint32_t i = 0; i < n; i++) {
        uint32_t pb = arr + i * AXPB_SIZE;
        if (ld16(base, pb + 0x10) != 1) continue;                 /* AX_PB_STATE_RUN */
        Voice v = { .base = base, .pb = pb };
        uint32_t ad = pb + 0x6E;
        v.loop = ld16(base, ad + 0x0) != 0;
        v.fmt = ld16(base, ad + 0x2);
        v.loop_a = pb32(base, ad + 0x4); v.end_a = pb32(base, ad + 0x8); v.cur = pb32(base, ad + 0xC);
        v.is_stream = ld16(base, pb + 0x12) == 1;
        if (v.loop && v.loop_a > v.end_a) v.loop = 0;
        if (v.fmt != 0 && v.fmt != 10 && v.fmt != 25) continue;
        for (int k = 0; k < 16; k++) v.coef[k] = (int16_t)ld16(base, pb + 0x7E + 2 * k);
        v.ps = ld16(base, pb + 0xA0); v.yn1 = (int16_t)ld16(base, pb + 0xA2); v.yn2 = (int16_t)ld16(base, pb + 0xA4);
        if (v.fmt == 0 && (v.cur & 15) < 2) { if ((v.cur & 15) == 0) v.ps = *phys(base, v.cur >> 1); v.cur = (v.cur & ~15u) + 2; }

        /* sample-rate conversion: 16.16 ratio, linear interpolation between the
         * last two input samples (kept in last_samples[2..3]) */
        uint32_t src = pb + 0xA6;
        uint32_t ratio = pb32(base, src + 0x0);
        if (ld16(base, pb + 0x8) == 2 || ratio == 0) ratio = 0x10000;
        if (ratio > 0x40000) ratio = 0x40000;
        uint32_t frac = ld16(base, src + 0x4);
        int s0 = (int16_t)ld16(base, src + 0x6 + 4), s1 = (int16_t)ld16(base, src + 0x6 + 6);

        /* volume envelope (1.15) and main-bus mix (1.15) with per-sample deltas */
        int32_t ve = ld16(base, pb + 0x6A), ved = (int16_t)ld16(base, pb + 0x6C);
        uint32_t mixc = ld32(base, pb + 0xC);
        int32_t vl = ld16(base, pb + 0x14), vr = ld16(base, pb + 0x18);
        int32_t dl = (mixc & 4) ? (int16_t)ld16(base, pb + 0x16) : 0, dr = (mixc & 4) ? (int16_t)ld16(base, pb + 0x1A) : 0;
        if (!(mixc & 1)) vl = 0;
        if (!(mixc & 2)) vr = 0;
        /* aux sends (AXPBMIX: A L/R +0x08/+0x0C, B L/R +0x10/+0x14, S +0x20, A S +0x24, B S +0x28) */
        int32_t aal = (mixc & (1u << 16)) ? ld16(base, pb + 0x14 + 0x08) : 0, aar = (mixc & (1u << 17)) ? ld16(base, pb + 0x14 + 0x0C) : 0;
        int32_t bal = (mixc & (1u << 21)) ? ld16(base, pb + 0x14 + 0x10) : 0, bar = (mixc & (1u << 22)) ? ld16(base, pb + 0x14 + 0x14) : 0;
        int32_t aas = (mixc & (1u << 19)) ? ld16(base, pb + 0x14 + 0x24) : 0, bas = (mixc & (1u << 24)) ? ld16(base, pb + 0x14 + 0x28) : 0;
        /* remote speaker: AXPB.remote (+0xD6), rmtMix main volumes for remotes 0..3 (+0xDA, +0xE2, +0xEA, +0xF2) */
        int32_t rv = 0;
        if (ld16(base, pb + 0xD6)) {
            uint32_t rc = ld16(base, pb + 0xD8);
            for (int r = 0; r < 4; r++) if (rc & (1u << (4 * r))) rv += ld16(base, pb + 0xDA + 8 * r);
        }

        for (int k = 0; k < AX_FRAME; k++) {
            frac += ratio;
            while (frac >= 0x10000) { frac -= 0x10000; s0 = s1; s1 = voice_next_sample(&v); }
            int smp = s0 + (int)(((int64_t)(s1 - s0) * (int64_t)frac) >> 16);
            smp = (int)(((int64_t)smp * ve) >> 15);
            g_mix[k * 2 + 0] += (int32_t)(((int64_t)smp * vl) >> 15);
            g_mix[k * 2 + 1] += (int32_t)(((int64_t)smp * vr) >> 15);
            if (aal | aar | aas) {
                g_aux[0][0][k] += (int32_t)(((int64_t)smp * aal) >> 15);
                g_aux[0][1][k] += (int32_t)(((int64_t)smp * aar) >> 15);
                g_aux[0][2][k] += (int32_t)(((int64_t)smp * aas) >> 15);
            }
            if (bal | bar | bas) {
                g_aux[1][0][k] += (int32_t)(((int64_t)smp * bal) >> 15);
                g_aux[1][1][k] += (int32_t)(((int64_t)smp * bar) >> 15);
                g_aux[1][2][k] += (int32_t)(((int64_t)smp * bas) >> 15);
            }
            if (rv) g_rmt[k] += (int32_t)(((int64_t)smp * rv) >> 15);
            ve += ved; if (ve < 0) ve = 0; if (ve > 0xFFFF) ve = 0xFFFF;
            vl += dl; vr += dr;
            if (vl < 0) vl = 0; if (vr < 0) vr = 0;
            if (v.stopped && frac == 0) break;
        }

        /* write back what the DSP would: addresses, decoder state, SRC state, envelope */
        /* (AX copies these DSP-side results back into its CPU-side voices) */
        {
            uint32_t q = pb;
            pb32w(base, q + 0x6E + 0xC, v.cur);
            st16(base, q + 0xA0, (uint16_t)v.ps); st16(base, q + 0xA2, (uint16_t)v.yn1); st16(base, q + 0xA4, (uint16_t)v.yn2);
            st16(base, q + 0xA6 + 0x4, (uint16_t)frac);
            st16(base, q + 0xA6 + 0x6 + 4, (uint16_t)s0); st16(base, q + 0xA6 + 0x6 + 6, (uint16_t)s1);
            st16(base, q + 0x6A, (uint16_t)ve);
            if (mixc & 4) { st16(base, q + 0x14, (uint16_t)vl); st16(base, q + 0x18, (uint16_t)vr); }
            if (v.stopped) st16(base, q + 0x10, 0);               /* AX_PB_STATE_STOP */
        }
    }
    ax_run_aux(ctx, base);
    ax_mix_remote();
    rt_audio_submit(g_mix, AX_FRAME);
}

static int getenv_cached_no_reverb(void) { static int v = -1; if (v < 0) v = getenv("RT_NO_REVERB") != NULL; return v; }

PPC_HLE(HLE_AXOutAiCallback) {
    rt_stats.hle_hits++;
    static int n;
    if (getenv("RT_AX_TRACE") && (n++ % 1000) == 0)
        rt_log(0, "ax: ai callback #%d, disabled %u state %u user cb %08X", n,
               ld32(base, ctx->r[13] - 0x5104), ld32(base, ctx->r[13] + (uint32_t)AX_OUT_DSP_STATE_SDA), ld32(base, ctx->r[13] - 0x50FC));
    st32(base, ctx->r[13] + (uint32_t)AX_OUT_DSP_STATE_SDA, 1);
    ax_mix_voices(ctx, base);
    uint32_t sda[3] = { AX_AUXA_CB_SDA, AX_AUXB_CB_SDA, AX_AUXC_CB_SDA }, saved[3];
    int ours = !getenv_cached_no_reverb();
    for (int i = 0; i < 3 && ours; i++) { saved[i] = ld32(base, ctx->r[13] + sda[i]); st32(base, ctx->r[13] + sda[i], 0); }
    __imp_f_801A1CE0(ctx, base);
    for (int i = 0; i < 3 && ours; i++) if (!ld32(base, ctx->r[13] + sda[i])) st32(base, ctx->r[13] + sda[i], saved[i]);
}

/* Until the audio path is complete, some sounds never get past nw4r::snd's
 * prepare stage, so code that waits for "sound ID n finished" (e.g. the
 * character voice after player selection, dScGameSetup_c VoiceEndWait) would
 * wait forever. If the same sound has been reported active for 3 seconds of
 * continuous polling, report it finished. */
void __imp_f_80196740(PPCContext *ctx, uint8_t *base);
PPC_HLE(HLE_SndIsSoundActive) {
    uint32_t id = ctx->r[4];
    __imp_f_80196740(ctx, base);
    static int off = -1;
    if (off < 0) off = getenv("RT_NO_SND_SAFETY") != NULL;
    if (off) return;
    static uint32_t last_id = 0xFFFFFFFFu; static uint64_t since_vi;
    if (!(ctx->r[3] & 0xFF)) { if (id == last_id) last_id = 0xFFFFFFFFu; return; }
    if (id != last_id) { last_id = id; since_vi = rt_stats.vi_frames; return; }
    if (rt_stats.vi_frames - since_vi > 180) {
        static uint32_t logged = 0xFFFFFFFFu;
        if (logged != id) { logged = id; rt_log(1, "snd: sound %X still active after 3 s of waiting, treating it as finished", id); }
        ctx->r[3] = 0;
    }
}
