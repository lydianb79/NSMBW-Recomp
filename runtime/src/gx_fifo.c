/*
 * gx_fifo.c - GX command processor front end.
 *
 * The game writes GX commands byte/half/word-wise to the write-gather pipe at
 * 0xCC008000. We buffer the stream, split it into complete commands and keep
 * the CP / XF / BP register state current. Draw commands are decoded to the
 * point where vertex size is known (VCD + VAT), then handed to the active
 * backend (rt_gx_backend) together with a pointer to the raw vertex data.
 *
 * A renderer (Metal, in platform/macos) implements RtGxBackend; with no
 * backend attached the commands are parsed and counted only.
 */
#include <stdlib.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>

#include "rt_internal.h"
#include "rt_gx.h"

RtGxState rt_gx;
volatile int rt_gx_skip_draws;
const RtGxBackend *rt_gx_backend;

static uint8_t  g_buf[1 << 16];
static uint32_t g_len;

static const int comp_size[8] = { 1, 1, 2, 2, 4, 0, 0, 0 };
static const int color_size[8] = { 2, 3, 4, 2, 3, 4, 0, 0 };

/* attribute descriptor from VCD: 0 none, 1 direct, 2 idx8, 3 idx16 */
static int vcd(int attr) {
    uint32_t lo = rt_gx.cp[0x50], hi = rt_gx.cp[0x60];
    switch (attr) {
    case GX_VA_POS:  return (lo >> 9) & 3;
    case GX_VA_NRM:  return (lo >> 11) & 3;
    case GX_VA_CLR0: return (lo >> 13) & 3;
    case GX_VA_CLR1: return (lo >> 15) & 3;
    default:
        if (attr >= GX_VA_TEX0 && attr <= GX_VA_TEX7) return (hi >> (2 * (attr - GX_VA_TEX0))) & 3;
        return 0;
    }
}

typedef struct { uint32_t base, nrm, clr, tex; } VtxParts;
static VtxParts vertex_parts(int vat);
uint32_t rt_gx_vertex_size(int vat) {
    VtxParts v = vertex_parts(vat);
    return v.base + v.nrm + v.clr + v.tex;
}

static VtxParts vertex_parts(int vat) {
    VtxParts vp = { 0, 0, 0, 0 };
    uint32_t a = rt_gx.cp[0x70 + vat], b = rt_gx.cp[0x80 + vat], c = rt_gx.cp[0x90 + vat];
    uint32_t lo = rt_gx.cp[0x50];
    vp.base += lo & 1;                                  /* PNMTXIDX */
    for (int i = 0; i < 8; i++) vp.base += (lo >> (1 + i)) & 1;   /* TEXnMTXIDX */
    int t;
    if ((t = vcd(GX_VA_POS))) vp.base += t == 1 ? comp_size[(a >> 1) & 7] * ((a & 1) ? 3 : 2) : t - 1;
    if ((t = vcd(GX_VA_NRM))) {
        int nbt = (a >> 9) & 1, idx3 = (a >> 31) & 1;
        if (t == 1) vp.nrm += comp_size[(a >> 10) & 7] * (nbt ? 9 : 3);
        else vp.nrm += (t - 1) * (nbt && idx3 ? 3 : 1);
    }
    if ((t = vcd(GX_VA_CLR0))) vp.clr += t == 1 ? color_size[(a >> 14) & 7] : t - 1;
    if ((t = vcd(GX_VA_CLR1))) vp.clr += t == 1 ? color_size[(a >> 18) & 7] : t - 1;
    /* texcoord formats: tex0 in A, tex1-3 + tex4 cnt/fmt in B */
    uint32_t tcnt[8], tfmt[8];
    tcnt[0] = (a >> 21) & 1; tfmt[0] = (a >> 22) & 7;
    tcnt[1] = b & 1;         tfmt[1] = (b >> 1) & 7;
    tcnt[2] = (b >> 9) & 1;  tfmt[2] = (b >> 10) & 7;
    tcnt[3] = (b >> 18) & 1; tfmt[3] = (b >> 19) & 7;
    tcnt[4] = (b >> 27) & 1; tfmt[4] = (b >> 28) & 7;
    tcnt[5] = (c >> 5) & 1;  tfmt[5] = (c >> 6) & 7;
    tcnt[6] = (c >> 14) & 1; tfmt[6] = (c >> 15) & 7;
    tcnt[7] = (c >> 23) & 1; tfmt[7] = (c >> 24) & 7;
    for (int i = 0; i < 8; i++)
        if ((t = vcd(GX_VA_TEX0 + i))) vp.tex += t == 1 ? comp_size[tfmt[i]] * (tcnt[i] ? 2 : 1) : t - 1;
    return vp;
}

