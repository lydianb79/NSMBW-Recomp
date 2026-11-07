/*
 * mmio.c - Hollywood/Flipper hardware registers seen by the game.
 *
 *   0xCC000000 CP  command processor      0xCC001000 PE  pixel engine
 *   0xCC002000 VI  video interface        0xCC003000 PI  processor interface
 *   0xCC004000 MI  memory interface       0xCC005000 DSP / ARAM
 *   0xCC008000 WGPIPE (GX FIFO write-gather pipe)
 *   0xCD000000 Hollywood: IPC (IOS), IRQ, GPIO      0xCD006000 DI / SI / EXI / AI
 *
 * Registers default to plain storage (big-endian byte arrays); the ones that
 * matter for booting and presenting have explicit behaviour.
 */
#include <stdio.h>
#include <string.h>

#include "rt_internal.h"

static uint8_t g_cc[0x10000];   /* 0xCC00xxxx */
static uint8_t g_cd[0x10000];   /* 0xCD00xxxx / 0xCD80xxxx */
static uint8_t g_seen[0x20000 / 8];
extern uint32_t rt_last_mmio_addr, rt_last_mmio_value;

uint32_t rt_vi_xfb_addr, rt_vi_width = 640, rt_vi_height = 528, rt_vi_stride = 1280;
uint32_t rt_pi_intsr, rt_pi_intmr;

static inline uint8_t *reg_ptr(uint32_t ea) {
    return ((ea >> 24) == 0xCC ? g_cc : g_cd) + (ea & 0xFFFF);
}
static inline uint32_t rd(uint32_t ea, int size) {
    uint8_t *p = reg_ptr(ea);
    return size == 4 ? (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]
         : size == 2 ? (uint32_t)p[0] << 8 | p[1] : p[0];
}
static inline void wr(uint32_t ea, uint32_t v, int size) {
    uint8_t *p = reg_ptr(ea);
    if (size == 4) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
    else if (size == 2) { p[0] = v >> 8; p[1] = v; }
    else p[0] = v;
}

static void note(uint32_t ea, const char *what, uint32_t v) {
    uint32_t k = (((ea >> 24) == 0xCD) << 16) | (ea & 0xFFFF);
    if (!(g_seen[k >> 3] & (1 << (k & 7)))) {
        g_seen[k >> 3] |= 1 << (k & 7);
        rt_log(3, "mmio %s %08X = %08X (first access)", what, ea, v);
    }
}

void rt_mmio_init(void) {
    memset(g_cc, 0, sizeof g_cc);
    memset(g_cd, 0, sizeof g_cd);
    wr(0xCC00302C, 0x246500B1, 4);   /* PI: Hollywood revision / reset status */
    wr(0xCC002000, 0x0F06 | (264 << 4), 2);  /* VI VTR default PAL */
    wr(0xCD000214, 0x00000000, 4);
    wr(0xCD0005A0, 0x00000000, 4);
}

/* ------------------------------------------------------------------ VI */
static void vi_update_geometry(void) {
    uint32_t tfbl = rd(0xCC00201C, 4);
    uint32_t addr = tfbl & 0x00FFFFFF;
    if (tfbl & (1u << 28)) addr <<= 5;
    rt_vi_xfb_addr = addr;
    uint32_t picconf = rd(0xCC002048, 2);
    uint32_t wpl = (picconf >> 8) & 0x7F, std = picconf & 0xFF;
    if (wpl) rt_vi_width = wpl * 16;           /* 16 pixels (32 bytes) per WPL unit */
    if (std) rt_vi_stride = std * 32;
    uint32_t vtr = rd(0xCC002000, 2);
    uint32_t acv = (vtr >> 4) & 0x3FF;
    uint32_t dcr = rd(0xCC002002, 2);
    int progressive = (dcr >> 2) & 1;
    if (acv) rt_vi_height = progressive ? acv : acv * 2;
}

static uint32_t vi_read(uint32_t ea, int size) {
    uint32_t off = ea & 0xFFF;
    if (off == 0x2C || off == 0x2E) {
        /* beam position: advance on every read so polling loops make progress */
        static uint32_t line;
        line = (line + 1) % 625;
        if (off == 0x2C) return size == 4 ? (line + 1) << 16 | 1 : line + 1;
        return 1;
    }
    return rd(ea, size);
}

