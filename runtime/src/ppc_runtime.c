/*
 * ppc_runtime.c - helpers called by recompiled code: indirect-call dispatch,
 * special-purpose registers, paired-single quantisation, FPU edge cases.
 */
#include <fenv.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "rt.h"
#include "rt_internal.h"

extern const PPCFuncEntry ppc_func_table[];
extern const uint32_t ppc_func_count;

/* ---------------------------------------------------------------------
 * Dispatch: direct-mapped lookup tables, one per executable region.
 * ------------------------------------------------------------------- */
typedef struct { uint32_t lo, hi; PPCFunc *slots; } DispatchRegion;
static DispatchRegion g_regions[8];
static int g_nregions;

void rt_dispatch_init(void) {
    /* group functions into regions separated by gaps > 1 MiB */
    uint32_t lo = ppc_func_table[0].addr, prev = lo;
    for (uint32_t i = 1; i <= ppc_func_count; i++) {
        uint32_t a = i < ppc_func_count ? ppc_func_table[i].addr : 0xFFFFFFFFu;
        if (a - prev > 0x100000 || i == ppc_func_count) {
            DispatchRegion *r = &g_regions[g_nregions++];
            r->lo = lo;
            r->hi = prev + 4;
            r->slots = calloc((r->hi - r->lo) / 4, sizeof(PPCFunc));
            lo = a;
        }
        prev = a;
    }
    for (uint32_t i = 0; i < ppc_func_count; i++) {
        uint32_t a = ppc_func_table[i].addr;
        for (int k = 0; k < g_nregions; k++)
            if (a >= g_regions[k].lo && a < g_regions[k].hi)
                g_regions[k].slots[(a - g_regions[k].lo) / 4] = ppc_func_table[i].fn;
    }
    rt_log(1, "dispatch: %u functions in %d regions", ppc_func_count, g_nregions);
}

PPCFunc rt_lookup(uint32_t addr) {
    addr &= ~0x40000000u;              /* 0xC... uncached mirror of code */
    for (int k = 0; k < g_nregions; k++) {
        DispatchRegion *r = &g_regions[k];
        if (addr >= r->lo && addr < r->hi && !(addr & 3)) return r->slots[(addr - r->lo) / 4];
    }
    return NULL;
}

void ppc_call(PPCContext *ctx, uint8_t *base, uint32_t addr) {
    rt_stats.indirect_calls++;
    PPCFunc fn = rt_lookup(addr);
    if (PPC_LIKELY(fn != NULL)) { fn(ctx, base); return; }
    rt_stats.unknown_calls++;
    rt_fatal(ctx, "call to unknown function 0x%08X (lr=0x%08X)", addr, ctx->lr);
}

void ppc_fallthrough(PPCContext *ctx, uint32_t addr) {
    rt_fatal(ctx, "fell through into non-function address 0x%08X", addr);
}

void ppc_illegal(PPCContext *ctx, uint32_t pc, uint32_t word) {
    rt_stats.illegal++;
    rt_fatal(ctx, "illegal instruction 0x%08X at 0x%08X", word, pc);
}

void ppc_tw(PPCContext *ctx, uint32_t pc, int to, uint32_t a, uint32_t b) {
    int32_t sa = (int32_t)a, sb = (int32_t)b;
    int trap = ((to & 16) && sa < sb) || ((to & 8) && sa > sb) || ((to & 4) && a == b) ||
               ((to & 2) && a < b) || ((to & 1) && a > b);
    if (trap) rt_fatal(ctx, "trap at 0x%08X", pc);
}

void ppc_syscall(PPCContext *ctx, uint8_t *base, uint32_t pc) {
    (void)base;
    /* the SDK only uses `sc` for the ICache-flush syscall (0x00000C00 handler) */
    rt_log(3, "sc at 0x%08X (r0=%u) ignored", pc, ctx->r[0]);
}

void ppc_rfi(PPCContext *ctx, uint8_t *base) {
    /* Outside the thread/interrupt HLE the SDK uses rfi in two ways:
     *  - as a return that also switches MSR (SRR0 == LR), e.g. BAT setup;
     *  - as a jump into a function in real mode (RealMode(fn)).
     * Anything else means an OSLoadContext-style path is missing an HLE. */
    uint32_t target = ctx->spr[26];
    ctx->msr = ctx->spr[27] | 0x2000 | 0x30;     /* keep FP on, MMU on (we never translate) */
    if (target == ctx->lr) return;
    PPCFunc fn = rt_lookup(target | 0x80000000u);
    if (fn) { fn(ctx, base); return; }
    rt_fatal(ctx, "rfi to 0x%08X reached (missing thread/exception HLE)", target);
}

/* ---------------------------------------------------------------------
 * SPRs
 * ------------------------------------------------------------------- */