static int real_opcode(uint8_t c) {
    return c == 0x08 || c == 0x10 || c == 0x20 || c == 0x28 || c == 0x30 || c == 0x38 ||
           c == 0x40 || c == 0x48 || c == 0x61 || (c >= 0x80 && c < 0xC0);
}
/* Does a command plausibly start at p[at]? NOP padding must be followed by
 * a real opcode (or run to the end of what we have). */
static int command_starts_at(const uint8_t *p, uint32_t at, uint32_t len) {
    if (at >= len) return 1;
    if (real_opcode(p[at])) return 1;
    if (p[at] != 0x00) return 0;
    while (at < len && p[at] == 0x00) at++;
    return at >= len || real_opcode(p[at]);
}

static inline uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static inline uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

static void bp_write(uint32_t v) {
    uint32_t reg = v >> 24, val = v & 0xFFFFFF;
    uint32_t mask = rt_gx.bp_mask;
    if ((reg & 0xF8) == 0xE0 && (val & 0x800000)) {
        /* konst colour write: shares the TEV register address, keep separate */
        rt_gx.bp_mask = 0xFFFFFF;
        if (rt_gx_backend && rt_gx_backend->bp_changed) rt_gx_backend->bp_changed(reg, val);
        return;
    }
    if (reg != 0xFE) {
        rt_gx.bp[reg] = (rt_gx.bp[reg] & ~mask) | (val & mask);
        rt_gx.bp_mask = 0xFFFFFF;
    } else {
        rt_gx.bp_mask = val;
        return;
    }
    if (reg == 0x45) rt_pe_signal_finish();                   /* PE_DONE: GXDrawDone / GXSetDrawDone */
    if (reg == 0x47) rt_pe_signal_token((uint16_t)val, 0);    /* PE_TOKEN */
    if (reg == 0x48) rt_pe_signal_token((uint16_t)val, 1);    /* PE_TOKEN_INT: GXSetDrawSync */
    if (reg == 0x52) {                       /* PE copy execute */
        uint32_t src_xy = rt_gx.bp[0x49], src_wh = rt_gx.bp[0x4A];
        uint32_t w = (src_wh & 0x3FF) + 1, h = ((src_wh >> 10) & 0x3FF) + 1;
        static int seq_left = -1; static uint64_t seq_last;
        if (seq_left < 0) seq_left = getenv("RT_GX_SEQ") ? atoi(getenv("RT_GX_SEQ")) : 0;
        if (seq_left > 0 && !rt_gx_skip_draws) {
            seq_left--;
            rt_log(0, "seq: %u draws, then %s copy src %u,%u %ux%u -> %08X fmt %X clear %u",
                   (unsigned)(rt_gx.draws - seq_last), (val & (1u << 14)) ? "DISPLAY" : "texture",
                   src_xy & 0x3FF, (src_xy >> 10) & 0x3FF, w, h, rt_gx.bp[0x4B] << 5, (val >> 4) & 7 | ((val >> 3) & 1) << 3, (val >> 11) & 1);
            seq_last = rt_gx.draws;
        }
        (void)src_xy;
        if (val & (1u << 14)) {              /* display copy (EFB -> XFB) */
            uint32_t dest = rt_gx.bp[0x4B] << 5;
            rt_gx.efb_copies++;
            if (rt_gx_backend && rt_gx_backend->copy_display) rt_gx_backend->copy_display(dest, w, h, val);
            rt_gx_copy_to_xfb(dest, w, h);
        } else {
            rt_gx.tex_copies++;
            if (rt_gx_backend && rt_gx_backend->copy_texture)
                rt_gx_backend->copy_texture(rt_gx.bp[0x4B] << 5, w, h, val);
        }
    }
    if (rt_gx_backend && rt_gx_backend->bp_changed) rt_gx_backend->bp_changed(reg, rt_gx.bp[reg]);
}

static void xf_write(uint32_t addr, uint32_t v) {
    if (addr < 0x1000) rt_gx.xf_mem[addr] = v;
    else if (addr >= 0x1000 && addr < 0x1100) rt_gx.xf_reg[addr - 0x1000] = v;
}

static int run_commands(const uint8_t *p, uint32_t len, int from_display_list);

static void call_display_list(uint32_t addr, uint32_t size) {
    static int depth;
    if (depth > 4 || size > 0x400000) return;
    depth++;
    run_commands(ppc_host(rt_base, addr), size, 1);
    depth--;
}