static void vi_write(uint32_t ea, uint32_t v, int size) {
    uint32_t off = ea & 0xFFF;
    if ((off == 0x30 || off == 0x34 || off == 0x38 || off == 0x3C) && size == 2 && !(v & 0x8000)) {
        /* writing 0 to the status bit (bit 15 of the high half) acknowledges the interrupt */
        uint32_t cur = rd(ea, 2);
        wr(ea, (cur & ~0x8000u) | (v & 0x7FFF), 2);
        return;
    }
    if ((off == 0x30 || off == 0x34 || off == 0x38 || off == 0x3C) && size == 4) {
        uint32_t cur = rd(ea, 4);
        if (!(v & 0x80000000u)) cur &= ~0x80000000u;
        wr(ea, (cur & 0x80000000u) | (v & 0x7FFFFFFFu), 4);
        return;
    }
    wr(ea, v, size);
    if ((off >= 0x1C && off < 0x2C) || off == 0x48 || off == 0x4A || off == 0x00 || off == 0x02)
        vi_update_geometry();
}

/* Called by the frame pacer once per field: raise VI DI0 if enabled. */
void rt_vi_retrace_irq(void) {
    uint32_t di0 = rd(0xCC002030, 4);
    if (di0 & (1u << 28)) {
        wr(0xCC002030, di0 | 0x80000000u, 4);
        ppc_irq_pending = 1;     /* level: PI VI cause stays up until DI0 is acknowledged */
    }
    rt_stats.vi_frames++;
    rt_video_vi_retrace();
}

/* ------------------------------------------------------------------ PI */
/* PI interrupt causes are level signals from the devices; they clear when the
 * device-side condition is acknowledged (VI DIx status, IPC Y1/Y2, DSP CSR). */
extern int rt_ios_irq_line(void);
uint32_t rt_pi_cause(void) {
    uint32_t c = rt_pi_intsr;
    c &= ~((1u << 8) | (1u << 14) | (1u << 6) | (1u << 9) | (1u << 10));
    for (int i = 0; i < 4; i++) {
        uint32_t di = rd(0xCC002030 + 4 * i, 4);
        if ((di & 0x80000000u) && (di & (1u << 28))) c |= 1u << 8;          /* VI */
    }
    if (rt_ios_irq_line()) c |= 1u << 14;                                   /* Hollywood IPC */
    uint32_t pe = rd(0xCC00100A, 2);
    if ((pe & 0x4) && (pe & 0x1)) c |= 1u << 9;                             /* PE token */
    if ((pe & 0x8) && (pe & 0x2)) c |= 1u << 10;                            /* PE finish */
    uint32_t csr = rd(0xCC00500A, 2);
    if (((csr & 0x08) && (csr & 0x10)) || ((csr & 0x20) && (csr & 0x40)) || ((csr & 0x80) && (csr & 0x100)))
        c |= 1u << 6;                                                       /* DSP / ARAM / AI */
    return c;
}

static uint32_t pi_read(uint32_t ea, int size) {
    uint32_t off = ea & 0xFFF;
    /* bit 16 RSWST is the console's reset button: 1 = released. Reading it as 0
     * made the game see a held Reset button and soft-reset as soon as resets
     * became allowed (right after the player-select screen). */
    if (off == 0x00) return rt_pi_cause() | 0x00010000u;
    if (off == 0x04) return rt_pi_intmr;
    return rd(ea, size);
}
static void pi_write(uint32_t ea, uint32_t v, int size) {
    uint32_t off = ea & 0xFFF;
    if (off == 0x00) { rt_pi_intsr &= ~v; return; }   /* write 1 to clear */
    if (off == 0x04) { rt_pi_intmr = v; return; }
    if (off == 0x24) { rt_log(1, "PI reset register written (%08X) - game requested reset", v); }
    wr(ea, v, size);
}

