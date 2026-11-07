/*
 * ppc_context.h - guest CPU state and the primitives generated code uses.
 *
 * Guest memory model: one 1 GiB host reservation `base`. A guest effective
 * address maps to base + (ea & 0x3FFFFFFF), which folds the cached /
 * uncached mirrors together:
 *     0x80000000 / 0xC0000000  -> MEM1 (24 MiB) + module arena (8 MiB)
 *     0x90000000 / 0xD0000000  -> MEM2 (64 MiB)
 *     0xE0000000               -> locked L1 cache scratch (16 KiB)
 * 0xCC000000-0xCDFFFFFF is hardware (MMIO) and goes through ppc_mmio_*.
 * Guest data is big-endian; every access byte-swaps.
 */
#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef union { double d; uint64_t u; } PPCFReg;

typedef struct PPCContext {
    uint32_t r[32];
    PPCFReg  f[32];          /* ps0 (FPR) */
    double   ps1[32];        /* paired-single slot 1 */
    uint8_t  crb[32];        /* CR as 32 individual bits, crb[0] = CR0[LT] */
    uint32_t lr, ctr;
    uint32_t xer_so, xer_ov, xer_ca, xer_bc;
    uint32_t fpscr, msr;
    uint32_t gqr[8];
    uint32_t sr[16];
    uint32_t reserve;
    uint32_t spr[1024];
    void    *thread;         /* owning guest thread (runtime) */
} PPCContext;

typedef void (*PPCFunc)(PPCContext *ctx, uint8_t *base);
typedef struct { uint32_t addr; PPCFunc fn; } PPCFuncEntry;

#define PPC_MEM_MASK 0x3FFFFFFFu

#if defined(__clang__) || defined(__GNUC__)
#  define PPC_LIKELY(x)   __builtin_expect(!!(x), 1)
#  define PPC_UNLIKELY(x) __builtin_expect(!!(x), 0)
#  define PPC_WEAK        __attribute__((weak))
#  define PPC_INLINE      static inline __attribute__((always_inline))
#else
#  define PPC_LIKELY(x) (x)
#  define PPC_UNLIKELY(x) (x)
#  define PPC_WEAK
#  define PPC_INLINE static inline
#endif

/* Guaranteed tail calls keep split functions / loops from growing the host
 * stack. clang supports musttail on all targets we care about (Apple clang). */
#if defined(__clang__) && __has_attribute(musttail)
#  define PPC_TAILCALL(call) __attribute__((musttail)) return call
#else
#  define PPC_TAILCALL(call) do { call; return; } while (0)
#endif

/* Each guest function is a weak wrapper around __imp_f_XXXXXXXX so that an
 * HLE implementation in runtime/hle can replace it by defining f_XXXXXXXX. */
#define PPC_FUNC_DECL(name) \
    void name(PPCContext *ctx, uint8_t *base); \
    void __imp_##name(PPCContext *ctx, uint8_t *base)