/* Consumes complete commands from p; returns bytes consumed. */
static int run_commands(const uint8_t *p, uint32_t len, int from_dl) {
    uint32_t i = 0;
    while (i < len) {
        uint8_t cmd = p[i];
        uint32_t need = 1;
        if (cmd == 0x00 || cmd == 0x48) { i++; continue; }
        if (cmd == 0x08) {
            need = 6; if (i + need > len) break;
            uint8_t reg = p[i + 1];
            rt_gx.cp[reg] = rd32(p + i + 2);
        } else if (cmd == 0x10) {
            if (i + 5 > len) break;
            uint32_t count = rd16(p + i + 1) + 1u, addr = rd16(p + i + 3);
            need = 5 + count * 4; if (i + need > len) break;
            for (uint32_t k = 0; k < count; k++) xf_write(addr + k, rd32(p + i + 5 + k * 4));
            if (rt_gx_backend && rt_gx_backend->xf_changed) rt_gx_backend->xf_changed(addr, count);
        } else if (cmd >= 0x20 && cmd <= 0x38 && (cmd & 7) == 0) {
            need = 5; if (i + need > len) break;
            uint32_t v = rd32(p + i + 1);
            /* indexed XF load: index[31:16], size-1 [15:12], addr[11:0] from array 12..15 */
            int arr = 12 + ((cmd - 0x20) >> 3);
            uint32_t idx = v >> 16, n = ((v >> 12) & 0xF) + 1, xa = v & 0xFFF;
            uint32_t src = rt_gx.cp[0xA0 + arr] + idx * rt_gx.cp[0xB0 + arr];
            for (uint32_t k = 0; k < n; k++) xf_write(xa + k, ld32(rt_base, 0x80000000u | (src + k * 4)));
        } else if (cmd == 0x40 || cmd == 0x44) {
            need = 9; if (i + need > len) break;
            if (!from_dl) {
                rt_log(3, "gx: call display list %08X size %u", rd32(p + i + 1), rd32(p + i + 5));
                call_display_list(0x80000000u | rd32(p + i + 1), rd32(p + i + 5));
            }
        } else if (cmd == 0x61) {
            need = 5; if (i + need > len) break;
            bp_write(rd32(p + i + 1));
        } else if (cmd >= 0x80 && cmd < 0xC0) {
            if (i + 3 > len) break;
            int vat = cmd & 7;
            uint32_t count = rd16(p + i + 1);
            VtxParts vp = vertex_parts(vat);
            /* Candidate vertex sizes: as described by VCD/VAT first; if the
             * command after the draw would not be a valid opcode, the game's
             * descriptor and data disagree (seen with debug drawing), so try
             * the size without texcoords / normals / colours and resync. */
            uint32_t cands[4] = { vp.base + vp.nrm + vp.clr + vp.tex, vp.base + vp.nrm + vp.clr,
                                  vp.base + vp.clr, vp.base };
            uint32_t vsz = cands[0];
            uint32_t end0 = i + 3 + count * cands[0];
            if (end0 > len) break;                                     /* wait for the whole draw */
            if (!command_starts_at(p, end0, len)) {
                for (int k = 1; k < 4; k++) {
                    uint32_t end = i + 3 + count * cands[k];
                    if (cands[k] && cands[k] != cands[0] && end < len && real_opcode(p[end]) &&
                        command_starts_at(p, end, len)) { vsz = cands[k]; break; }
                }
            }
            if (vsz != cands[0]) {
                rt_gx.resyncs++;
                if (rt_gx.resyncs < 4) rt_log(2, "gx: draw %02X x%u: data uses %u-byte vertices, descriptor says %u", cmd, count, vsz, cands[0]);
            }
            need = 3 + count * vsz; if (i + need > len) break;
            rt_gx.draws++;
            rt_gx.vertices += count;
            rt_stats.gx_draws++;
            if (rt_gx_backend && rt_gx_backend->draw && !rt_gx_skip_draws)
                rt_gx_backend->draw((cmd & 0xF8), vat, count, p + i + 3, vsz);
        } else {
            /* unknown opcode: GX would hang; resynchronise by dropping one byte */
            rt_gx.bad_opcodes++;
            if (rt_gx.bad_opcodes < 4) {
                rt_log(1, "gx: unknown opcode %02X (%s, offset %u of %u) VCD %X/%X VAT0 %X/%X/%X", cmd,
                       from_dl ? "display list" : "fifo", i, len, rt_gx.cp[0x50], rt_gx.cp[0x60],
                       rt_gx.cp[0x70], rt_gx.cp[0x80], rt_gx.cp[0x90]);
                char hex[200]; int k = 0;
                for (uint32_t q = i > 24 ? i - 24 : 0; q < i + 16 && q < len && k < 190; q++) k += snprintf(hex + k, sizeof hex - k, "%02X ", p[q]);
                rt_log(1, "gx:   %s", hex);
            }
            need = 1;
        }
        i += need;
    }
    return (int)i;
}

#include <stdlib.h>
static FILE *g_dump; static int g_dump_init;
static void dump_bytes(const uint8_t *p, uint32_t n) {
    if (!g_dump_init) { g_dump_init = 1; const char *e = getenv("RT_GX_DUMP"); if (e) g_dump = fopen(e, "wb"); }
    if (g_dump) { fwrite(p, 1, n, g_dump); fflush(g_dump); }
}