/* ------------------------------------------------------------------ DSP */
/* DSP: only the boot handshake of __OSInitAudioSystem and ARAM DMA status
 * matter until AX is HLE'd. Offsets: 0x00/0x02 CPU->DSP mailbox,
 * 0x04/0x06 DSP->CPU mailbox, 0x0A CSR, 0x20.. ARAM DMA. */
static int g_dsp_mail_full = 1;               /* DSP ROM greets with 0x8071FEED after reset */
static uint32_t g_dsp_mail = 0x8071FEED;

static uint32_t dsp_read(uint32_t ea, int size) {
    uint32_t off = ea & 0xFFF;
    switch (off) {
    case 0x000: return rd(ea, 2) & 0x7FFF;            /* our mail is always consumed at once */
    case 0x002: return rd(ea, 2);
    case 0x004: return (g_dsp_mail_full ? 0x8000u : 0) | ((g_dsp_mail >> 16) & 0x7FFF);
    case 0x006: g_dsp_mail_full = 0; return g_dsp_mail & 0xFFFF;   /* reading the low half pops the mail */
    case 0x00A: return rd(ea, 2) & ~(0x0001u | 0x0200u | 0x0400u);  /* reset done, no DMA busy */
    case 0x016: return 1;                             /* ARAM mode: ready */
    case 0x02A: case 0x03A: return 0;                 /* DMA byte counts: complete */
    default: return rd(ea, size);
    }
}

static void dsp_write(uint32_t ea, uint32_t v, int size) {
    uint32_t off = ea & 0xFFF;
    if (off == 0x00A && size == 2) {
        /* writing 1 to an interrupt status bit acknowledges it; RES completes immediately */
        uint32_t cur = rd(ea, 2);
        uint32_t ack = v & (0x0008u | 0x0020u | 0x0080u);
        if ((v & 0x0001u) || ((cur & 0x0004u) && !(v & 0x0004u))) {
            g_dsp_mail = 0x8071FEED; g_dsp_mail_full = 1;     /* reset / un-halt -> DSP ROM greeting */
        }
        wr(ea, ((cur & ~ack) & 0x00A8u) | (v & ~0x00A8u & ~0x0801u), 2);
        return;
    }
    wr(ea, v, size);
    if ((off == 0x028 && size == 4) || off == 0x02A) {
        /* ARAM DMA started: the Wii has no ARAM, so it just completes and raises ARINT */
        uint32_t csr = rd(0xCC00500A, 2) | 0x0020u;
        wr(0xCC00500A, csr, 2);
        if (csr & 0x0040u) ppc_irq_pending = 1;          /* ARINT enabled -> PI DSP cause */
    }
}

/* ------------------------------------------------------------------ PE */
/* Pixel engine interrupts, raised by the GX FIFO decoder when it sees
 * BP 0x45 (draw done -> FINISH) or BP 0x48 (token + interrupt -> TOKEN).
 * PE_INTRCTRL 0xCC00100A: bit0 token enable, bit1 finish enable,
 *                          bit2 token status, bit3 finish status (write 1 = ack). */
void rt_pe_signal_finish(void) {
    rt_log(3, "pe: draw done (intrctrl %04X)", rd(0xCC00100A, 2));
    wr(0xCC00100A, rd(0xCC00100A, 2) | 0x8, 2);
    ppc_irq_pending = 1;
}
void rt_pe_signal_token(uint16_t token, int irq) {
    wr(0xCC00100E, token, 2);
    if (irq) { wr(0xCC00100A, rd(0xCC00100A, 2) | 0x4, 2); ppc_irq_pending = 1; }
}
static void pe_write(uint32_t ea, uint32_t v, int size) {
    rt_log(3, "pe: write %08X = %04X (%d)", ea, v, size);
    if ((ea & 0xFFF) == 0x00A) {
        uint32_t cur = rd(ea, 2);
        uint32_t st = cur & 0xC & ~(v & 0xC);          /* write-1-to-clear status */
        wr(ea, st | (v & 0x3), 2);
        return;
    }
    wr(ea, v, size);
}