#define PPC_FUNC_IMPL(name) \
    PPC_WEAK void name(PPCContext *ctx, uint8_t *base) { PPC_TAILCALL(__imp_##name(ctx, base)); } \
    void __imp_##name(PPCContext *ctx, uint8_t *base)
/* For HLE overrides: */
#define PPC_HLE(name) void name(PPCContext *ctx, uint8_t *base)

/* ---------------------------------------------------------------- runtime */
void     ppc_call(PPCContext *ctx, uint8_t *base, uint32_t addr);
void     ppc_fallthrough(PPCContext *ctx, uint32_t addr);
void     ppc_illegal(PPCContext *ctx, uint32_t pc, uint32_t word);
void     ppc_tw(PPCContext *ctx, uint32_t pc, int to, uint32_t a, uint32_t b);
void     ppc_syscall(PPCContext *ctx, uint8_t *base, uint32_t pc);
void     ppc_rfi(PPCContext *ctx, uint8_t *base);
uint32_t ppc_mfspr(PPCContext *ctx, int spr);
void     ppc_mtspr(PPCContext *ctx, uint8_t *base, int spr, uint32_t v);
uint32_t ppc_mftb(PPCContext *ctx, int tbr);
void     ppc_mtmsr(PPCContext *ctx, uint32_t v);
void     ppc_psq_load(PPCContext *ctx, uint8_t *base, uint32_t ea, int frd, int w, int i);
void     ppc_psq_store(PPCContext *ctx, uint8_t *base, uint32_t ea, int frs, int w, int i);
uint64_t ppc_fctiw(PPCContext *ctx, double v);
uint64_t ppc_fctiwz(PPCContext *ctx, double v);
void     ppc_mtfsf(PPCContext *ctx, uint32_t fm, uint32_t v);
void     ppc_fpscr_changed(PPCContext *ctx);
void     ppc_mcrfs(PPCContext *ctx, int crfd, int crfs);
void     ppc_lswi(PPCContext *ctx, uint8_t *base, int rd, int ra, int nb);
void     ppc_stswi(PPCContext *ctx, uint8_t *base, int rs, int ra, int nb);
void     ppc_lswx(PPCContext *ctx, uint8_t *base, int rd, int ra, int rb);
void     ppc_stswx(PPCContext *ctx, uint8_t *base, int rs, int ra, int rb);

uint32_t ppc_mmio_read32(uint32_t ea);
uint16_t ppc_mmio_read16(uint32_t ea);
uint8_t  ppc_mmio_read8(uint32_t ea);
void     ppc_mmio_write32(uint32_t ea, uint32_t v);
void     ppc_mmio_write16(uint32_t ea, uint16_t v);
void     ppc_mmio_write8(uint32_t ea, uint8_t v);

/* Pending-interrupt poll, emitted by the recompiler on loop back-edges and
 * after mtmsr so interrupts (VI retrace, IPC replies) reach spinning code. */
extern volatile int ppc_irq_pending;
void ppc_irq_check(PPCContext *ctx, uint8_t *base);
#define PPC_CHECK_IRQ() do { if (PPC_UNLIKELY(ppc_irq_pending)) ppc_irq_check(ctx, base); } while (0)

/* ---------------------------------------------------------------- memory */
PPC_INLINE int ppc_is_mmio(uint32_t ea) { return (ea & 0xFE000000u) == 0xCC000000u; }
PPC_INLINE uint8_t *ppc_host(uint8_t *base, uint32_t ea) { return base + (ea & PPC_MEM_MASK); }

PPC_INLINE uint32_t ld32(uint8_t *base, uint32_t ea) {
    if (PPC_UNLIKELY(ppc_is_mmio(ea))) return ppc_mmio_read32(ea);
    uint32_t v; memcpy(&v, ppc_host(base, ea), 4); return __builtin_bswap32(v);
}
PPC_INLINE uint32_t ld16(uint8_t *base, uint32_t ea) {
    if (PPC_UNLIKELY(ppc_is_mmio(ea))) return ppc_mmio_read16(ea);
    uint16_t v; memcpy(&v, ppc_host(base, ea), 2); return __builtin_bswap16(v);
}
PPC_INLINE uint32_t ld16s(uint8_t *base, uint32_t ea) { return (uint32_t)(int32_t)(int16_t)ld16(base, ea); }
PPC_INLINE uint32_t ld8(uint8_t *base, uint32_t ea) {
    if (PPC_UNLIKELY(ppc_is_mmio(ea))) return ppc_mmio_read8(ea);
    return *ppc_host(base, ea);
}
PPC_INLINE uint64_t ld64(uint8_t *base, uint32_t ea) {
    return ((uint64_t)ld32(base, ea) << 32) | ld32(base, ea + 4);
}
PPC_INLINE void st32(uint8_t *base, uint32_t ea, uint32_t v) {
    if (PPC_UNLIKELY(ppc_is_mmio(ea))) { ppc_mmio_write32(ea, v); return; }
    v = __builtin_bswap32(v); memcpy(ppc_host(base, ea), &v, 4);
}
PPC_INLINE void st16(uint8_t *base, uint32_t ea, uint32_t v) {
    if (PPC_UNLIKELY(ppc_is_mmio(ea))) { ppc_mmio_write16(ea, (uint16_t)v); return; }
    uint16_t s = __builtin_bswap16((uint16_t)v); memcpy(ppc_host(base, ea), &s, 2);
}
PPC_INLINE void st8(uint8_t *base, uint32_t ea, uint32_t v) {
    if (PPC_UNLIKELY(ppc_is_mmio(ea))) { ppc_mmio_write8(ea, (uint8_t)v); return; }
    *ppc_host(base, ea) = (uint8_t)v;
}
PPC_INLINE void st64(uint8_t *base, uint32_t ea, uint64_t v) {
    st32(base, ea, (uint32_t)(v >> 32)); st32(base, ea + 4, (uint32_t)v);
}
PPC_INLINE uint32_t ld32le(uint8_t *base, uint32_t ea) { return __builtin_bswap32(ld32(base, ea)); }
PPC_INLINE uint32_t ld16le(uint8_t *base, uint32_t ea) { return __builtin_bswap16((uint16_t)ld16(base, ea)); }
PPC_INLINE void st32le(uint8_t *base, uint32_t ea, uint32_t v) { st32(base, ea, __builtin_bswap32(v)); }
PPC_INLINE void st16le(uint8_t *base, uint32_t ea, uint32_t v) { st16(base, ea, __builtin_bswap16((uint16_t)v)); }

PPC_INLINE float ld32f(uint8_t *base, uint32_t ea) { uint32_t u = ld32(base, ea); float f; memcpy(&f, &u, 4); return f; }
PPC_INLINE void st32f(uint8_t *base, uint32_t ea, float f) { uint32_t u; memcpy(&u, &f, 4); st32(base, ea, u); }

PPC_INLINE void ppc_dcbz(uint8_t *base, uint32_t ea) {
    if (PPC_UNLIKELY(ppc_is_mmio(ea))) return;
    memset(ppc_host(base, ea), 0, 32);
}

/* ---------------------------------------------------------------- integer */
PPC_INLINE uint32_t ppc_rotl(uint32_t v, uint32_t s) { s &= 31; return s ? (v << s) | (v >> (32 - s)) : v; }
PPC_INLINE uint32_t ppc_cntlzw(uint32_t v) { return v ? (uint32_t)__builtin_clz(v) : 32; }
PPC_INLINE uint32_t ppc_divw(uint32_t a, uint32_t b) {
    int32_t sa = (int32_t)a, sb = (int32_t)b;
    if (PPC_UNLIKELY(sb == 0 || (sa == INT32_MIN && sb == -1))) return sa < 0 ? 0xFFFFFFFFu : 0;
    return (uint32_t)(sa / sb);
}
PPC_INLINE uint32_t ppc_divwu(uint32_t a, uint32_t b) { return b ? a / b : 0; }
PPC_INLINE uint32_t ppc_sraw(PPCContext *ctx, uint32_t v, uint32_t sh) {
    int32_t s = (int32_t)v;
    sh &= 0x3F;
    if (sh > 31) { ctx->xer_ca = s < 0; return s < 0 ? 0xFFFFFFFFu : 0; }
    ctx->xer_ca = (s < 0) && sh && (v & ((1u << sh) - 1));
    return (uint32_t)(s >> sh);
}

/* ---------------------------------------------------------------- CR / XER */
#define PPC_CMPS(c, f, a, b) do { int32_t _a = (a), _b = (b); uint8_t *_c = &(c)->crb[(f) * 4]; \
    _c[0] = _a < _b; _c[1] = _a > _b; _c[2] = _a == _b; _c[3] = (uint8_t)(c)->xer_so; } while (0)
#define PPC_CMPU(c, f, a, b) do { uint32_t _a = (a), _b = (b); uint8_t *_c = &(c)->crb[(f) * 4]; \
    _c[0] = _a < _b; _c[1] = _a > _b; _c[2] = _a == _b; _c[3] = (uint8_t)(c)->xer_so; } while (0)