enum { SPR_DEC = 22, SPR_SRR0 = 26, SPR_SRR1 = 27, SPR_TBL_W = 284, SPR_TBU_W = 285,
       SPR_HID0 = 1008, SPR_HID1 = 1009, SPR_HID2 = 920, SPR_WPAR = 921,
       SPR_DMAU = 922, SPR_DMAL = 923, SPR_L2CR = 1017, SPR_PVR = 287 };

static uint64_t host_ticks_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* Broadway timebase runs at bus clock / 4 = 243 MHz / 4 = 60.75 MHz */
static uint64_t g_tb_offset;
uint64_t rt_timebase(void) {
    static uint64_t t0;
    uint64_t now = host_ticks_ns();
    if (!t0) t0 = now;
    return (uint64_t)((double)(now - t0) * 0.06075) + g_tb_offset;
}

uint32_t ppc_mftb(PPCContext *ctx, int tbr) {
    (void)ctx;
    uint64_t tb = rt_timebase();
    return tbr == 269 ? (uint32_t)(tb >> 32) : (uint32_t)tb;
}

uint32_t ppc_mfspr(PPCContext *ctx, int spr) {
    switch (spr) {
    case 268: return (uint32_t)rt_timebase();
    case 269: return (uint32_t)(rt_timebase() >> 32);
    case SPR_PVR: return 0x00087102;            /* Broadway */
    case SPR_DEC: return 0x7FFFFFFF;
    default: return ctx->spr[spr & 1023];
    }
}

static void locked_cache_dma(PPCContext *ctx, uint8_t *base) {
    uint32_t dmau = ctx->spr[SPR_DMAU], dmal = ctx->spr[SPR_DMAL];
    uint32_t mem = dmau & ~0x1Fu, lc = dmal & ~0x1Fu;
    uint32_t blocks = ((dmau & 0x1F) << 2) | ((dmal >> 2) & 3);
    if (!blocks) blocks = 128;
    uint32_t len = blocks * 32;
    if (dmal & 0x10) memmove(ppc_host(base, lc), ppc_host(base, mem), len);   /* load into LC */
    else             memmove(ppc_host(base, mem), ppc_host(base, lc), len);   /* store from LC */
    ctx->spr[SPR_DMAL] &= ~3u;   /* clear trigger + flush: completes instantly */
}

void ppc_mtspr(PPCContext *ctx, uint8_t *base, int spr, uint32_t v) {
    ctx->spr[spr & 1023] = v;
    switch (spr) {
    case SPR_DMAL: if (v & 2) locked_cache_dma(ctx, base); break;
    case SPR_TBL_W: case SPR_TBU_W: break;  /* OS sets timebase at boot; ignore */
    case SPR_SRR0: case SPR_SRR1: break;
    default: break;
    }
}

void ppc_mtmsr(PPCContext *ctx, uint32_t v) {
    uint32_t old = ctx->msr;
    ctx->msr = v;
    if ((v & 0x8000) && !(old & 0x8000)) rt_interrupts_enabled(ctx);
}

/* ---------------------------------------------------------------------
 * Paired-single quantised load/store (GQR driven)
 * ------------------------------------------------------------------- */
static inline double ldexp_fast(double v, int e) { return ldexp(v, e); }

static float dequant(uint8_t *base, uint32_t ea, int type, int scale, int *size) {
    switch (type) {
    case 4: *size = 1; return (float)ldexp_fast((double)(uint8_t)ld8(base, ea), -scale);
    case 5: *size = 2; return (float)ldexp_fast((double)(uint16_t)ld16(base, ea), -scale);
    case 6: *size = 1; return (float)ldexp_fast((double)(int8_t)ld8(base, ea), -scale);
    case 7: *size = 2; return (float)ldexp_fast((double)(int16_t)ld16(base, ea), -scale);
    default: *size = 4; return ld32f(base, ea);
    }
}

static int sext6(uint32_t v) { v &= 0x3F; return (v & 0x20) ? (int)v - 64 : (int)v; }

void ppc_psq_load(PPCContext *ctx, uint8_t *base, uint32_t ea, int frd, int w, int i) {
    uint32_t gqr = ctx->gqr[i];
    int type = (gqr >> 16) & 7, scale = sext6(gqr >> 24), sz;
    double v0 = dequant(base, ea, type, scale, &sz);
    double v1 = w ? 1.0 : dequant(base, ea + sz, type, scale, &sz);
    ctx->f[frd].d = v0;
    ctx->ps1[frd] = v1;
}