/* AI DMA: while enabled (DSP 0x36 bit 15) the audio interface pulls one
 * buffer of len*32 bytes (16-bit stereo, 32 kHz) and raises AIDINT (CSR bit 3)
 * when done. A host timer thread marks a buffer as finished; the flag is
 * applied on the CPU thread so CSR read-modify-writes never race. */
volatile int rt_ai_dma_due;
uint32_t rt_ai_dma_period_us(void) {
    uint32_t ctl = rd(0xCC005036, 2);
    uint32_t blocks = ctl & 0x7FFF;
    if (!(ctl & 0x8000) || !blocks) return 0;
    return blocks * 32u / 4u * 1000000u / 32000u;   /* 0x180 bytes -> 3000 us */
}
void rt_ai_dma_apply(void) {
    if (!rt_ai_dma_due) return;
    rt_ai_dma_due = 0;
    if (rd(0xCC005036, 2) & 0x8000) wr(0xCC00500A, rd(0xCC00500A, 2) | 0x0008u, 2);
}

/* ------------------------------------------------------------------ AI */
/* Audio interface sample counter: advances at 48 kHz (32 kHz if AFR=0)
 * while playback (AI_CR PSTAT) is enabled; written values reset it. */
static uint64_t g_ai_base_tb;
static uint32_t g_ai_base_count;
static uint32_t ai_sample_counter(void) {
    uint32_t cr = rd(0xCD006C00, 4);
    if (!(cr & 1)) return g_ai_base_count;
    uint64_t ticks = rt_timebase() - g_ai_base_tb;              /* 60.75 MHz */
    uint32_t rate = (cr & 0x40) ? 48000 : 32000;
    return g_ai_base_count + (uint32_t)(ticks * rate / 60750000ull);
}
static void ai_write(uint32_t ea, uint32_t v, int size) {
    uint32_t off = ea & 0xFF;
    if (off == 0x00) {                                          /* AI_CR */
        uint32_t old = rd(ea, 4);
        if ((v & 1) != (old & 1)) { g_ai_base_count = ai_sample_counter(); g_ai_base_tb = rt_timebase(); }
        if (v & 0x20) { g_ai_base_count = 0; g_ai_base_tb = rt_timebase(); v &= ~0x20u; }   /* SCRESET */
        wr(ea, v & ~0x8u, 4);                                   /* AIINT ack */
        return;
    }
    if (off == 0x08) { g_ai_base_count = v; g_ai_base_tb = rt_timebase(); }
    wr(ea, v, size);
}

/* ------------------------------------------------------------------ dispatch */
/* ------------------------------------------------------------------ FIFO */
/* CPU side: the write-gather pipe appends to memory at PI_FIFO_WPTR within
 * [PI_FIFO_BASE, PI_FIFO_END]. GPU side: the command processor executes the
 * bytes only while the CPU FIFO is the GP FIFO (same base) and CP reads are
 * enabled; otherwise the game is recording a display list into memory, which
 * it later replays with GX "call display list". */
#define PI_FIFO_BASE 0xCC00300C
#define PI_FIFO_END  0xCC003010
#define PI_FIFO_WPTR 0xCC003014
static uint32_t cp_fifo_base(void) { return rd(0xCC000020, 2) | (rd(0xCC000022, 2) << 16); }
static int gp_linked(void) {
    uint32_t pib = rd(PI_FIFO_BASE, 4) & 0x1FFFFFE0u;
    return pib && pib == (cp_fifo_base() & 0x1FFFFFE0u) && (rd(0xCC000002, 2) & 1);
}
static void wgpipe_write(uint32_t v, int size) {
    uint32_t base = rd(PI_FIFO_BASE, 4) & 0x1FFFFFE0u, end = rd(PI_FIFO_END, 4) & 0x1FFFFFE0u;
    uint32_t w = rd(PI_FIFO_WPTR, 4);
    uint32_t wp = w & 0x1FFFFFFFu, wrap = w & 0x20000000u;
    if (base && end > base) {
        for (int k = size - 1; k >= 0; k--) {
            *ppc_host(rt_base, wp | 0x80000000u) = (uint8_t)(v >> (8 * k));
            if (++wp >= end + 32) { wp = base; wrap = 0x20000000u; }
        }
        wr(PI_FIFO_WPTR, wp | wrap, 4);
    }
    if (!base || gp_linked()) {
        if (size == 4) rt_gx_write32(v); else if (size == 2) rt_gx_write16((uint16_t)v); else rt_gx_write8((uint8_t)v);
    }
}