#define PPC_CR0(c, v) PPC_CMPS(c, 0, (int32_t)(v), 0)
#define PPC_FCMP(c, f, a, b) do { double _a = (a), _b = (b); uint8_t *_c = &(c)->crb[(f) * 4]; \
    int _u = (_a != _a) || (_b != _b); _c[0] = !_u && _a < _b; _c[1] = !_u && _a > _b; \
    _c[2] = !_u && _a == _b; _c[3] = (uint8_t)_u; } while (0)
#define PPC_CR1(c) do { (c)->crb[4] = ((c)->fpscr >> 31) & 1; (c)->crb[5] = ((c)->fpscr >> 30) & 1; \
    (c)->crb[6] = ((c)->fpscr >> 29) & 1; (c)->crb[7] = ((c)->fpscr >> 28) & 1; } while (0)

PPC_INLINE uint32_t ppc_get_cr(PPCContext *ctx) {
    uint32_t v = 0;
    for (int i = 0; i < 32; i++) v |= (uint32_t)(ctx->crb[i] & 1) << (31 - i);
    return v;
}
PPC_INLINE void ppc_set_cr(PPCContext *ctx, uint32_t v, uint32_t crm) {
    for (int f = 0; f < 8; f++)
        if (crm & (0x80u >> f))
            for (int b = 0; b < 4; b++) ctx->crb[f * 4 + b] = (v >> (31 - (f * 4 + b))) & 1;
}
PPC_INLINE uint32_t ppc_get_xer(PPCContext *ctx) {
    return (ctx->xer_so << 31) | (ctx->xer_ov << 30) | (ctx->xer_ca << 29) | (ctx->xer_bc & 0x7F);
}
PPC_INLINE void ppc_set_xer(PPCContext *ctx, uint32_t v) {
    ctx->xer_so = (v >> 31) & 1; ctx->xer_ov = (v >> 30) & 1; ctx->xer_ca = (v >> 29) & 1; ctx->xer_bc = v & 0x7F;
}

#ifdef __cplusplus
}
#endif
