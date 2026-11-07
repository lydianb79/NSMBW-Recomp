/*
 * rt_gx.h - GX (Flipper/Hollywood GPU) state shared between the FIFO decoder
 * and a rendering backend.
 */
#pragma once
#include <stdint.h>

enum { GX_VA_PNMTXIDX, GX_VA_TEX0MTXIDX, GX_VA_POS = 9, GX_VA_NRM, GX_VA_CLR0, GX_VA_CLR1,
       GX_VA_TEX0, GX_VA_TEX7 = GX_VA_TEX0 + 7 };

enum { GX_QUADS = 0x80, GX_QUADS2 = 0x88, GX_TRIANGLES = 0x90, GX_TRIANGLESTRIP = 0x98,
       GX_TRIANGLEFAN = 0xA0, GX_LINES = 0xA8, GX_LINESTRIP = 0xB0, GX_POINTS = 0xB8 };

typedef struct {
    uint32_t cp[256];        /* CP registers (VCD 0x50/0x60, VAT 0x70-0x9F, arrays 0xA0-0xBF) */
    uint32_t xf_mem[0x1000]; /* XF matrix memory (pos/tex 0x000, normal 0x400, post 0x500) */
    uint32_t xf_reg[0x100];  /* XF registers 0x1000.. (viewport, projection, channels, texgen) */
    uint32_t bp[256];        /* BP registers (TEV, blending, textures, EFB copies) */
    uint32_t bp_mask;
    uint64_t draws, vertices, efb_copies, tex_copies, bad_opcodes, resyncs;
} RtGxState;

typedef struct {
    void (*draw)(uint32_t primitive, int vat, uint32_t count, const uint8_t *vertices, uint32_t vertex_size);
    void (*bp_changed)(uint32_t reg, uint32_t value);
    void (*xf_changed)(uint32_t addr, uint32_t count);
    void (*copy_display)(uint32_t xfb_addr, uint32_t w, uint32_t h, uint32_t copy_flags);
    void (*copy_texture)(uint32_t dest_addr, uint32_t w, uint32_t h, uint32_t copy_flags);
} RtGxBackend;

extern RtGxState rt_gx;
extern const RtGxBackend *rt_gx_backend;
extern volatile int rt_gx_skip_draws;   /* debug: skip rasterising (headless fast-forward) */
uint32_t rt_gx_vertex_size(int vat);
void rt_gx_flush(void);