/* CP status as the GX library sees it: the GPU is infinitely fast, so the GP
 * FIFO is always drained (read pointer == write pointer, count 0, idle). */
static uint32_t cp_read(uint32_t ea, int size) {
    uint32_t off = ea & 0xFFF;
    switch (off) {
    case 0x00: return (rd(ea, 2) & ~0x3u) | 0x0C;          /* SR: idle, cmd idle, no over/underflow */
    case 0x38: case 0x3A: return rd(0xCC000034 + (off - 0x38), 2);   /* RW distance */
    case 0x30: return 0;                                   /* FIFO RW distance lo */
    case 0x32: return 0;
    case 0x34: case 0x36: return rd(ea, 2);                 /* write pointer */
    default: return rd(ea, size);
    }
}

uint32_t mmio_read(uint32_t ea, int size) {
    rt_stats.mmio_reads++;
    uint32_t v;
    uint32_t blk = ea & 0xFFFFF000u;
    if ((ea >> 24) == 0xCD) {
        uint32_t off = ea & 0xFFFF;
        if (off < 0x10) v = rt_ios_ipc_read(off);
        else if (off >= 0x6400 && off < 0x6800 && (off & 0xFF) == 0x38) v = 0;   /* SI status: idle */
        else if (off >= 0x6800 && off < 0x6C00) v = rd(ea, size) & ~1u;          /* EXI: transfer done */
        else if (off == 0x6C08) v = ai_sample_counter();                          /* AI: samples played */
        else v = rd(ea, size);
    } else if (blk == 0xCC002000) v = vi_read(ea, size);
    else if (blk == 0xCC000000) v = cp_read(ea, size);
    else if (blk == 0xCC003000) v = pi_read(ea, size);
    else if (blk == 0xCC005000) v = dsp_read(ea, size);
    else v = rd(ea, size);
    note(ea, "R", v);
    rt_last_mmio_addr = ea; rt_last_mmio_value = v;
    return v;
}

void mmio_write(uint32_t ea, uint32_t v, int size) {
    rt_stats.mmio_writes++;
    uint32_t blk = ea & 0xFFFFF000u;
    if (blk == 0xCC008000) { wgpipe_write(v, size); return; }
    note(ea, "W", v);
    rt_last_mmio_addr = ea; rt_last_mmio_value = v;
    if ((ea >> 24) == 0xCD) {
        uint32_t off = ea & 0xFFFF;
        if (off < 0x10) { rt_ios_ipc_write(off, v); return; }
        if (off >= 0x6C00 && off < 0x6C20) { ai_write(ea, v, size); return; }
        if (off >= 0x6800 && off < 0x6C00) {     /* EXI: complete transfers instantly */
            wr(ea, ((ea & 0xF) == 0xC) ? (v & ~1u) : v, size);
            return;
        }
        wr(ea, v, size);
        return;
    }
    if (blk == 0xCC002000) vi_write(ea, v, size);
    else if (blk == 0xCC003000) pi_write(ea, v, size);
    else if (blk == 0xCC005000) dsp_write(ea, v, size);
    else if (blk == 0xCC001000) pe_write(ea, v, size);
    else wr(ea, v, size);
}

uint32_t ppc_mmio_read32(uint32_t ea) { return mmio_read(ea, 4); }
uint16_t ppc_mmio_read16(uint32_t ea) { return (uint16_t)mmio_read(ea, 2); }
uint8_t  ppc_mmio_read8(uint32_t ea)  { return (uint8_t)mmio_read(ea, 1); }
void ppc_mmio_write32(uint32_t ea, uint32_t v) { mmio_write(ea, v, 4); }
void ppc_mmio_write16(uint32_t ea, uint16_t v) { mmio_write(ea, v, 2); }
void ppc_mmio_write8(uint32_t ea, uint8_t v)   { mmio_write(ea, v, 1); }
