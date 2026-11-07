/*
 * gx_gpu.h - interface between the GX front end (gx_soft.c) and a hardware
 * rasteriser (MetalGX.m on macOS).
 *
 * gx_soft.c always does the work the CPU is good at and the GPU cannot see:
 * command decoding, vertex fetch, XF transform, lighting and texgen, texture
 * decoding, and encoding EFB copies into the formats the game reads back.
 * When a GPU sink is installed, triangles are handed to it instead of the
 * software rasteriser; before every EFB copy the sink reads the affected
 * rectangle back into the CPU-side EFB, so copies work unchanged.
 *
 * All structures are plain 32-bit fields so the Metal shader can declare the
 * same layout.
 */
#pragma once
#include <stdint.h>

typedef struct {
    float x, y, z;          /* window coordinates (x, y in EFB pixels; z 0..1) */
    float w;                /* 1/w_clip, for perspective-correct interpolation */
    float col[2][4];        /* rasterised colour channels, 0..255 */
    float tex[8][2];        /* texture coordinates (already divided by q) */
    int   ntex;
} Vtx;                      /* 116 bytes, mirrored in the Metal shader */

typedef struct {
    int32_t csel[4], asel[4];                       /* input selectors a, b, c, d */
    int32_t cbias, csub, cclamp, cshift, cdst, ccmp;
    int32_t abias, asub, aclamp, ashift, adst, acmp;
    int32_t ras;            /* 0 colour0, 1 colour1, 2 zero, 3 alpha bump, 4 alpha bump normalised */
    int32_t tex_slot;       /* -1: no texture, else index into RtGpuDraw.tex */
    int32_t coord;          /* texture coordinate set */
    int32_t pad;
    int32_t kc[4];          /* konst colour rgb, [3] = konst alpha */
    int32_t ibt, ifmt, ibias, ibs, imid, isw, itw, ifb;   /* indirect command (BP 0x10+stage) */
} RtGpuStage;               /* 36 x int32 */

typedef struct {
    int32_t ntev;
    int32_t aref0, aref1, afunc0, afunc1, aop;      /* alpha compare (BP 0xF3) */
    int32_t pad[2];
    int32_t regs[4][4];     /* initial prev, C0, C1, C2 (signed 11-bit) */
    int32_t nind, ind_active, ipad[2];
    int32_t ind_slot[4], ind_coord[4];
    float   ind_div_s[4], ind_div_t[4];
    float   imtx[3][8];     /* ma mb mc md me mf (already /1024), scale 2^(s-17), pad */
    RtGpuStage st[16];
} RtGpuTev;                 /* 2592 bytes */

typedef struct {
    const uint32_t *rgba;   /* decoded texels, 0xAABBGGRR */
    uint32_t w, h;
    int32_t wrap_s, wrap_t; /* 0 clamp, 1 repeat, 2 mirror */
    int32_t linear;
    int32_t cache_slot;     /* stable identity in the front end's texture cache */
    uint64_t hash;          /* changes whenever the texels change */
    uint32_t efb_copy;      /* rgba == NULL: GPU-side EFB copy made for this guest address */
} RtGpuTex;

typedef struct {
    const Vtx *v; uint32_t nv;
    const uint32_t *idx; uint32_t ntri;             /* triangles, already culled */
    RtGpuTev tev;
    RtGpuTex tex[8]; int ntex;
    int blend_on, blend_sub, src_f, dst_f, cupd, aupd, no_alpha;
    int z_enable, z_func, z_update;
    int sx0, sy0, sx1, sy1;                         /* inclusive scissor, EFB pixels */
} RtGpuDraw;

typedef struct {
    void (*draw)(const RtGpuDraw *d);
    /* fill a rectangle; colour 0xAABBGGRR, depth 0..1 */
    void (*clear)(int x, int y, int w, int h, uint32_t rgba, float z, int color_upd, int alpha_upd, int z_upd);
    /* finish all queued work and copy the rectangle into dst (row stride in pixels) */
    void (*readback)(int x, int y, int w, int h, uint32_t *dst, int stride);
    /* optional: EFB -> texture copy kept on the GPU (no CPU wait). fmt is the
     * simplified target (6 RGBA8, 4 RGB565, 5 RGB5A3, 3 IA8, 1 I8). Returns 0
     * if not handled, then the front end reads back and encodes on the CPU. */
    int (*copy_tex)(int x, int y, int w, int h, int half, int fmt, int no_alpha, uint32_t dest, uint32_t tw, uint32_t th);
} RtGpuSink;

/* install (or remove, with NULL) the hardware rasteriser; call before rt_run */
void rt_gx_set_gpu(const RtGpuSink *sink);