static void pump(void) {
    int used = run_commands(g_buf, g_len, 0);
    if (used > 0) { memmove(g_buf, g_buf + used, g_len - (uint32_t)used); g_len -= (uint32_t)used; }
    if (g_len > sizeof g_buf - 64) { rt_log(1, "gx: FIFO overflow, dropping"); g_len = 0; }
}

/* ---------------------------------------------------------------------
 * Asynchronous GPU: the CPU thread appends to a single-producer /
 * single-consumer ring; the GPU thread decodes and draws. Synchronisation is
 * the game's own: GXDrawDone / draw-sync tokens are raised by the GPU thread
 * when it reaches them, exactly like the real command processor.
 * ------------------------------------------------------------------- */
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>
#define RING_SIZE (8u << 20)
static uint8_t *g_ring;
static _Atomic uint32_t g_ring_head, g_ring_tail;     /* head: producer, tail: consumer */
static int g_async;

static void gpu_consume(void) {
    uint32_t tail = atomic_load_explicit(&g_ring_tail, memory_order_relaxed);
    uint32_t head = atomic_load_explicit(&g_ring_head, memory_order_acquire);
    while (tail != head) {
        uint32_t room = (uint32_t)sizeof g_buf - 64 - g_len;
        uint32_t avail = head - tail;
        uint32_t n = avail < room ? avail : room;
        uint32_t off = tail % RING_SIZE;
        if (n > RING_SIZE - off) n = RING_SIZE - off;
        memcpy(g_buf + g_len, g_ring + off, n);
        g_len += n;
        tail += n;
        atomic_store_explicit(&g_ring_tail, tail, memory_order_release);
        pump();
        head = atomic_load_explicit(&g_ring_head, memory_order_acquire);
    }
}

static _Atomic int g_gpu_busy;
static void *gpu_thread(void *arg) {
    (void)arg;
    for (;;) {
        uint32_t tail = atomic_load_explicit(&g_ring_tail, memory_order_relaxed);
        if (tail == atomic_load_explicit(&g_ring_head, memory_order_acquire)) { usleep(50); continue; }
        atomic_store(&g_gpu_busy, 1);
        gpu_consume();
        atomic_store(&g_gpu_busy, 0);
    }
    return NULL;
}

/* wait until everything written to the FIFO has been processed */
void rt_gx_idle(void) {
    if (!g_async) return;
    while (atomic_load(&g_ring_tail) != atomic_load(&g_ring_head) || atomic_load(&g_gpu_busy)) usleep(100);
}

void rt_gx_ss_register(void) {
    rt_ss_add(&rt_gx, sizeof rt_gx);
    rt_ss_add(g_buf, sizeof g_buf);
    rt_ss_add(&g_len, sizeof g_len);
}

static inline void ring_put(const uint8_t *b, uint32_t n) {
    uint32_t head = atomic_load_explicit(&g_ring_head, memory_order_relaxed);
    while (head + n - atomic_load_explicit(&g_ring_tail, memory_order_acquire) > RING_SIZE) sched_yield();
    for (uint32_t k = 0; k < n; k++) g_ring[(head + k) % RING_SIZE] = b[k];
    atomic_store_explicit(&g_ring_head, head + n, memory_order_release);
}

static inline void fifo_put(const uint8_t *b, uint32_t n) {
    if (g_async) { ring_put(b, n); return; }
    memcpy(g_buf + g_len, b, n);
    g_len += n;
    if (g_len >= 32) pump();
}

void rt_gx_write8(uint8_t v) { rt_stats.gx_fifo_bytes++; dump_bytes(&v, 1); fifo_put(&v, 1); }
void rt_gx_write16(uint16_t v) { rt_stats.gx_fifo_bytes += 2; uint8_t b[2] = { v >> 8, (uint8_t)v }; dump_bytes(b, 2); fifo_put(b, 2); }
void rt_gx_write32(uint32_t v) {
    rt_stats.gx_fifo_bytes += 4;
    uint8_t b[4] = { v >> 24, v >> 16, v >> 8, (uint8_t)v };
    dump_bytes(b, 4);
    fifo_put(b, 4);
}

void rt_gx_flush(void) { if (!g_async) pump(); }

void rt_gx_start_async(void) {
    if (g_async) return;
    g_ring = malloc(RING_SIZE);
    g_async = 1;
    pthread_t t;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 8u << 20);
    pthread_create(&t, &at, gpu_thread, NULL);
    pthread_attr_destroy(&at);
    rt_log(1, "renderer: asynchronous GPU thread");
}

void rt_gx_init(void) {
    memset(&rt_gx, 0, sizeof rt_gx);
    rt_gx.bp_mask = 0xFFFFFF;
    g_len = 0;
}