static void quant(uint8_t *base, uint32_t ea, double v, int type, int scale, int *size) {
    if (type == 0) { *size = 4; st32f(base, ea, (float)v); return; }
    double s = ldexp(v, scale);
    switch (type) {
    case 4: *size = 1; st8(base, ea, (uint32_t)(s <= 0 ? 0 : s >= 255 ? 255 : s)); break;
    case 5: *size = 2; st16(base, ea, (uint32_t)(s <= 0 ? 0 : s >= 65535 ? 65535 : s)); break;
    case 6: *size = 1; st8(base, ea, (uint32_t)(int32_t)(s <= -128 ? -128 : s >= 127 ? 127 : s)); break;
    case 7: *size = 2; st16(base, ea, (uint32_t)(int32_t)(s <= -32768 ? -32768 : s >= 32767 ? 32767 : s)); break;
    default: *size = 4; st32f(base, ea, (float)v); break;
    }
}

void ppc_psq_store(PPCContext *ctx, uint8_t *base, uint32_t ea, int frs, int w, int i) {
    uint32_t gqr = ctx->gqr[i];
    int type = gqr & 7, scale = sext6(gqr >> 8), sz;
    quant(base, ea, ctx->f[frs].d, type, scale, &sz);
    if (!w) quant(base, ea + sz, ctx->ps1[frs], type, scale, &sz);
}

/* ---------------------------------------------------------------------
 * FPU
 * ------------------------------------------------------------------- */
static uint64_t fcti(double v, int truncate) {
    int32_t r;
    if (v != v) r = INT32_MIN;
    else if (v >= 2147483648.0) r = INT32_MAX;
    else if (v < -2147483648.0) r = INT32_MIN;
    else r = truncate ? (int32_t)v : (int32_t)nearbyint(v);
    return 0xFFF8000000000000ull | (uint32_t)r;
}
uint64_t ppc_fctiw(PPCContext *ctx, double v) { (void)ctx; return fcti(v, 0); }
uint64_t ppc_fctiwz(PPCContext *ctx, double v) { (void)ctx; return fcti(v, 1); }

void ppc_fpscr_changed(PPCContext *ctx) {
    static const int modes[4] = { FE_TONEAREST, FE_TOWARDZERO, FE_UPWARD, FE_DOWNWARD };
    fesetround(modes[ctx->fpscr & 3]);
}

void ppc_mtfsf(PPCContext *ctx, uint32_t fm, uint32_t v) {
    uint32_t m = 0;
    for (int k = 0; k < 8; k++)
        if (fm & (0x80u >> k)) m |= 0xF0000000u >> (4 * k);
    ctx->fpscr = (ctx->fpscr & ~m) | (v & m);
    ppc_fpscr_changed(ctx);
}

void ppc_mcrfs(PPCContext *ctx, int crfd, int crfs) {
    uint32_t nib = (ctx->fpscr >> (28 - 4 * crfs)) & 0xF;
    for (int b = 0; b < 4; b++) ctx->crb[crfd * 4 + b] = (nib >> (3 - b)) & 1;
    ctx->fpscr &= ~((0xFu << (28 - 4 * crfs)) & 0x9FF80700u);  /* clear exception bits only */
}

/* ---------------------------------------------------------------------
 * String load/store (rare in MWCC output, used by struct copies)
 * ------------------------------------------------------------------- */
static void lsw_simple(PPCContext *ctx, uint8_t *base, int rd, uint32_t ea, uint32_t n) {
    int r = (rd - 1) & 31;
    for (uint32_t k = 0; k < n; k++) {
        int pos = k & 3;
        if (pos == 0) { r = (r + 1) & 31; ctx->r[r] = 0; }
        ctx->r[r] |= ld8(base, ea + k) << (24 - 8 * pos);
    }
}
static void stsw_simple(PPCContext *ctx, uint8_t *base, int rs, uint32_t ea, uint32_t n) {
    int r = (rs - 1) & 31;
    for (uint32_t k = 0; k < n; k++) {
        int pos = k & 3;
        if (pos == 0) r = (r + 1) & 31;
        st8(base, ea + k, ctx->r[r] >> (24 - 8 * pos));
    }
}
void ppc_lswi(PPCContext *ctx, uint8_t *base, int rd, int ra, int nb) {
    lsw_simple(ctx, base, rd, ra ? ctx->r[ra] : 0, nb ? (uint32_t)nb : 32);
}
void ppc_stswi(PPCContext *ctx, uint8_t *base, int rs, int ra, int nb) {
    stsw_simple(ctx, base, rs, ra ? ctx->r[ra] : 0, nb ? (uint32_t)nb : 32);
}
void ppc_lswx(PPCContext *ctx, uint8_t *base, int rd, int ra, int rb) {
    lsw_simple(ctx, base, rd, (ra ? ctx->r[ra] : 0) + ctx->r[rb], ctx->xer_bc);
}
void ppc_stswx(PPCContext *ctx, uint8_t *base, int rs, int ra, int rb) {
    stsw_simple(ctx, base, rs, (ra ? ctx->r[ra] : 0) + ctx->r[rb], ctx->xer_bc);
}
