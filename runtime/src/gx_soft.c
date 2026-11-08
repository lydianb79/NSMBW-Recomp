/*
 * gx_soft.c - software rasteriser backend for the GX command stream.
 *
 * Implements RtGxBackend on the CPU: vertex decode (direct + indexed),
 * XF position/texture transforms, viewport, triangle setup for every
 * primitive type, a per-pixel TEV interpreter (all stages, colour + alpha
 * combiners, konst/regs/rasterised colour), every GX texture format
 * (incl. C4/C8/C14X2 palettes and CMPR), alpha compare, Z buffer and
 * blending. EFB copies write the XFB (YUYV) or a texture in GX tiled format,
 * exactly where the game expects them.
 *
 * With a GPU sink installed (gx_gpu.h) only rasterisation moves to the GPU.
 */
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "rt_internal.h"
#include "rt_gx.h"
#include "gx_gpu.h"

#define EFB_W 640
#define EFB_H 528

static uint32_t g_efb[EFB_W * EFB_H];   /* 0xAABBGGRR */
static float g_zbuf[EFB_W * EFB_H];
static uint8_t g_tmem[0x100000];        /* only TLUT area is used */

/* hardware rasteriser (Metal), NULL = software */
static const RtGpuSink *g_gpu;
void rt_gx_set_gpu(const RtGpuSink *sink) { g_gpu = sink; }

/* ------------------------------------------------------------ helpers */
static inline float u2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static inline uint32_t bp(int r) { return rt_gx.bp[r]; }
static inline float xff(int a) { return u2f(rt_gx.xf_mem[a & 0xFFF]); }
static inline float xfr(int r) { return u2f(rt_gx.xf_reg[r & 0xFF]); }
static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static inline uint8_t* gmem(uint32_t phys) { return ppc_host(rt_base, phys | 0x80000000u); }
static inline uint16_t be16p(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static inline uint32_t be32p(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

/* ------------------------------------------------------------ vertices */
/* Vtx is shared with the GPU sink: see gx_gpu.h */

static const int k_comp[8] = { 1, 1, 2, 2, 4, 0, 0, 0 };

static float read_comp(const uint8_t *p, int fmt, int frac) {
    float s = frac ? 1.0f / (float)(1 << frac) : 1.0f;
    switch (fmt) {
    case 0: return p[0] * s;
    case 1: return (int8_t)p[0] * s;
    case 2: return be16p(p) * s;
    case 3: return (int16_t)be16p(p) * s;
    default: return u2f(be32p(p));
    }
}

static void read_color(const uint8_t *p, int fmt, float out[4]) {
    uint32_t v;
    switch (fmt) {
    case 0: v = be16p(p); out[0] = ((v >> 11) & 31) * 255.0f / 31; out[1] = ((v >> 5) & 63) * 255.0f / 63; out[2] = (v & 31) * 255.0f / 31; out[3] = 255; break;
    case 1: case 2: out[0] = p[0]; out[1] = p[1]; out[2] = p[2]; out[3] = 255; break;
    case 3: v = be16p(p); out[0] = ((v >> 12) & 15) * 17.0f; out[1] = ((v >> 8) & 15) * 17.0f; out[2] = ((v >> 4) & 15) * 17.0f; out[3] = (v & 15) * 17.0f; break;
    case 4: v = (uint32_t)p[0] << 16 | p[1] << 8 | p[2];
            out[0] = ((v >> 18) & 63) * 255.0f / 63; out[1] = ((v >> 12) & 63) * 255.0f / 63;
            out[2] = ((v >> 6) & 63) * 255.0f / 63; out[3] = (v & 63) * 255.0f / 63; break;
    default: out[0] = p[0]; out[1] = p[1]; out[2] = p[2]; out[3] = p[3]; break;
    }
}
static const int k_color_size[8] = { 2, 3, 4, 2, 3, 4, 0, 0 };

/* attribute source: direct pointer or indexed array element */
static const uint8_t *attr_ptr(const uint8_t **cur, int mode, int array) {
    if (mode == 1) return *cur;
    uint32_t idx;
    if (mode == 2) { idx = (*cur)[0]; *cur += 1; }
    else { idx = be16p(*cur); *cur += 2; }
    uint32_t base = rt_gx.cp[0xA0 + array] & 0x3FFFFFFF, stride = rt_gx.cp[0xB0 + array] & 0xFF;
    return gmem(base + idx * stride);
}

static void mtx_mul(int row0, const float in[3], float out[3]) {
    for (int r = 0; r < 3; r++)
        out[r] = xff(row0 + r * 4 + 0) * in[0] + xff(row0 + r * 4 + 1) * in[1] + xff(row0 + r * 4 + 2) * in[2] + xff(row0 + r * 4 + 3);
}

static void decode_vertex(const uint8_t *p, int vat, uint32_t vsz, Vtx *v) {
    uint32_t lo = rt_gx.cp[0x50], hi = rt_gx.cp[0x60];
    uint32_t a = rt_gx.cp[0x70 + vat], b = rt_gx.cp[0x80 + vat], c = rt_gx.cp[0x90 + vat];
    uint32_t mi_a = rt_gx.cp[0x30], mi_b = rt_gx.cp[0x40];
    const uint8_t *cur = p;
    int pmtx = mi_a & 63;
    int tmtx[8] = { (mi_a >> 6) & 63, (mi_a >> 12) & 63, (mi_a >> 18) & 63, (mi_a >> 24) & 63,
                    mi_b & 63, (mi_b >> 6) & 63, (mi_b >> 12) & 63, (mi_b >> 18) & 63 };
    if (lo & 1) pmtx = *cur++ & 63;
    for (int i = 0; i < 8; i++) if ((lo >> (1 + i)) & 1) tmtx[i] = *cur++ & 63;

    float pos[3] = { 0, 0, 0 };
    int t = (lo >> 9) & 3;
    if (t) {
        int fmt = (a >> 1) & 7, frac = (a >> 4) & 31, n = (a & 1) ? 3 : 2;
        const uint8_t *q = attr_ptr(&cur, t, 0);
        for (int k = 0; k < n; k++) pos[k] = read_comp(q + k * k_comp[fmt], fmt, frac);
        if (t == 1) cur += n * k_comp[fmt];
    }
    float nrm[3] = { 0, 0, 1 }, bin_t[3] = { 1, 0, 0 }, bin_b[3] = { 0, 1, 0 };
    int has_nrm = 0;
    t = (lo >> 11) & 3;
    if (t) {
        int fmt = (a >> 10) & 7, nbt = (a >> 9) & 1, idx3 = (a >> 31) & 1;
        /* normals are fixed point: s8 -> 1/64, s16 -> 1/16384 */
        int frac = fmt == 1 ? 6 : fmt == 3 ? 14 : 0;
        int sz = k_comp[fmt];
        float *dst[3] = { nrm, bin_t, bin_b };
        int sets = nbt ? 3 : 1;
        if (t == 1) {
            for (int st = 0; st < sets; st++)
                for (int k = 0; k < 3; k++) dst[st][k] = read_comp(cur + (st * 3 + k) * sz, fmt, frac);
            cur += sets * 3 * sz;
        } else if (nbt && idx3) {
            for (int st = 0; st < 3; st++) {
                const uint8_t *q = attr_ptr(&cur, t, 1);
                for (int k = 0; k < 3; k++) dst[st][k] = read_comp(q + (st * 3 + k) * sz, fmt, frac);
            }
        } else {
            const uint8_t *q = attr_ptr(&cur, t, 1);
            for (int st = 0; st < sets; st++)
                for (int k = 0; k < 3; k++) dst[st][k] = read_comp(q + (st * 3 + k) * sz, fmt, frac);
        }
        has_nrm = 1;
    }
    for (int ci = 0; ci < 2; ci++) {
        t = (lo >> (13 + 2 * ci)) & 3;
        v->col[ci][0] = v->col[ci][1] = v->col[ci][2] = v->col[ci][3] = 255;
        if (t) {
            int fmt = (a >> (ci ? 18 : 14)) & 7;
            const uint8_t *q = attr_ptr(&cur, t, 2 + ci);
            read_color(q, fmt, v->col[ci]);
            if (t == 1) cur += k_color_size[fmt];
        }
    }
    uint32_t tcnt[8] = { (a >> 21) & 1, b & 1, (b >> 9) & 1, (b >> 18) & 1, (b >> 27) & 1, (c >> 5) & 1, (c >> 14) & 1, (c >> 23) & 1 };
    uint32_t tfmt[8] = { (a >> 22) & 7, (b >> 1) & 7, (b >> 10) & 7, (b >> 19) & 7, (b >> 28) & 7, (c >> 6) & 7, (c >> 15) & 7, (c >> 24) & 7 };
    uint32_t tfrac[8] = { (a >> 25) & 31, (b >> 4) & 31, (b >> 13) & 31, (b >> 22) & 31, c & 31, (c >> 9) & 31, (c >> 18) & 31, (c >> 27) & 31 };
    float tin[8][2];
    memset(tin, 0, sizeof tin);
    for (int i = 0; i < 8; i++) {
        t = (hi >> (2 * i)) & 3;
        if (!t) continue;
        const uint8_t *q = attr_ptr(&cur, t, 4 + i);
        int n = tcnt[i] ? 2 : 1, sz = k_comp[tfmt[i]];
        for (int k = 0; k < n; k++) tin[i][k] = read_comp(q + k * sz, tfmt[i], tfrac[i]);
        if (t == 1) cur += n * sz;
    }
    (void)vsz;

    /* position: modelview (3x4 at pmtx*4), then projection */
    float vp[3];
    mtx_mul(pmtx * 4, pos, vp);
    float P[6]; for (int k = 0; k < 6; k++) P[k] = xfr(0x20 + k);
    int ortho = rt_gx.xf_reg[0x26] & 1;
    float cx, cy, cz, cw;
    if (!ortho) { cx = P[0] * vp[0] + P[1] * vp[2]; cy = P[2] * vp[1] + P[3] * vp[2]; cz = P[4] * vp[2] + P[5]; cw = -vp[2]; }
    else { cx = P[0] * vp[0] + P[1]; cy = P[2] * vp[1] + P[3]; cz = P[4] * vp[2] + P[5]; cw = 1.0f; }
    if (fabsf(cw) < 1e-6f) cw = 1e-6f;
    float iw = 1.0f / cw;
    float wd = xfr(0x1A), ht = xfr(0x1B), zr = xfr(0x1C), xo = xfr(0x1D), yo = xfr(0x1E), fz = xfr(0x1F);
    v->x = xo + cx * iw * wd - 342.0f;
    v->y = yo + cy * iw * ht - 342.0f;
    v->z = (fz + cz * iw * zr) / 16777215.0f;
    v->w = iw;

    /* normals into view space with the normal matrix (3x3 at 0x400 + pmtx*3) */
    float vn[3] = { 0, 0, 1 }, vt[3], vb[3];
    if (has_nrm) {
        int nb = 0x400 + pmtx * 3;
        for (int r = 0; r < 3; r++) {
            vn[r] = xff(nb + r * 3) * nrm[0] + xff(nb + r * 3 + 1) * nrm[1] + xff(nb + r * 3 + 2) * nrm[2];
            vt[r] = xff(nb + r * 3) * bin_t[0] + xff(nb + r * 3 + 1) * bin_t[1] + xff(nb + r * 3 + 2) * bin_t[2];
            vb[r] = xff(nb + r * 3) * bin_b[0] + xff(nb + r * 3 + 1) * bin_b[1] + xff(nb + r * 3 + 2) * bin_b[2];
        }
        float l = sqrtf(vn[0] * vn[0] + vn[1] * vn[1] + vn[2] * vn[2]);
        if (l > 1e-6f) { vn[0] /= l; vn[1] /= l; vn[2] /= l; }
    } else { vt[0] = 1; vt[1] = vt[2] = 0; vb[1] = 1; vb[0] = vb[2] = 0; }

    /* colour channels: material/ambient from register or vertex, optional lighting */
    float vcol[2][4];
    memcpy(vcol, v->col, sizeof vcol);
    int nchan = rt_gx.xf_reg[0x09] & 3;
    for (int ci = 0; ci < 2; ci++) {
        uint32_t ctl[2] = { rt_gx.xf_reg[0x0E + ci], rt_gx.xf_reg[0x10 + ci] };
        uint32_t mat = rt_gx.xf_reg[0x0C + ci], amb = rt_gx.xf_reg[0x0A + ci];
        for (int part = 0; part < 2; part++) {         /* 0: RGB, 1: alpha */
            uint32_t c = ctl[part];
            float m[4], am[4];
            for (int k = 0; k < 4; k++) {
                m[k] = (c & 1) ? vcol[ci][k] : (float)((mat >> (24 - 8 * k)) & 255);
                am[k] = (c & 0x40) ? vcol[ci][k] : (float)((amb >> (24 - 8 * k)) & 255);
            }
            float out[4] = { m[0], m[1], m[2], m[3] };
            if (c & 2) {                                  /* lighting enabled */
                float acc[4] = { am[0], am[1], am[2], am[3] };
                uint32_t mask = ((c >> 2) & 15) | (((c >> 11) & 15) << 4);
                int dfn = (c >> 7) & 3, atten_on = (c >> 9) & 1, spot = (c >> 10) & 1;
                for (int li = 0; li < 8; li++) {
                    if (!(mask & (1u << li))) continue;
                    int L = 0x600 + li * 16;
                    uint32_t lc = rt_gx.xf_mem[L + 3];
                    float lp[3] = { xff(L + 10), xff(L + 11), xff(L + 12) }, ld[3] = { xff(L + 13), xff(L + 14), xff(L + 15) };
                    float dv[3] = { lp[0] - vp[0], lp[1] - vp[1], lp[2] - vp[2] };
                    float dist2 = dv[0] * dv[0] + dv[1] * dv[1] + dv[2] * dv[2], dist = sqrtf(dist2);
                    float ln[3] = { dist > 0 ? dv[0] / dist : 0, dist > 0 ? dv[1] / dist : 0, dist > 0 ? dv[2] / dist : 0 };
                    float att = 1.0f;
                    if (atten_on) {
                        /* GX stores light directions negated (GXInitLightDir), so the
                         * spot cosine is +dot(to_light, dir). Specular lights keep the
                         * half-angle in the direction slot and only shine on surfaces
                         * facing the light; their distance factors are normalised when
                         * a diffuse function is active (matches hardware/Dolphin). */
                        float k0 = xff(L + 7), k1 = xff(L + 8), k2 = xff(L + 9);
                        float a0 = xff(L + 4), a1 = xff(L + 5), a2 = xff(L + 6);
                        if (spot) {
                            float cs = ln[0] * ld[0] + ln[1] * ld[1] + ln[2] * ld[2];
                            if (cs < 0) cs = 0;
                            float aa = a0 + a1 * cs + a2 * cs * cs;
                            float da = k0 + k1 * dist + k2 * dist2;
                            att = da != 0 ? fmaxf(aa, 0) / da : 0;
                        } else {
                            float facing = vn[0] * ln[0] + vn[1] * ln[1] + vn[2] * ln[2];
                            float cs = 0;
                            if (facing >= 0) { cs = vn[0] * ld[0] + vn[1] * ld[1] + vn[2] * ld[2]; if (cs < 0) cs = 0; }
                            if (dfn != 0) {
                                float kl = sqrtf(k0 * k0 + k1 * k1 + k2 * k2);
                                if (kl > 0) { k0 /= kl; k1 /= kl; k2 /= kl; }
                            }
                            float aa = a0 + a1 * cs + a2 * cs * cs;
                            float da = k0 + k1 * cs + k2 * cs * cs;
                            att = da != 0 ? fmaxf(aa, 0) / da : 0;
                        }
                    }
                    float d = vn[0] * ln[0] + vn[1] * ln[1] + vn[2] * ln[2];
                    float dif = dfn == 0 ? 1.0f : dfn == 1 ? d : fmaxf(d, 0.0f);
                    for (int k = 0; k < 4; k++) acc[k] += att * dif * (float)((lc >> (24 - 8 * k)) & 255);
                }
                for (int k = 0; k < 4; k++) out[k] = m[k] * fminf(fmaxf(acc[k], 0.0f), 255.0f) / 255.0f;
            }
            if (part == 0) { v->col[ci][0] = out[0]; v->col[ci][1] = out[1]; v->col[ci][2] = out[2]; }
            else v->col[ci][3] = out[3];
        }
        if (ci >= nchan && nchan) { /* channel not enabled: leave computed value (harmless) */ }
    }

    /* texgens: XF 0x40+n, optional dual-tex post transform 0x50+n */
    int ntg = rt_gx.xf_reg[0x3F] & 15;
    if (ntg == 0) ntg = 1;
    v->ntex = ntg;
    int dual = rt_gx.xf_reg[0x12] & 1;
    for (int n = 0; n < ntg && n < 8; n++) {
        uint32_t tg = rt_gx.xf_reg[0x40 + n];
        int proj3 = (tg >> 1) & 1, abc1 = (tg >> 2) & 1, type = (tg >> 4) & 7, src = (tg >> 7) & 31;
        float in[4] = { 0, 0, 1, 1 };
        switch (src) {
        case 0: in[0] = pos[0]; in[1] = pos[1]; in[2] = pos[2]; break;          /* position (model space) */
        case 1: in[0] = nrm[0]; in[1] = nrm[1]; in[2] = nrm[2]; break;          /* normal */
        case 2: in[0] = v->col[0][0] / 255; in[1] = v->col[0][1] / 255; in[2] = v->col[0][2] / 255; break;
        case 3: in[0] = bin_t[0]; in[1] = bin_t[1]; in[2] = bin_t[2]; break;
        case 4: in[0] = bin_b[0]; in[1] = bin_b[1]; in[2] = bin_b[2]; break;
        default: if (src >= 5 && src <= 12) { in[0] = tin[src - 5][0]; in[1] = tin[src - 5][1]; } break;
        }
        if (!abc1) in[2] = 1.0f;
        float s_, t_, q_ = 1.0f;
        if (type == 2 || type == 3) {                     /* GX_TG_SRTG: s,t from colour channel R,G */
            int ch = type - 2;
            s_ = v->col[ch][0] / 255.0f; t_ = v->col[ch][1] / 255.0f;
        } else if (type == 1) {                           /* emboss: shift previous coords (approx.) */
            int es = (tg >> 12) & 7;
            s_ = v->tex[es][0]; t_ = v->tex[es][1];
        } else {
            int m = tmtx[n];
            if (m == 60) { s_ = in[0]; t_ = in[1]; q_ = proj3 ? in[2] : 1.0f; }
            else {
                int r0 = m * 4;
                s_ = xff(r0 + 0) * in[0] + xff(r0 + 1) * in[1] + xff(r0 + 2) * in[2] + xff(r0 + 3) * in[3];
                t_ = xff(r0 + 4) * in[0] + xff(r0 + 5) * in[1] + xff(r0 + 6) * in[2] + xff(r0 + 7) * in[3];
                if (proj3) q_ = xff(r0 + 8) * in[0] + xff(r0 + 9) * in[1] + xff(r0 + 10) * in[2] + xff(r0 + 11) * in[3];
            }
            if (dual) {
                uint32_t pt = rt_gx.xf_reg[0x50 + n];
                int pm = pt & 63, norm = (pt >> 8) & 1;
                if (norm) {
                    float l = sqrtf(s_ * s_ + t_ * t_ + q_ * q_);
                    if (l > 1e-6f) { s_ /= l; t_ /= l; q_ /= l; }
                }
                int r0 = 0x500 + pm * 4;
                float s2 = xff(r0 + 0) * s_ + xff(r0 + 1) * t_ + xff(r0 + 2) * q_ + xff(r0 + 3);
                float t2 = xff(r0 + 4) * s_ + xff(r0 + 5) * t_ + xff(r0 + 6) * q_ + xff(r0 + 7);
                float q2 = xff(r0 + 8) * s_ + xff(r0 + 9) * t_ + xff(r0 + 10) * q_ + xff(r0 + 11);
                s_ = s2; t_ = t2; q_ = proj3 ? q2 : 1.0f;
            }
        }
        if (fabsf(q_) < 1e-6f) q_ = 1e-6f;
        v->tex[n][0] = s_ / q_; v->tex[n][1] = t_ / q_;
    }
    (void)vt; (void)vb;
}

/* ------------------------------------------------------------ textures */
typedef struct {
    uint32_t addr, w, h, fmt, tlut_key, wrap_s, wrap_t;
    int linear;
    uint64_t hash, checked_frame;
    uint32_t bytes;
    uint32_t *rgba;
    int gpu_copy;           /* texels live only on the GPU (EFB copy), rgba == NULL */
} Tex;
static uint64_t g_frame_no = 1;

/* FNV-1a over 64-bit words: cheap compared with decoding */
static uint64_t hash_mem(const uint8_t *p, uint32_t n) {
    uint64_t h = 1469598103934665603ull;
    uint32_t i = 0;
    for (; i + 8 <= n; i += 8) { uint64_t w; memcpy(&w, p + i, 8); h = (h ^ w) * 1099511628211ull; }
    for (; i < n; i++) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}
static uint32_t tex_bytes(uint32_t w, uint32_t h, uint32_t fmt) {
    static const int bw[16] = { 8, 8, 8, 4, 4, 4, 4, 0, 8, 8, 4, 0, 0, 0, 8, 0 };
    static const int bh[16] = { 8, 4, 4, 4, 4, 4, 4, 0, 8, 4, 4, 0, 0, 0, 8, 0 };
    int BW = bw[fmt & 15] ? bw[fmt & 15] : 4, BH = bh[fmt & 15] ? bh[fmt & 15] : 4;
    uint32_t blocks = ((w + BW - 1) / BW) * ((h + BH - 1) / BH);
    return blocks * (fmt == 6 ? 64 : 32);
}

#define TEX_CACHE 512
static Tex g_tex[TEX_CACHE];
static int g_tex_next;

__attribute__((unused)) static void tex_cache_clear(void) {
    for (int i = 0; i < TEX_CACHE; i++) { free(g_tex[i].rgba); g_tex[i].rgba = NULL; g_tex[i].addr = 0; }
}
/* drop cached textures overlapping [addr, addr+len) (render-to-texture target) */
static uint32_t g_tmem_gen = 1;     /* bumped on TLUT loads / invalidation: resets get_texmap's fast path */
static void tex_cache_invalidate(uint32_t addr, uint32_t len) {
    g_tmem_gen++;                       /* drop the per-map fast path */
    for (int i = 0; i < TEX_CACHE; i++) {
        Tex *t = &g_tex[i];
        if (t->rgba && t->addr < addr + len && addr < t->addr + t->bytes) { free(t->rgba); t->rgba = NULL; t->addr = 0; }
    }
}

static uint32_t rgba(int r, int g, int b, int a) { return (uint32_t)r | (uint32_t)g << 8 | (uint32_t)b << 16 | (uint32_t)a << 24; }

static uint32_t tlut_color(uint32_t tlut, int idx) {
    /* SETTLUT: bits 0-9 tmem offset (in 512-byte units), 10-11 format */
    uint32_t off = (tlut & 0x3FF) << 9, fmt = (tlut >> 10) & 3;
    const uint8_t *p = g_tmem + ((off + (uint32_t)idx * 2) & (sizeof g_tmem - 2));
    uint32_t v = be16p(p);
    switch (fmt) {
    case 0: return rgba(v >> 8, v >> 8, v >> 8, v & 255);                          /* IA8 */
    case 1: return rgba(((v >> 11) & 31) * 255 / 31, ((v >> 5) & 63) * 255 / 63, (v & 31) * 255 / 31, 255);
    default:
        if (v & 0x8000) return rgba(((v >> 10) & 31) * 255 / 31, ((v >> 5) & 31) * 255 / 31, (v & 31) * 255 / 31, 255);
        return rgba(((v >> 8) & 15) * 17, ((v >> 4) & 15) * 17, (v & 15) * 17, ((v >> 12) & 7) * 255 / 7);
    }
}

static void decode_cmpr_block(const uint8_t *s, uint32_t *dst, int stride) {
    uint16_t c0 = be16p(s), c1 = be16p(s + 2);
    uint32_t pal[4];
    int r0 = ((c0 >> 11) & 31) * 255 / 31, g0 = ((c0 >> 5) & 63) * 255 / 63, b0 = (c0 & 31) * 255 / 31;
    int r1 = ((c1 >> 11) & 31) * 255 / 31, g1 = ((c1 >> 5) & 63) * 255 / 63, b1 = (c1 & 31) * 255 / 31;
    pal[0] = rgba(r0, g0, b0, 255); pal[1] = rgba(r1, g1, b1, 255);
    if (c0 > c1) {
        pal[2] = rgba((2 * r0 + r1) / 3, (2 * g0 + g1) / 3, (2 * b0 + b1) / 3, 255);
        pal[3] = rgba((r0 + 2 * r1) / 3, (g0 + 2 * g1) / 3, (b0 + 2 * b1) / 3, 255);
    } else {
        pal[2] = rgba((r0 + r1) / 2, (g0 + g1) / 2, (b0 + b1) / 2, 255);
        pal[3] = rgba((r0 + r1) / 2, (g0 + g1) / 2, (b0 + b1) / 2, 0);
    }
    for (int y = 0; y < 4; y++) {
        uint8_t bits = s[4 + y];
        for (int x = 0; x < 4; x++) dst[y * stride + x] = pal[(bits >> (6 - 2 * x)) & 3];
    }
}

static void decode_texture(Tex *t, uint32_t tlut) {
    uint32_t w = t->w, h = t->h, fmt = t->fmt;
    t->rgba = malloc((size_t)w * h * 4);
    static const int bw[16] = { 8, 8, 8, 4, 4, 4, 4, 0, 8, 8, 4, 0, 0, 0, 8, 0 };
    static const int bh[16] = { 8, 4, 4, 4, 4, 4, 4, 0, 8, 4, 4, 0, 0, 0, 8, 0 };
    int BW = bw[fmt & 15] ? bw[fmt & 15] : 4, BH = bh[fmt & 15] ? bh[fmt & 15] : 4;
    const uint8_t *src = gmem(t->addr);
    uint32_t bx_n = (w + BW - 1) / BW, by_n = (h + BH - 1) / BH;
    for (uint32_t by = 0; by < by_n; by++)
        for (uint32_t bx = 0; bx < bx_n; bx++) {
            uint32_t tmp[64];
            switch (fmt) {
            case 0: for (int i = 0; i < 64; i++) { int v = (src[i / 2] >> ((i & 1) ? 0 : 4)) & 15; v *= 17; tmp[i] = rgba(v, v, v, v); } src += 32; break;
            case 1: for (int i = 0; i < 32; i++) { int v = src[i]; tmp[i] = rgba(v, v, v, v); } src += 32; break;
            case 2: for (int i = 0; i < 32; i++) { int a = (src[i] >> 4) * 17, v = (src[i] & 15) * 17; tmp[i] = rgba(v, v, v, a); } src += 32; break;
            case 3: for (int i = 0; i < 16; i++) { int a = src[2 * i], v = src[2 * i + 1]; tmp[i] = rgba(v, v, v, a); } src += 32; break;
            case 4: for (int i = 0; i < 16; i++) { uint32_t v = be16p(src + 2 * i);
                        tmp[i] = rgba(((v >> 11) & 31) * 255 / 31, ((v >> 5) & 63) * 255 / 63, (v & 31) * 255 / 31, 255); } src += 32; break;
            case 5: for (int i = 0; i < 16; i++) { uint32_t v = be16p(src + 2 * i);
                        tmp[i] = (v & 0x8000) ? rgba(((v >> 10) & 31) * 255 / 31, ((v >> 5) & 31) * 255 / 31, (v & 31) * 255 / 31, 255)
                                              : rgba(((v >> 8) & 15) * 17, ((v >> 4) & 15) * 17, (v & 15) * 17, ((v >> 12) & 7) * 255 / 7); } src += 32; break;
            case 6: for (int i = 0; i < 16; i++) tmp[i] = rgba(src[2 * i + 1], src[32 + 2 * i], src[32 + 2 * i + 1], src[2 * i]); src += 64; break;
            case 8: for (int i = 0; i < 64; i++) tmp[i] = tlut_color(tlut, (src[i / 2] >> ((i & 1) ? 0 : 4)) & 15); src += 32; break;
            case 9: for (int i = 0; i < 32; i++) tmp[i] = tlut_color(tlut, src[i]); src += 32; break;
            case 10: for (int i = 0; i < 16; i++) tmp[i] = tlut_color(tlut, be16p(src + 2 * i) & 0x3FFF); src += 32; break;
            case 14: decode_cmpr_block(src, tmp, 8); decode_cmpr_block(src + 8, tmp + 4, 8);
                     decode_cmpr_block(src + 16, tmp + 32, 8); decode_cmpr_block(src + 24, tmp + 36, 8); src += 32; break;
            default: for (int i = 0; i < 64; i++) tmp[i] = rgba(255, 0, 255, 255); src += 32; break;
            }
            for (int y = 0; y < BH; y++)
                for (int x = 0; x < BW; x++) {
                    uint32_t px = bx * BW + x, py = by * BH + y;
                    if (px < w && py < h) t->rgba[py * w + px] = tmp[y * BW + x];
                }
        }
}

/* EFB copies kept on the GPU, by guest address. The guest memory is left as it
 * was; if the game (or a DMA) later writes there, the hash changes and the
 * entry is dropped so the new contents are decoded normally. */
#define EFB_COPIES 128
typedef struct { uint32_t addr, w, h, bytes; uint64_t mem_hash, checked_frame; int valid; } EfbCopy;
static EfbCopy g_copies[EFB_COPIES];
static Tex g_copy_tex[EFB_COPIES];
static EfbCopy *efb_copy_find(uint32_t addr) {
    for (int i = 0; i < EFB_COPIES; i++) if (g_copies[i].valid && g_copies[i].addr == addr) return &g_copies[i];
    return NULL;
}

/* last lookup per texture map: skips the cache scan (and the palette hash)
 * while the map's registers, TMEM palettes and frame are unchanged */
static struct { uint32_t mode0, img0, img3, tlut, tmem_gen; uint64_t frame; Tex *tex; } g_map_last[8];
static Tex *get_texmap_slow(int map);
static Tex *get_texmap(int map) {
    int mi = map & 3, set = map >> 2, m = map & 7;
    uint32_t mode0 = bp((set ? 0xA0 : 0x80) + mi), img0 = bp((set ? 0xA8 : 0x88) + mi);
    uint32_t img3 = bp((set ? 0xB4 : 0x94) + mi), tlut = bp((set ? 0xB8 : 0x98) + mi);
    if (g_map_last[m].tex && g_map_last[m].frame == g_frame_no && g_map_last[m].mode0 == mode0 &&
        g_map_last[m].img0 == img0 && g_map_last[m].img3 == img3 && g_map_last[m].tlut == tlut &&
        g_map_last[m].tmem_gen == g_tmem_gen && g_map_last[m].tex->addr == ((img3 & 0xFFFFFF) << 5) &&
        (g_map_last[m].tex->rgba || g_map_last[m].tex->gpu_copy))
        return g_map_last[m].tex;
    Tex *t = get_texmap_slow(map);
    g_map_last[m].mode0 = mode0; g_map_last[m].img0 = img0; g_map_last[m].img3 = img3; g_map_last[m].tlut = tlut;
    g_map_last[m].tmem_gen = g_tmem_gen; g_map_last[m].frame = g_frame_no; g_map_last[m].tex = t;
    return t;
}
static Tex *get_texmap_slow(int map) {
    int mi = map & 3, set = map >> 2;
    uint32_t mode0 = bp((set ? 0xA0 : 0x80) + mi);
    uint32_t img0 = bp((set ? 0xA8 : 0x88) + mi);
    uint32_t img3 = bp((set ? 0xB4 : 0x94) + mi);
    uint32_t tlut = bp((set ? 0xB8 : 0x98) + mi);
    uint32_t addr = (img3 & 0xFFFFFF) << 5, w = (img0 & 0x3FF) + 1, h = ((img0 >> 10) & 0x3FF) + 1, fmt = (img0 >> 20) & 15;
    if (!addr) return NULL;
    if (g_gpu) {
        EfbCopy *ec = efb_copy_find(addr);
        if (ec && ec->w == w && ec->h == h) {
            if (ec->checked_frame != g_frame_no) {
                ec->checked_frame = g_frame_no;
                if (hash_mem(gmem(addr), ec->bytes) != ec->mem_hash) { ec->valid = 0; ec = NULL; }
            }
            if (ec) {
                Tex *t = &g_copy_tex[ec - g_copies];
                t->addr = addr; t->w = w; t->h = h; t->fmt = fmt; t->rgba = NULL; t->gpu_copy = 1;
                t->wrap_s = mode0 & 3; t->wrap_t = (mode0 >> 2) & 3;
                t->linear = rt_settings.texture_filter == RT_TEXFILTER_LINEAR ||
                            (rt_settings.texture_filter == RT_TEXFILTER_GAME && ((mode0 >> 4) & 1));
                return t;
            }
        }
    }
    uint32_t key = 0;
    if (fmt >= 8 && fmt <= 10) {
        uint32_t off = (tlut & 0x3FF) << 9, entries = fmt == 8 ? 16 : fmt == 9 ? 256 : 4096;
        if (off + entries * 2 > sizeof g_tmem) entries = (sizeof g_tmem - off) / 2;
        key = (uint32_t)hash_mem(g_tmem + off, entries * 2) ^ tlut;
    }
    uint32_t nbytes = tex_bytes(w, h, fmt);
    for (int i = 0; i < TEX_CACHE; i++) {
        Tex *t = &g_tex[i];
        if (t->rgba && t->addr == addr && t->w == w && t->h == h && t->fmt == fmt && t->tlut_key == key) {
            if (t->checked_frame != g_frame_no) {            /* validate once per frame */
                uint64_t hsh = hash_mem(gmem(addr), nbytes);
                t->checked_frame = g_frame_no;
                if (hsh != t->hash) { free(t->rgba); t->hash = hsh; decode_texture(t, tlut); }
            }
            t->wrap_s = mode0 & 3; t->wrap_t = (mode0 >> 2) & 3;
            t->linear = rt_settings.texture_filter == RT_TEXFILTER_LINEAR ||
                        (rt_settings.texture_filter == RT_TEXFILTER_GAME && ((mode0 >> 4) & 1));
            return t;
        }
    }
    Tex *t = &g_tex[g_tex_next];
    g_tex_next = (g_tex_next + 1) % TEX_CACHE;
    g_tmem_gen++;                       /* a slot is being reused: drop fast-path pointers */
    free(t->rgba);
    t->addr = addr; t->w = w; t->h = h; t->fmt = fmt; t->tlut_key = key;
    t->bytes = nbytes; t->hash = hash_mem(gmem(addr), nbytes); t->checked_frame = g_frame_no;
    t->wrap_s = mode0 & 3; t->wrap_t = (mode0 >> 2) & 3;
    t->linear = rt_settings.texture_filter == RT_TEXFILTER_LINEAR ||
                (rt_settings.texture_filter == RT_TEXFILTER_GAME && ((mode0 >> 4) & 1));
    decode_texture(t, tlut);
    return t;
}

static inline int wrap(int c, int n, int mode) {
    if (mode == 1) {
        if (!(n & (n - 1))) return c & (n - 1);              /* power of two: no division */
        c %= n; return c < 0 ? c + n : c;
    }
    if (mode == 2) {
        int p = n * 2;
        c = !(p & (p - 1)) ? (c & (p - 1)) : ((c % p) + p) % p;
        return c < n ? c : p - 1 - c;
    }
    return c < 0 ? 0 : c >= n ? n - 1 : c;
}

static void sample(Tex *t, float s, float tt, int out[4]) {
    if (!t) { out[0] = out[1] = out[2] = out[3] = 255; return; }
    float fx = s * t->w - 0.5f, fy = tt * t->h - 0.5f;
    if (!t->linear) {
        int x = wrap((int)floorf(fx + 0.5f), (int)t->w, t->wrap_s), y = wrap((int)floorf(fy + 0.5f), (int)t->h, t->wrap_t);
        uint32_t c = t->rgba[y * t->w + x];
        out[0] = c & 255; out[1] = (c >> 8) & 255; out[2] = (c >> 16) & 255; out[3] = c >> 24;
        return;
    }
    /* bilinear in 8.8 fixed point */
    int ix = (int)floorf(fx * 256.0f), iy = (int)floorf(fy * 256.0f);
    int x0 = ix >> 8, y0 = iy >> 8, ax = ix & 255, ay = iy & 255;
    int xa = wrap(x0, (int)t->w, t->wrap_s), xb = wrap(x0 + 1, (int)t->w, t->wrap_s);
    const uint32_t *r0 = t->rgba + (size_t)wrap(y0, (int)t->h, t->wrap_t) * t->w;
    const uint32_t *r1 = t->rgba + (size_t)wrap(y0 + 1, (int)t->h, t->wrap_t) * t->w;
    uint32_t c00 = r0[xa], c10 = r0[xb], c01 = r1[xa], c11 = r1[xb];
    int w00 = (256 - ax) * (256 - ay), w10 = ax * (256 - ay), w01 = (256 - ax) * ay, w11 = ax * ay;
    for (int k = 0; k < 4; k++, c00 >>= 8, c10 >>= 8, c01 >>= 8, c11 >>= 8)
        out[k] = (int)(((c00 & 255) * w00 + (c10 & 255) * w10 + (c01 & 255) * w01 + (c11 & 255) * w11 + 32768) >> 16);
}

/* ------------------------------------------------------------ TEV */
typedef struct { int c[4]; } C4;

static int reg_val(int reg, int comp) {           /* TEV colour registers BP 0xE0..0xE7 (11-bit signed) */
    uint32_t ra = bp(0xE0 + reg * 2), bg = bp(0xE1 + reg * 2);
    int v;
    switch (comp) {
    case 0: v = ra & 0x7FF; break;                 /* R */
    case 3: v = (ra >> 12) & 0x7FF; break;         /* A */
    case 2: v = bg & 0x7FF; break;                 /* B */
    default: v = (bg >> 12) & 0x7FF; break;        /* G */
    }
    return (v & 0x400) ? v - 0x800 : v;
}
/* konst registers live in the same BP addresses with bit 23 set */
static int g_konst[4][4];
static int g_tevreg[4][4];

static void tev_regs_load(void) {
    for (int r = 0; r < 4; r++)
        for (int k = 0; k < 4; k++) g_tevreg[r][k] = reg_val(r, k);
}

static int konst_color(int sel, int comp) {
    /* KCSEL/KASEL: 0..7 constants 1, 7/8 .. 1/8; 12.. konst regs */
    static const int frac[8] = { 255, 223, 191, 159, 128, 96, 64, 32 };
    if (sel < 8) return frac[sel];
    if (sel >= 12 && sel <= 15) return g_konst[sel - 12][comp];
    if (sel >= 16) { int r = (sel - 16) & 3, ch = (sel - 16) >> 2; return g_konst[r][ch]; }
    return 255;
}

static inline int tev_clamp(int v, int clamp) { return clamp ? clampi(v, 0, 255) : clampi(v, -1024, 1023); }

__attribute__((unused)) static void tev_run(const int tex_col[16][4], const int ras[2][4], int out[4], int nstages) {
    int prev[4], regs[4][4];
    memcpy(regs, g_tevreg, sizeof regs);
    for (int k = 0; k < 4; k++) prev[k] = regs[0][k];
    for (int s = 0; s < nstages; s++) {
        uint32_t ce = bp(0xC0 + 2 * s), ae = bp(0xC1 + 2 * s);
        uint32_t tref = bp(0x28 + s / 2) >> ((s & 1) * 12);
        uint32_t ksel = bp(0xF6 + s / 2) >> ((s & 1) * 10);
        int kc = (ksel >> 4) & 31, ka = (ksel >> 9) & 31;
        int chan = (tref >> 7) & 7;
        const int *tx = tex_col[s];
        int rc[4];
        int ri = chan == 1 ? 1 : 0;
        if (chan == 7) { rc[0] = rc[1] = rc[2] = rc[3] = 0; }
        else memcpy(rc, ras[ri], sizeof rc);
        if (chan == 0 || chan == 1) { /* COLOR0A0 / COLOR1A1 */ }
        /* colour inputs */
        int cin[4][3];
        for (int i = 0; i < 4; i++) {
            int sel = (ce >> (12 - 4 * i)) & 15;     /* a=bits12-15, b=8-11, c=4-7, d=0-3 */
            for (int k = 0; k < 3; k++) {
                int v;
                switch (sel) {
                case 0: v = prev[k]; break;  case 1: v = prev[3]; break;
                case 2: v = regs[1][k]; break; case 3: v = regs[1][3]; break;
                case 4: v = regs[2][k]; break; case 5: v = regs[2][3]; break;
                case 6: v = regs[3][k]; break; case 7: v = regs[3][3]; break;
                case 8: v = tx[k]; break;   case 9: v = tx[3]; break;
                case 10: v = rc[k]; break;  case 11: v = rc[3]; break;
                case 12: v = 255; break;    case 13: v = 128; break;
                case 14: v = konst_color(kc, k); break;
                default: v = 0; break;
                }
                cin[i][k] = v;
            }
        }
        int ain[4];
        for (int i = 0; i < 4; i++) {
            int sel = (ae >> (13 - 3 * i)) & 7;      /* a=13-15, b=10-12, c=7-9, d=4-6 */
            switch (sel) {
            case 0: ain[i] = prev[3]; break; case 1: ain[i] = regs[1][3]; break;
            case 2: ain[i] = regs[2][3]; break; case 3: ain[i] = regs[3][3]; break;
            case 4: ain[i] = tx[3]; break;   case 5: ain[i] = rc[3]; break;
            case 6: ain[i] = konst_color(ka, 3); break;
            default: ain[i] = 0; break;
            }
        }
        static const int scale_shift[4] = { 0, 1, 2, -1 };
        int cbias = (ce >> 16) & 3, csub = (ce >> 18) & 1, cclamp = (ce >> 19) & 1, cscale = (ce >> 20) & 3, cdst = (ce >> 22) & 3;
        int abias = (ae >> 16) & 3, asub = (ae >> 18) & 1, aclamp = (ae >> 19) & 1, ascale = (ae >> 20) & 3, adst = (ae >> 22) & 3;
        int res[4];
        for (int k = 0; k < 3; k++) {
            int a = cin[0][k] & 255, b = cin[1][k] & 255, c = cin[2][k] & 255, d = cin[3][k];
            if (cbias == 3) {   /* compare modes */
                res[k] = d + (a > b ? c : 0);
            } else {
                int lerp = (a * (256 - (c + (c >> 7))) + b * (c + (c >> 7))) >> 8;
                int v = csub ? d - lerp : d + lerp;
                v += cbias == 1 ? 128 : cbias == 2 ? -128 : 0;
                int sh = scale_shift[cscale];
                v = sh >= 0 ? v << sh : v >> 1;
                res[k] = v;
            }
            res[k] = tev_clamp(res[k], cclamp);
        }
        {
            int a = ain[0] & 255, b = ain[1] & 255, c = ain[2] & 255, d = ain[3];
            int v;
            if (abias == 3) v = d + (a > b ? c : 0);
            else {
                int lerp = (a * (256 - (c + (c >> 7))) + b * (c + (c >> 7))) >> 8;
                v = asub ? d - lerp : d + lerp;
                v += abias == 1 ? 128 : abias == 2 ? -128 : 0;
                int sh = scale_shift[ascale];
                v = sh >= 0 ? v << sh : v >> 1;
            }
            res[3] = tev_clamp(v, aclamp);
        }
        int *dc = cdst == 0 ? prev : regs[cdst];
        dc[0] = res[0]; dc[1] = res[1]; dc[2] = res[2];
        int *da = adst == 0 ? prev : regs[adst];
        da[3] = res[3];
    }
    for (int k = 0; k < 4; k++) out[k] = clampi(prev[k], 0, 255);
}

static int alpha_compare(int a) {
    uint32_t ac = bp(0xF3);
    int r0 = ac & 255, r1 = (ac >> 8) & 255, f0 = (ac >> 16) & 7, f1 = (ac >> 19) & 7, op = (ac >> 22) & 3;
    int t0, t1;
#define CMP(f, r) ((f) == 0 ? 0 : (f) == 1 ? a < (r) : (f) == 2 ? a == (r) : (f) == 3 ? a <= (r) : (f) == 4 ? a > (r) : (f) == 5 ? a != (r) : (f) == 6 ? a >= (r) : 1)
    t0 = CMP(f0, r0); t1 = CMP(f1, r1);
#undef CMP
    switch (op) { case 0: return t0 && t1; case 1: return t0 || t1; case 2: return t0 != t1; default: return t0 == t1; }
}

static inline float blend_factor(int f, int src, int dst, int sa, int da, int is_src) {
    switch (f) {
    case 0: return 0;
    case 1: return 1;
    case 2: return (is_src ? dst : src) / 255.0f;
    case 3: return 1 - (is_src ? dst : src) / 255.0f;
    case 4: return sa / 255.0f;
    case 5: return 1 - sa / 255.0f;
    case 6: return da / 255.0f;
    default: return 1 - da / 255.0f;
    }
}

/* ------------------------------------------------------------ compiled TEV */
/* Stage configuration decoded once per draw. Colour input codes:
 * 0..7 prev/regs (rgb or alpha), 8/9 tex rgb/a, 10/11 ras rgb/a, 12 one,
 * 13 half, 14 konst, 15 zero. */
typedef struct {
    uint8_t csel[4], asel[4];
    uint8_t cbias, csub, cclamp, cshift, cdst, ccmp;
    uint8_t abias, asub, aclamp, ashift, adst, acmp;
    uint8_t ras;          /* 0 colour0, 1 colour1, 2 zero, 3 alpha bump, 4 alpha bump normalised */
    int16_t kc[3], ka;
} TevStage;

static void tev_compile(TevStage *ts, int nstages) {
    for (int s = 0; s < nstages; s++) {
        TevStage *t = &ts[s];
        uint32_t ce = bp(0xC0 + 2 * s), ae = bp(0xC1 + 2 * s);
        uint32_t tref = bp(0x28 + s / 2) >> ((s & 1) * 12);
        uint32_t ksel = bp(0xF6 + s / 2) >> ((s & 1) * 10);
        int kc = (ksel >> 4) & 31, ka = (ksel >> 9) & 31, chan = (tref >> 7) & 7;
        for (int i = 0; i < 4; i++) { t->csel[i] = (ce >> (12 - 4 * i)) & 15; t->asel[i] = (ae >> (13 - 3 * i)) & 7; }
        t->cbias = (ce >> 16) & 3; t->csub = (ce >> 18) & 1; t->cclamp = (ce >> 19) & 1; t->cdst = (ce >> 22) & 3;
        t->abias = (ae >> 16) & 3; t->asub = (ae >> 18) & 1; t->aclamp = (ae >> 19) & 1; t->adst = (ae >> 22) & 3;
        t->cshift = (ce >> 20) & 3; t->ashift = (ae >> 20) & 3;
        t->ccmp = t->cbias == 3; t->acmp = t->abias == 3;
        t->ras = chan == 0 ? 0 : chan == 1 ? 1 : chan == 5 ? 3 : chan == 6 ? 4 : 2;   /* 3/4: alpha bump (normalised) */
        for (int k = 0; k < 3; k++) t->kc[k] = (int16_t)konst_color(kc, k);
        t->ka = (int16_t)konst_color(ka, 3);
    }
}

static inline int tev_lerp(int a, int b, int c, int d, int bias, int sub, int shift) {
    int lerp = (a * (256 - (c + (c >> 7))) + b * (c + (c >> 7))) >> 8;
    int v = sub ? d - lerp : d + lerp;
    v += bias == 1 ? 128 : bias == 2 ? -128 : 0;
    return shift == 0 ? v : shift == 1 ? v << 1 : shift == 2 ? v << 2 : v >> 1;
}

static void tev_run_compiled(const TevStage *ts, int nstages, const int tex_col[16][4], const int ras[2][4], int out[4], const int *bump) {
    int R[4][4];                     /* R[0] = prev, R[1..3] = C0..C2 */
    memcpy(R, g_tevreg, sizeof R);
    static const int zero4[4] = { 0, 0, 0, 0 };
    for (int s = 0; s < nstages; s++) {
        const TevStage *t = &ts[s];
        const int *tx = tex_col[s];
        int bumpc[4];
        const int *rc;
        if (t->ras <= 1) rc = ras[t->ras];
        else if (t->ras >= 3 && bump) {
            int ab = bump[s]; if (t->ras == 4) ab |= ab >> 5;
            bumpc[0] = bumpc[1] = bumpc[2] = bumpc[3] = ab; rc = bumpc;
        } else rc = zero4;
        int cin[4][3];
        for (int i = 0; i < 4; i++) {
            int sel = t->csel[i];
            const int *src; int alpha = sel & 1;
            switch (sel) {
            case 0: case 1: src = R[0]; break;
            case 2: case 3: src = R[1]; break;
            case 4: case 5: src = R[2]; break;
            case 6: case 7: src = R[3]; break;
            case 8: case 9: src = tx; break;
            case 10: case 11: src = rc; break;
            case 12: cin[i][0] = cin[i][1] = cin[i][2] = 255; continue;
            case 13: cin[i][0] = cin[i][1] = cin[i][2] = 128; continue;
            case 14: cin[i][0] = t->kc[0]; cin[i][1] = t->kc[1]; cin[i][2] = t->kc[2]; continue;
            default: cin[i][0] = cin[i][1] = cin[i][2] = 0; continue;
            }
            if (alpha) cin[i][0] = cin[i][1] = cin[i][2] = src[3];
            else { cin[i][0] = src[0]; cin[i][1] = src[1]; cin[i][2] = src[2]; }
        }
        int ain[4];
        for (int i = 0; i < 4; i++) {
            switch (t->asel[i]) {
            case 0: ain[i] = R[0][3]; break; case 1: ain[i] = R[1][3]; break;
            case 2: ain[i] = R[2][3]; break; case 3: ain[i] = R[3][3]; break;
            case 4: ain[i] = tx[3]; break;   case 5: ain[i] = rc[3]; break;
            case 6: ain[i] = t->ka; break;   default: ain[i] = 0; break;
            }
        }
        int res[4];
        for (int k = 0; k < 3; k++) {
            int v = t->ccmp ? cin[3][k] + ((cin[0][k] & 255) > (cin[1][k] & 255) ? (cin[2][k] & 255) : 0)
                            : tev_lerp(cin[0][k] & 255, cin[1][k] & 255, cin[2][k] & 255, cin[3][k], t->cbias, t->csub, t->cshift);
            res[k] = tev_clamp(v, t->cclamp);
        }
        {
            int v = t->acmp ? ain[3] + ((ain[0] & 255) > (ain[1] & 255) ? (ain[2] & 255) : 0)
                            : tev_lerp(ain[0] & 255, ain[1] & 255, ain[2] & 255, ain[3], t->abias, t->asub, t->ashift);
            res[3] = tev_clamp(v, t->aclamp);
        }
        int *dc = R[t->cdst]; dc[0] = res[0]; dc[1] = res[1]; dc[2] = res[2];
        R[t->adst][3] = res[3];
    }
    for (int k = 0; k < 4; k++) out[k] = clampi(R[0][k], 0, 255);
}

/* ------------------------------------------------------------ raster */
static int g_trace_lit;
/* per-frame raster statistics (approximate under threads; diagnostics only) */
static uint64_t g_st_tris, g_st_cull, g_st_empty, g_st_cov, g_st_zfail, g_st_afail, g_st_written;
static int g_trace_stats;
static uint64_t g_st_draws[8], g_st_draw_empty, g_st_soft_total;
static double g_lit_sum[3]; static uint64_t g_lit_n;
typedef struct { int bt, fmt, bias, bs, mid, sw, tw, fb; } IndCmd;   /* BP 0x10+stage */
typedef struct {
    int lit;
    int ntev, ntex, cull;
    int z_enable, z_func, z_update;
    uint32_t blend;
    Tex *stage_tex[16];
    int stage_coord[16];
    int sx0, sy0, sx1, sy1;
    TevStage tev[16];
    int blend_on, blend_sub, src_f, dst_f, cupd, aupd;
    int no_alpha;         /* EFB pixel format without alpha (PE 0x43: RGB8_Z24, RGB565_Z16) */
    /* indirect texturing */
    int ind_active, nind;
    Tex *ind_tex[4]; int ind_coord[4]; float ind_div_s[4], ind_div_t[4];
    float imtx[3][6]; float imtx_scale[3];      /* ma mb mc md me mf (/1024), 2^(scale-17) */
    IndCmd icmd[16];
} DrawState;

static void setup_state(DrawState *st) {
    uint32_t gen = bp(0x00);
    st->ntev = ((gen >> 10) & 15) + 1;
    st->ntex = (gen >> 0) & 15;
    st->cull = (gen >> 14) & 3;
    uint32_t zm = bp(0x40);
    st->z_enable = zm & 1; st->z_func = (zm >> 1) & 7; st->z_update = (zm >> 4) & 1;
    st->blend = bp(0x41);
    for (int s = 0; s < st->ntev; s++) {
        uint32_t tref = bp(0x28 + s / 2) >> ((s & 1) * 12);
        int map = tref & 7, coord = (tref >> 3) & 7, en = (tref >> 6) & 1;
        st->stage_tex[s] = en ? get_texmap(map) : NULL;
        st->stage_coord[s] = coord;
    }
    for (int r = 0; r < 4; r++) {
        uint32_t ra = bp(0xE0 + r * 2), bg = bp(0xE1 + r * 2);
        /* konst regs are written with bit 23 set; we kept the last write per address in bp[] */
        g_konst[r][0] = ra & 255; g_konst[r][3] = (ra >> 12) & 255; g_konst[r][2] = bg & 255; g_konst[r][1] = (bg >> 12) & 255;
    }
    tev_regs_load();
    st->blend_on = st->blend & 1; st->blend_sub = (st->blend >> 11) & 1;
    st->src_f = (st->blend >> 8) & 7; st->dst_f = (st->blend >> 5) & 7;
    st->cupd = (st->blend >> 3) & 1; st->aupd = (st->blend >> 4) & 1;
    { int pf = bp(0x43) & 7; st->no_alpha = pf == 0 || pf == 2; }
    /* indirect texturing: GENMODE.nbmp stages (BP 0x27 IREF, 0x25/0x26 scales,
     * 0x06..0x0E matrices) and one command per TEV stage (BP 0x10..0x1F) */
    st->nind = (gen >> 16) & 7;
    if (st->nind > 4) st->nind = 4;
    st->ind_active = 0;
    for (int s = 0; s < st->ntev; s++) {
        uint32_t c = bp(0x10 + s);
        IndCmd *ic = &st->icmd[s];
        ic->bt = c & 3; ic->fmt = (c >> 2) & 3; ic->bias = (c >> 4) & 7; ic->bs = (c >> 7) & 3;
        ic->mid = (c >> 9) & 15; ic->sw = (c >> 13) & 7; ic->tw = (c >> 16) & 7; ic->fb = (c >> 20) & 1;
        if (c & 0x1FFFFF) st->ind_active = 1;
    }
    uint32_t iref = bp(0x27);
    for (int i = 0; i < st->nind; i++) {
        st->ind_tex[i] = get_texmap((iref >> (6 * i)) & 7);
        st->ind_coord[i] = (iref >> (6 * i + 3)) & 7;
        uint32_t ss = bp(0x25 + i / 2) >> ((i & 1) * 8);
        st->ind_div_s[i] = 1.0f / (float)(1 << (ss & 15));
        st->ind_div_t[i] = 1.0f / (float)(1 << ((ss >> 4) & 15));
    }
    for (int m = 0; m < 3; m++) {
        uint32_t a = bp(0x06 + 3 * m), b = bp(0x07 + 3 * m), c = bp(0x08 + 3 * m);
        int raw[6] = { (int)(a & 0x7FF), (int)((a >> 11) & 0x7FF), (int)(b & 0x7FF), (int)((b >> 11) & 0x7FF),
                       (int)(c & 0x7FF), (int)((c >> 11) & 0x7FF) };
        for (int k = 0; k < 6; k++) st->imtx[m][k] = (float)((raw[k] & 0x400) ? raw[k] - 0x800 : raw[k]) / 1024.0f;
        int scale = (int)((a >> 22) & 3) | (int)((b >> 22) & 3) << 2 | (int)((c >> 22) & 3) << 4;
        st->imtx_scale[m] = ldexpf(1.0f, scale - 17);
    }
    /* scissor: BP 0x20 (top-left) 0x21 (bottom-right), offset 0x59, all +342 */
    uint32_t tl = bp(0x20), br = bp(0x21), off = bp(0x59);
    int ox = (int)(off & 0x3FF) * 2 - 342, oy = (int)((off >> 10) & 0x3FF) * 2 - 342;
    st->sy0 = (int)(tl & 0x7FF) - 342 - oy; st->sx0 = (int)((tl >> 12) & 0x7FF) - 342 - ox;
    st->sy1 = (int)(br & 0x7FF) - 342 - oy; st->sx1 = (int)((br >> 12) & 0x7FF) - 342 - ox;
    if (st->sx1 <= st->sx0 || st->sy1 <= st->sy0) { st->sx0 = 0; st->sy0 = 0; st->sx1 = EFB_W - 1; st->sy1 = EFB_H - 1; }
    st->sx0 = clampi(st->sx0, 0, EFB_W - 1); st->sx1 = clampi(st->sx1, 0, EFB_W - 1);
    st->sy0 = clampi(st->sy0, 0, EFB_H - 1); st->sy1 = clampi(st->sy1, 0, EFB_H - 1);
}

/* konst/colour register writes share BP addresses; remember konst separately */
static uint32_t g_konst_bp[8];

static inline float edge(const Vtx *a, const Vtx *b, float x, float y) {
    return (b->x - a->x) * (y - a->y) - (b->y - a->y) * (x - a->x);
}

#define BAND 8
/* Per-pixel texture lookups with indirect texturing (GX "bump" path). Coordinates
 * are handled in texel units like the hardware: wrap the regular coordinate,
 * add the offset computed from the indirect texture (matrix or dynamic S/T),
 * optionally add the previous stage's coordinate. */
static inline float wrap_ind(float c, int w) {
    if (!w) return c;
    if (w == 6) return 0;
    float size = (float)(256 >> (w - 1));
    return c - size * floorf(c / size);
}
static void ind_lookups(const DrawState *st, const float uv[8][2], int texc[16][4], int bump[16]) {
    int ind[4][3];
    for (int i = 0; i < st->nind; i++) {
        int c[4];
        int tc = st->ind_coord[i];
        sample(st->ind_tex[i], uv[tc][0] * st->ind_div_s[i], uv[tc][1] * st->ind_div_t[i], c);
        ind[i][0] = c[3]; ind[i][1] = c[2]; ind[i][2] = c[1];          /* s = A, t = B, u = G */
    }
    static const int fmt_mask[4] = { 255, 31, 15, 7 }, bump_mask[4] = { 0xF8, 0xE0, 0xF0, 0xF8 };
    float prev_s = 0, prev_t = 0;
    for (int s = 0; s < st->ntev; s++) {
        const IndCmd *ic = &st->icmd[s];
        Tex *t = st->stage_tex[s];
        float tw = t ? (float)t->w : 1.0f, th = t ? (float)t->h : 1.0f;
        int tc = st->stage_coord[s];
        float u = wrap_ind(uv[tc][0] * tw, ic->sw), v = wrap_ind(uv[tc][1] * th, ic->tw);
        float os = 0, ot = 0;
        bump[s] = 0;
        if (ic->bt < st->nind) {
            int crd[3];
            for (int k = 0; k < 3; k++) crd[k] = ind[ic->bt][k] & fmt_mask[ic->fmt];
            if (ic->bs) bump[s] = ind[ic->bt][ic->bs - 1] & bump_mask[ic->fmt];
            int badd = ic->fmt == 0 ? -128 : 1;
            for (int k = 0; k < 3; k++) if ((ic->bias >> k) & 1) crd[k] += badd;
            if (ic->mid >= 1 && ic->mid <= 3) {
                const float *m = st->imtx[ic->mid - 1];
                float sc = st->imtx_scale[ic->mid - 1];
                os = (m[0] * crd[0] + m[2] * crd[1] + m[4] * crd[2]) * sc;
                ot = (m[1] * crd[0] + m[3] * crd[1] + m[5] * crd[2]) * sc;
            } else if (ic->mid >= 5 && ic->mid <= 7) {          /* dynamic S */
                float sc = st->imtx_scale[ic->mid - 5];
                os = uv[tc][0] * tw * crd[0] / 256.0f * sc; ot = uv[tc][1] * th * crd[0] / 256.0f * sc;
            } else if (ic->mid >= 9 && ic->mid <= 11) {         /* dynamic T */
                float sc = st->imtx_scale[ic->mid - 9];
                os = uv[tc][0] * tw * crd[1] / 256.0f * sc; ot = uv[tc][1] * th * crd[1] / 256.0f * sc;
            }
        }
        float fs = u + os, ft = v + ot;
        if (ic->fb) { fs += prev_s; ft += prev_t; }
        prev_s = fs; prev_t = ft;
        if (!t) { texc[s][0] = texc[s][1] = texc[s][2] = texc[s][3] = 255; continue; }
        sample(t, fs / tw, ft / th, texc[s]);
    }
}

static void raster_tri(const DrawState *st, const Vtx *v0, const Vtx *v1, const Vtx *v2, int worker, int nworkers) {
    float area = edge(v0, v1, v2->x, v2->y);
    if (g_trace_stats && worker == 0) g_st_tris++;
    if (fabsf(area) < 1e-6f) return;
    if (st->cull == 3 || (st->cull == 1 && area < 0) || (st->cull == 2 && area > 0)) {
        if (g_trace_stats && worker == 0) g_st_cull++;
        return;
    }
    int minx = (int)floorf(fminf(v0->x, fminf(v1->x, v2->x))), maxx = (int)ceilf(fmaxf(v0->x, fmaxf(v1->x, v2->x)));
    int miny = (int)floorf(fminf(v0->y, fminf(v1->y, v2->y))), maxy = (int)ceilf(fmaxf(v0->y, fmaxf(v1->y, v2->y)));
    minx = clampi(minx, st->sx0, st->sx1); maxx = clampi(maxx, st->sx0, st->sx1);
    miny = clampi(miny, st->sy0, st->sy1); maxy = clampi(maxy, st->sy0, st->sy1);
    if (minx > maxx || miny > maxy) { if (g_trace_stats && worker == 0) g_st_empty++; return; }
    float inv = 1.0f / area;
    /* barycentric weights are affine in x and y: step them incrementally */
    float e0dx = -(v2->y - v1->y) * inv, e0dy = (v2->x - v1->x) * inv;
    float e1dx = -(v0->y - v2->y) * inv, e1dy = (v0->x - v2->x) * inv;
    float e2dx = -(v1->y - v0->y) * inv, e2dy = (v1->x - v0->x) * inv;
    const int ntev = st->ntev;
    const int affine = v0->w == v1->w && v1->w == v2->w;   /* 2D / orthographic: no per-pixel divide */
    for (int y = miny; y <= maxy; y++) {
        if (nworkers > 1 && ((y / BAND) % nworkers) != worker) { y = (y / BAND) * BAND + BAND - 1; continue; }
        float py = y + 0.5f, px0 = minx + 0.5f;
        float w0 = edge(v1, v2, px0, py) * inv, w1 = edge(v2, v0, px0, py) * inv, w2 = edge(v0, v1, px0, py) * inv;
        (void)e0dy; (void)e1dy; (void)e2dy;
        uint32_t *row = &g_efb[y * EFB_W];
        float *zrow = &g_zbuf[y * EFB_W];
        for (int x = minx; x <= maxx; x++, w0 += e0dx, w1 += e1dx, w2 += e2dx) {
            if (w0 < 0 || w1 < 0 || w2 < 0) continue;
            float z = w0 * v0->z + w1 * v1->z + w2 * v2->z;
            if (g_trace_stats) g_st_cov++;
            if (st->z_enable) {
                float zb = zrow[x];
                int pass;
                switch (st->z_func) {
                case 0: pass = 0; break; case 1: pass = z < zb; break; case 2: pass = z == zb; break;
                case 3: pass = z <= zb; break; case 4: pass = z > zb; break; case 5: pass = z != zb; break;
                case 6: pass = z >= zb; break; default: pass = 1; break;
                }
                if (!pass) { if (g_trace_stats) g_st_zfail++; continue; }
            }
            float q0, q1, q2;
            if (affine) { q0 = w0; q1 = w1; q2 = w2; }
            else {
                float ipw = 1.0f / (w0 * v0->w + w1 * v1->w + w2 * v2->w);
                q0 = w0 * v0->w * ipw; q1 = w1 * v1->w * ipw; q2 = w2 * v2->w * ipw;
            }
            int ras[2][4];
            for (int c = 0; c < 2; c++)
                for (int k = 0; k < 4; k++)
                    ras[c][k] = clampi((int)(q0 * v0->col[c][k] + q1 * v1->col[c][k] + q2 * v2->col[c][k] + 0.5f), 0, 255);
            int texc[16][4], bump[16];
            if (st->ind_active) {
                float uv[8][2];
                for (int c = 0; c < 8; c++) {
                    uv[c][0] = q0 * v0->tex[c][0] + q1 * v1->tex[c][0] + q2 * v2->tex[c][0];
                    uv[c][1] = q0 * v0->tex[c][1] + q1 * v1->tex[c][1] + q2 * v2->tex[c][1];
                }
                ind_lookups(st, (const float (*)[2])uv, texc, bump);
            } else
            for (int s2 = 0; s2 < ntev; s2++) {
                if (!st->stage_tex[s2]) { texc[s2][0] = texc[s2][1] = texc[s2][2] = texc[s2][3] = 255; continue; }
                int tc = st->stage_coord[s2];
                float u = q0 * v0->tex[tc][0] + q1 * v1->tex[tc][0] + q2 * v2->tex[tc][0];
                float vv = q0 * v0->tex[tc][1] + q1 * v1->tex[tc][1] + q2 * v2->tex[tc][1];
                sample(st->stage_tex[s2], u, vv, texc[s2]);
            }
            int out[4];
            tev_run_compiled(st->tev, ntev, (const int (*)[4])texc, (const int (*)[4])ras, out, st->ind_active ? bump : NULL);
            if (!alpha_compare(out[3])) { if (g_trace_stats) g_st_afail++; continue; }
            if (g_trace_stats) g_st_written++;
            uint32_t d = row[x];
            int dr = d & 255, dg = (d >> 8) & 255, db = (d >> 16) & 255, da = d >> 24;
            int r = out[0], g = out[1], b = out[2], a = out[3];
            if (st->blend_on) {
                if (st->blend_sub) { r = dr - r; g = dg - g; b = db - b; }
                else {
                    int sf = st->src_f, df = st->dst_f;
                    float sfr = blend_factor(sf, r, dr, a, da, 1), sfg = blend_factor(sf, g, dg, a, da, 1), sfb = blend_factor(sf, b, db, a, da, 1);
                    float dfr = blend_factor(df, r, dr, a, da, 0), dfg = blend_factor(df, g, dg, a, da, 0), dfb = blend_factor(df, b, db, a, da, 0);
                    float sfa = blend_factor(sf, a, da, a, da, 1), dfa = blend_factor(df, a, da, a, da, 0);
                    r = (int)(r * sfr + dr * dfr + 0.5f); g = (int)(g * sfg + dg * dfg + 0.5f);
                    b = (int)(b * sfb + db * dfb + 0.5f); a = (int)(a * sfa + da * dfa + 0.5f);
                }
                r = clampi(r, 0, 255); g = clampi(g, 0, 255); b = clampi(b, 0, 255); a = clampi(a, 0, 255);
            }
            if (!st->cupd) { r = dr; g = dg; b = db; }
            if (!st->aupd) a = da;
            if (st->no_alpha) a = 255;           /* RGB8 / RGB565 EFB: alpha reads back as 1.0 */
            row[x] = rgba(r, g, b, a);
            if (g_trace_lit && st->lit) { g_lit_sum[0] += r; g_lit_sum[1] += g; g_lit_sum[2] += b; g_lit_n++; }
            if (st->z_enable && st->z_update) zrow[x] = z;
        }
    }
}

/* ------------------------------------------------------------ worker pool */
/* Every worker rasterises all triangles of a draw but only its own
 * interleaved 8-row bands, so no two threads touch the same pixel and the
 * result is identical to single-threaded rendering. */
typedef struct {
    const DrawState *st; const Vtx *v; const uint32_t (*tri)[3]; uint32_t nt;
} RasterJob;

static int g_nworkers = 1;
static pthread_t g_workers[32];
static pthread_mutex_t g_pool_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_pool_go = PTHREAD_COND_INITIALIZER, g_pool_done = PTHREAD_COND_INITIALIZER;
static RasterJob g_job;
static uint64_t g_job_gen;
static int g_job_pending;

static void raster_part(const RasterJob *j, int w, int n) {
    for (uint32_t t = 0; t < j->nt; t++)
        raster_tri(j->st, &j->v[j->tri[t][0]], &j->v[j->tri[t][1]], &j->v[j->tri[t][2]], w, n);
}

static void *raster_worker(void *arg) {
    int w = (int)(intptr_t)arg;
    uint64_t seen = 0;
    for (;;) {
        pthread_mutex_lock(&g_pool_lock);
        while (g_job_gen == seen) pthread_cond_wait(&g_pool_go, &g_pool_lock);
        seen = g_job_gen;
        RasterJob j = g_job;
        pthread_mutex_unlock(&g_pool_lock);
        raster_part(&j, w, g_nworkers);
        pthread_mutex_lock(&g_pool_lock);
        if (--g_job_pending == 0) pthread_cond_signal(&g_pool_done);
        pthread_mutex_unlock(&g_pool_lock);
    }
    return NULL;
}

static void raster_dispatch(const DrawState *st, const Vtx *v, const uint32_t (*tri)[3], uint32_t nt) {
    RasterJob j = { st, v, tri, nt };
    /* small draws are cheaper on one thread than a pool round trip */
    float area = 0;
    for (uint32_t t = 0; t < nt && area < 4096; t++) {
        const Vtx *a = &v[tri[t][0]], *b = &v[tri[t][1]], *c = &v[tri[t][2]];
        area += fabsf((b->x - a->x) * (c->y - a->y) - (b->y - a->y) * (c->x - a->x)) * 0.5f;
    }
    if (g_nworkers <= 1 || area < 4096) { raster_part(&j, 0, 1); return; }
    pthread_mutex_lock(&g_pool_lock);
    g_job = j;
    g_job_pending = g_nworkers - 1;
    g_job_gen++;
    pthread_cond_broadcast(&g_pool_go);
    pthread_mutex_unlock(&g_pool_lock);
    raster_part(&j, 0, g_nworkers);                      /* the caller is worker 0 */
    pthread_mutex_lock(&g_pool_lock);
    while (g_job_pending) pthread_cond_wait(&g_pool_done, &g_pool_lock);
    pthread_mutex_unlock(&g_pool_lock);
}

/* ------------------------------------------------------------ GPU hand-off */
static int gpu_tex_slot(RtGpuDraw *d, Tex *tx) {
    if (!tx) return -1;
    if (tx->gpu_copy) {
        for (int k = 0; k < d->ntex; k++) if (!d->tex[k].rgba && d->tex[k].efb_copy == tx->addr) return k;
        if (d->ntex >= 8) return -1;
        RtGpuTex *gt = &d->tex[d->ntex];
        memset(gt, 0, sizeof *gt);
        gt->efb_copy = tx->addr; gt->w = tx->w; gt->h = tx->h;
        gt->wrap_s = tx->wrap_s; gt->wrap_t = tx->wrap_t; gt->linear = tx->linear; gt->cache_slot = -1;
        return d->ntex++;
    }
    if (!tx->rgba) return -1;
    for (int k = 0; k < d->ntex; k++) if (d->tex[k].rgba == tx->rgba) return k;
    if (d->ntex >= 8) return -1;
    int slot = d->ntex++;
    RtGpuTex *gt = &d->tex[slot];
    gt->efb_copy = 0;
    gt->rgba = tx->rgba; gt->w = tx->w; gt->h = tx->h;
    gt->wrap_s = tx->wrap_s; gt->wrap_t = tx->wrap_t; gt->linear = tx->linear;
    gt->cache_slot = (int32_t)(tx - g_tex); gt->hash = tx->hash ^ ((uint64_t)tx->tlut_key << 32) ^ tx->addr;
    return slot;
}
static void gpu_submit(const DrawState *st, const Vtx *v, uint32_t nv, const uint32_t (*tri)[3], uint32_t nt) {
    static uint32_t *idx; static uint32_t idx_cap;
    if (nt * 3 > idx_cap) { idx_cap = nt * 3 + 1024; idx = realloc(idx, idx_cap * sizeof *idx); }
    uint32_t n = 0;
    for (uint32_t t = 0; t < nt; t++) {             /* culling stays on the CPU (GX winding rules) */
        const Vtx *a = &v[tri[t][0]], *b = &v[tri[t][1]], *c = &v[tri[t][2]];
        float area = edge(a, b, c->x, c->y);
        if (fabsf(area) < 1e-6f) continue;
        if (st->cull == 3 || (st->cull == 1 && area < 0) || (st->cull == 2 && area > 0)) continue;
        idx[n++] = tri[t][0]; idx[n++] = tri[t][1]; idx[n++] = tri[t][2];
    }
    if (!n) return;
    static RtGpuDraw d;                              /* large: keep it off the stack */
    memset(&d, 0, sizeof d);
    d.v = v; d.nv = nv; d.idx = idx; d.ntri = n / 3;
    uint32_t ac = bp(0xF3);
    d.tev.ntev = st->ntev;
    d.tev.aref0 = ac & 255; d.tev.aref1 = (ac >> 8) & 255;
    d.tev.afunc0 = (ac >> 16) & 7; d.tev.afunc1 = (ac >> 19) & 7; d.tev.aop = (ac >> 22) & 3;
    memcpy(d.tev.regs, g_tevreg, sizeof d.tev.regs);
    for (int s = 0; s < st->ntev; s++) {
        const TevStage *t = &st->tev[s];
        RtGpuStage *g = &d.tev.st[s];
        for (int i = 0; i < 4; i++) { g->csel[i] = t->csel[i]; g->asel[i] = t->asel[i]; }
        g->cbias = t->cbias; g->csub = t->csub; g->cclamp = t->cclamp; g->cshift = t->cshift; g->cdst = t->cdst; g->ccmp = t->ccmp;
        g->abias = t->abias; g->asub = t->asub; g->aclamp = t->aclamp; g->ashift = t->ashift; g->adst = t->adst; g->acmp = t->acmp;
        g->ras = t->ras; g->coord = st->stage_coord[s];
        g->kc[0] = t->kc[0]; g->kc[1] = t->kc[1]; g->kc[2] = t->kc[2]; g->kc[3] = t->ka;
        const IndCmd *ic = &st->icmd[s];
        g->ibt = ic->bt; g->ifmt = ic->fmt; g->ibias = ic->bias; g->ibs = ic->bs;
        g->imid = ic->mid; g->isw = ic->sw; g->itw = ic->tw; g->ifb = ic->fb;
        g->tex_slot = gpu_tex_slot(&d, st->stage_tex[s]);
    }
    d.tev.nind = st->nind; d.tev.ind_active = st->ind_active;
    for (int i = 0; i < st->nind; i++) {
        d.tev.ind_slot[i] = gpu_tex_slot(&d, st->ind_tex[i]);
        d.tev.ind_coord[i] = st->ind_coord[i];
        d.tev.ind_div_s[i] = st->ind_div_s[i]; d.tev.ind_div_t[i] = st->ind_div_t[i];
    }
    for (int m = 0; m < 3; m++) {
        for (int k = 0; k < 6; k++) d.tev.imtx[m][k] = st->imtx[m][k];
        d.tev.imtx[m][6] = st->imtx_scale[m];
    }
    d.blend_on = st->blend_on; d.blend_sub = st->blend_sub; d.src_f = st->src_f; d.dst_f = st->dst_f;
    d.cupd = st->cupd; d.aupd = st->aupd; d.no_alpha = st->no_alpha;
    d.z_enable = st->z_enable; d.z_func = st->z_func; d.z_update = st->z_update;
    d.sx0 = st->sx0; d.sy0 = st->sy0; d.sx1 = st->sx1; d.sy1 = st->sy1;
    g_gpu->draw(&d);
}

static void soft_draw(uint32_t prim, int vat, uint32_t count, const uint8_t *data, uint32_t vsz) {
    if (g_trace_stats) { g_st_draws[(prim >> 3) & 7]++; if (!count) g_st_draw_empty++; g_st_soft_total++; }
    if (!count || count > 65535) return;
    if (g_trace_lit && ((rt_gx.cp[0x50] >> 11) & 3)) {
        static int seen; static uint32_t keys[64];
        uint32_t k = rt_gx.xf_reg[0x0E] ^ (rt_gx.xf_reg[0x0C] * 31) ^ (rt_gx.xf_reg[0x0A] * 131) ^ (bp(0xC0) * 7);
        int dup = 0; for (int i = 0; i < seen; i++) if (keys[i] == k) dup = 1;
        if (!dup && seen < 64) {
            keys[seen++] = k;
            rt_log(0, "lit draw: chan0 %08X alpha0 %08X mat %08X amb %08X | ntev %d tev0 c %06X a %06X tref %06X | nchan %u ntex %u",
                   rt_gx.xf_reg[0x0E], rt_gx.xf_reg[0x10], rt_gx.xf_reg[0x0C], rt_gx.xf_reg[0x0A],
                   ((bp(0) >> 10) & 15) + 1, bp(0xC0), bp(0xC1), bp(0x28), rt_gx.xf_reg[0x09], rt_gx.xf_reg[0x3F]);
            for (int n = 0; n < (int)(rt_gx.xf_reg[0x3F] & 15); n++)
                rt_log(0, "   texgen %d: %08X post %08X dual %u | cp mtxidx %08X %08X", n, rt_gx.xf_reg[0x40 + n], rt_gx.xf_reg[0x50 + n],
                       rt_gx.xf_reg[0x12] & 1, rt_gx.cp[0x30], rt_gx.cp[0x40]);
            for (int s2 = 0; s2 < ((bp(0) >> 10) & 15) + 1; s2++) {
                uint32_t tref = bp(0x28 + s2 / 2) >> ((s2 & 1) * 12);
                int map = tref & 7; uint32_t img0 = bp((map >> 2 ? 0xA8 : 0x88) + (map & 3)), mode0 = bp((map >> 2 ? 0xA0 : 0x80) + (map & 3));
                rt_log(0, "   stage %d: c %06X a %06X tref %03X ksel %06X | map %d %ux%u fmt %u wrap %u/%u",
                       s2, bp(0xC0 + 2 * s2), bp(0xC1 + 2 * s2), tref & 0xFFF, bp(0xF6 + s2 / 2),
                       map, (img0 & 0x3FF) + 1, ((img0 >> 10) & 0x3FF) + 1, (img0 >> 20) & 15, mode0 & 3, (mode0 >> 2) & 3);
            }
        }
    }
    DrawState st;
    setup_state(&st);
    st.lit = (rt_gx.cp[0x50] >> 11) & 3;
    for (int r = 0; r < 4; r++) {   /* konst colours were captured separately */
        uint32_t ra = g_konst_bp[r * 2], bg = g_konst_bp[r * 2 + 1];
        g_konst[r][0] = ra & 255; g_konst[r][3] = (ra >> 12) & 255; g_konst[r][2] = bg & 255; g_konst[r][1] = (bg >> 12) & 255;
    }
    tev_compile(st.tev, st.ntev);
    static Vtx *v; static uint32_t v_cap;
    if (count > v_cap) { v_cap = count + 1024; free(v); v = malloc(sizeof(Vtx) * v_cap); }
    for (uint32_t i = 0; i < count; i++) decode_vertex(data + i * vsz, vat, vsz, &v[i]);
    /* triangle list */
    uint32_t maxt = count * 2 + 4, nt = 0;
    static uint32_t (*tri)[3]; static uint32_t tri_cap;
    if (maxt > tri_cap) { tri_cap = maxt + 2048; free(tri); tri = malloc(sizeof *tri * tri_cap); }
    switch (prim) {
    case GX_QUADS: case GX_QUADS2:
        for (uint32_t i = 0; i + 3 < count; i += 4) {
            tri[nt][0] = i; tri[nt][1] = i + 1; tri[nt][2] = i + 2; nt++;
            tri[nt][0] = i; tri[nt][1] = i + 2; tri[nt][2] = i + 3; nt++;
        }
        break;
    case GX_TRIANGLES:
        for (uint32_t i = 0; i + 2 < count; i += 3) { tri[nt][0] = i; tri[nt][1] = i + 1; tri[nt][2] = i + 2; nt++; }
        break;
    case GX_TRIANGLESTRIP:
        for (uint32_t i = 0; i + 2 < count; i++) {
            if (i & 1) { tri[nt][0] = i + 1; tri[nt][1] = i; } else { tri[nt][0] = i; tri[nt][1] = i + 1; }
            tri[nt][2] = i + 2; nt++;
        }
        break;
    case GX_TRIANGLEFAN:
        for (uint32_t i = 1; i + 1 < count; i++) { tri[nt][0] = 0; tri[nt][1] = i; tri[nt][2] = i + 1; nt++; }
        break;
    default:   /* lines / points: debug drawing only, skipped */
        break;
    }
    static long drawlog = -2;
    if (drawlog == -2) drawlog = getenv("RT_GX_DRAWLOG") ? atol(getenv("RT_GX_DRAWLOG")) : -1;
    uint64_t w_before = g_st_written;
    if (nt && g_gpu) gpu_submit(&st, v, count, tri, nt);
    else if (nt) raster_dispatch(&st, v, tri, nt);
    if (drawlog >= 0 && (long)g_frame_no >= drawlog && (long)g_frame_no < drawlog + 1) {
        float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
        for (uint32_t i = 0; i < count; i++) { x0 = fminf(x0, v[i].x); x1 = fmaxf(x1, v[i].x); y0 = fminf(y0, v[i].y); y1 = fmaxf(y1, v[i].y); }
        Tex *t0 = st.stage_tex[0];
        rt_log(0, "draw prim %02X n %u bbox %.0f,%.0f-%.0f,%.0f z %.3f w %.3f col0 %d,%d,%d,%d ntev %d tex0 %ux%u f%u blend %d sf %d df %d cull %d ac %06X zen %d scis %d,%d-%d,%d written %llu",
               prim, count, x0, y0, x1, y1, v[0].z, v[0].w, (int)v[0].col[0][0], (int)v[0].col[0][1], (int)v[0].col[0][2], (int)v[0].col[0][3], st.ntev,
               t0 ? t0->w : 0, t0 ? t0->h : 0, t0 ? t0->fmt : 99, st.blend_on, st.src_f, st.dst_f, st.cull, bp(0xF3), st.z_enable,
               st.sx0, st.sy0, st.sx1, st.sy1, (unsigned long long)(g_st_written - w_before));
    }
}

static void soft_bp_changed(uint32_t reg, uint32_t value) {
    /* TEV colour/konst registers share 0xE0..0xE7: bit 23 selects konst */
    if (reg >= 0xE0 && reg <= 0xE7 && (value & 0x800000)) {
        g_konst_bp[reg - 0xE0] = value;
    }
    if (reg == 0x64 || reg == 0x65) {
        /* LOADTLUT0 (src addr >> 5), LOADTLUT1 (tmem addr >> 9 | count << 10) */
        if (reg == 0x65) {
            uint32_t src = (bp(0x64) & 0xFFFFFF) << 5;
            uint32_t tmem = (value & 0x3FF) << 9, n = ((value >> 10) & 0x7FF) * 32;
            if (tmem + n <= sizeof g_tmem) memcpy(g_tmem + tmem, gmem(src), n);
            g_tmem_gen++;
        }
    }
}

/* ------------------------------------------------------------ EFB copies */
static void efb_clear(uint32_t flags) {
    (void)flags;
    uint32_t ar = bp(0x4F), gb = bp(0x50);
    int pf = bp(0x43) & 7;
    uint32_t c = rgba((ar) & 255, (gb >> 8) & 255, gb & 255, (pf == 0 || pf == 2) ? 255 : (ar >> 8) & 255);
    float z = (bp(0x51) & 0xFFFFFF) / 16777215.0f;
    uint32_t xy = bp(0x49), wh = bp(0x4A);
    int x0 = xy & 0x3FF, y0 = (xy >> 10) & 0x3FF, w = (wh & 0x3FF) + 1, h = ((wh >> 10) & 0x3FF) + 1;
    uint32_t zm = bp(0x40);
    uint32_t bm = bp(0x41);
    if (g_gpu) g_gpu->clear(x0, y0, w, h, c, z, (bm >> 3) & 1, ((bm >> 4) & 1) && !(pf == 0 || pf == 2), (zm >> 4) & 1);
    for (int y = y0; y < y0 + h && y < EFB_H; y++)
        for (int x = x0; x < x0 + w && x < EFB_W; x++) {
            if ((bm >> 3) & 1) g_efb[y * EFB_W + x] = c;
            if ((zm >> 4) & 1) g_zbuf[y * EFB_W + x] = z;
        }
}

static void soft_copy_display(uint32_t xfb, uint32_t w, uint32_t h, uint32_t flags) {
    uint32_t xy = bp(0x49);
    int x0 = xy & 0x3FF, y0 = (xy >> 10) & 0x3FF;
    uint32_t stride = (bp(0x4D) & 0x3FF) * 32;
    if (!stride) stride = w * 2;
    if (g_gpu) {
        int rw = (int)w, rh = (int)h;
        if (x0 + rw > EFB_W) rw = EFB_W - x0;
        if (y0 + rh > EFB_H) rh = EFB_H - y0;
        if (rw > 0 && rh > 0) {
            if (g_gpu->display) g_gpu->display(x0, y0, rw, rh, xfb);
            g_gpu->readback(x0, y0, rw, rh, &g_efb[y0 * EFB_W + x0], EFB_W);
            int pf = bp(0x43) & 7;
            if (pf == 0 || pf == 2)              /* no alpha in the EFB: reads back as 1.0 */
                for (int yy = 0; yy < rh; yy++)
                    for (int xx = 0; xx < rw; xx++) g_efb[(y0 + yy) * EFB_W + x0 + xx] |= 0xFF000000u;
        }
    }
    uint8_t *dst = gmem(xfb);
    for (uint32_t y = 0; y < h && y0 + (int)y < EFB_H; y++) {
        uint8_t *row = dst + (size_t)y * stride;
        for (uint32_t x = 0; x + 1 < w && x0 + (int)x + 1 < EFB_W; x += 2) {
            uint32_t p0 = g_efb[(y0 + y) * EFB_W + x0 + x], p1 = g_efb[(y0 + y) * EFB_W + x0 + x + 1];
            int r0 = p0 & 255, g0 = (p0 >> 8) & 255, b0 = (p0 >> 16) & 255;
            int r1 = p1 & 255, g1 = (p1 >> 8) & 255, b1 = (p1 >> 16) & 255;
            int Y0 = ((66 * r0 + 129 * g0 + 25 * b0 + 128) >> 8) + 16, Y1 = ((66 * r1 + 129 * g1 + 25 * b1 + 128) >> 8) + 16;
            int rr = (r0 + r1) / 2, gg = (g0 + g1) / 2, bb = (b0 + b1) / 2;
            int U = ((-38 * rr - 74 * gg + 112 * bb + 128) >> 8) + 128, V = ((112 * rr - 94 * gg - 18 * bb + 128) >> 8) + 128;
            row[x * 2 + 0] = (uint8_t)clampi(Y0, 0, 255); row[x * 2 + 1] = (uint8_t)clampi(U, 0, 255);
            row[x * 2 + 2] = (uint8_t)clampi(Y1, 0, 255); row[x * 2 + 3] = (uint8_t)clampi(V, 0, 255);
        }
    }
    if (flags & (1u << 11)) efb_clear(flags);
    g_frame_no++;        /* textures are re-validated (hashed) once per frame */
    if (g_trace_stats && g_frame_no % 60 == 0) {
        rt_log(0, "raster/frame: tris %llu culled %llu offscreen %llu | px covered %llu zfail %llu alphafail %llu written %llu",
               (unsigned long long)g_st_tris / 60, (unsigned long long)g_st_cull / 60, (unsigned long long)g_st_empty / 60,
               (unsigned long long)g_st_cov / 60, (unsigned long long)g_st_zfail / 60, (unsigned long long)g_st_afail / 60,
               (unsigned long long)g_st_written / 60);
        rt_log(0, "totals: fifo draws %llu, renderer draws %llu, display copies %llu",
               (unsigned long long)rt_gx.draws, (unsigned long long)g_st_soft_total, (unsigned long long)rt_gx.efb_copies);
        rt_log(0, "draws/frame by type: quads %llu quads2 %llu tris %llu strip %llu fan %llu lines %llu lstrip %llu points %llu (empty %llu)",
               (unsigned long long)g_st_draws[0] / 60, (unsigned long long)g_st_draws[1] / 60, (unsigned long long)g_st_draws[2] / 60,
               (unsigned long long)g_st_draws[3] / 60, (unsigned long long)g_st_draws[4] / 60, (unsigned long long)g_st_draws[5] / 60,
               (unsigned long long)g_st_draws[6] / 60, (unsigned long long)g_st_draws[7] / 60, (unsigned long long)g_st_draw_empty / 60);
        memset(g_st_draws, 0, sizeof g_st_draws); g_st_draw_empty = 0;
        g_st_tris = g_st_cull = g_st_empty = g_st_cov = g_st_zfail = g_st_afail = g_st_written = 0;
    }
    if (g_trace_lit && g_lit_n) {
        static int fr; if (++fr % 30 == 0)
            rt_log(0, "lit pixels: %llu, avg rgb %.0f %.0f %.0f", (unsigned long long)g_lit_n,
                   g_lit_sum[0] / g_lit_n, g_lit_sum[1] / g_lit_n, g_lit_sum[2] / g_lit_n);
        g_lit_sum[0] = g_lit_sum[1] = g_lit_sum[2] = 0; g_lit_n = 0;
    }
}

/* EFB -> texture: write GX tiled data so later draws can sample it */
static void soft_copy_texture(uint32_t dest, uint32_t w, uint32_t h, uint32_t flags) {
    uint32_t xy = bp(0x49);
    int x0 = xy & 0x3FF, y0 = (xy >> 10) & 0x3FF;
    int half = (flags >> 9) & 1;            /* box-filter downscale */
    int fmt = ((flags >> 4) & 7) | (((flags >> 3) & 1) << 3);
    int intensity = (flags >> 15) & 1;
    (void)intensity;
    uint32_t tw = half ? w / 2 : w, th = half ? h / 2 : h;
    uint8_t *dst = gmem(dest);
    {
        int tf = fmt == 6 ? 6 : fmt == 4 ? 4 : fmt == 5 ? 5 : fmt == 3 ? 3 : (fmt == 1 || fmt == 8) ? 1 : 6;
        int pf = bp(0x43) & 7;
        static int no_gpu_copies = -1;
        if (no_gpu_copies < 0) no_gpu_copies = getenv("RT_CPU_EFB_COPIES") != NULL;
        if (g_gpu && g_gpu->copy_tex && !no_gpu_copies &&
            g_gpu->copy_tex(x0, y0, (int)w, (int)h, half, tf, pf == 0 || pf == 2, dest, tw, th)) {
            uint32_t bytes = tex_bytes(tw, th, (uint32_t)tf);
            EfbCopy *ec = efb_copy_find(dest);
            if (!ec) {
                static int next;
                for (int i = 0; i < EFB_COPIES && !ec; i++) if (!g_copies[i].valid) ec = &g_copies[i];
                if (!ec) { ec = &g_copies[next]; next = (next + 1) % EFB_COPIES; }
            }
            ec->addr = dest; ec->w = tw; ec->h = th; ec->bytes = bytes; ec->valid = 1;
            ec->mem_hash = hash_mem(dst, bytes); ec->checked_frame = g_frame_no;
            /* any overlapping entry for a different address is stale now */
            for (int i = 0; i < EFB_COPIES; i++)
                if (g_copies[i].valid && &g_copies[i] != ec && g_copies[i].addr < dest + bytes && dest < g_copies[i].addr + g_copies[i].bytes)
                    g_copies[i].valid = 0;
            if (flags & (1u << 11)) efb_clear(flags);
            tex_cache_invalidate(dest, bytes);
            return;
        }
    }
    if (g_gpu) {
        int rw = (int)w, rh = (int)h;
        if (x0 + rw > EFB_W) rw = EFB_W - x0;
        if (y0 + rh > EFB_H) rh = EFB_H - y0;
        if (rw > 0 && rh > 0) {
            g_gpu->readback(x0, y0, rw, rh, &g_efb[y0 * EFB_W + x0], EFB_W);
            int pf = bp(0x43) & 7;
            if (pf == 0 || pf == 2)              /* no alpha in the EFB: reads back as 1.0 */
                for (int yy = 0; yy < rh; yy++)
                    for (int xx = 0; xx < rw; xx++) g_efb[(y0 + yy) * EFB_W + x0 + xx] |= 0xFF000000u;
        }
    }
    /* supported encodings: RGBA8 (6), RGB565 (4), RGB5A3 (5), I8 (1/8), IA8 (3) */
    int tfmt = fmt == 6 ? 6 : fmt == 4 ? 4 : fmt == 5 ? 5 : fmt == 3 ? 3 : (fmt == 1 || fmt == 8) ? 1 : 6;
    int BW = tfmt == 1 ? 8 : 4, BH = 4;
    size_t o = 0;
    for (uint32_t by = 0; by < th; by += BH)
        for (uint32_t bx = 0; bx < tw; bx += BW) {
            uint32_t px[32];
            for (int y = 0; y < BH; y++)
                for (int x = 0; x < BW; x++) {
                    int sx = x0 + (int)(bx + x) * (half ? 2 : 1), sy = y0 + (int)(by + y) * (half ? 2 : 1);
                    px[y * BW + x] = (sx < EFB_W && sy < EFB_H) ? g_efb[sy * EFB_W + sx] : 0;
                }
            if (tfmt == 6) {
                for (int i = 0; i < 16; i++) { dst[o + 2 * i] = px[i] >> 24; dst[o + 2 * i + 1] = px[i] & 255; }
                for (int i = 0; i < 16; i++) { dst[o + 32 + 2 * i] = (px[i] >> 8) & 255; dst[o + 32 + 2 * i + 1] = (px[i] >> 16) & 255; }
                o += 64;
            } else if (tfmt == 1) {
                for (int i = 0; i < 32; i++) { uint32_t c = px[i]; dst[o + i] = (uint8_t)(((c & 255) * 77 + ((c >> 8) & 255) * 150 + ((c >> 16) & 255) * 29) >> 8); }
                o += 32;
            } else {
                for (int i = 0; i < 16; i++) {
                    uint32_t c = px[i]; int r = c & 255, g = (c >> 8) & 255, b = (c >> 16) & 255, a = c >> 24;
                    uint16_t v;
                    if (tfmt == 4) v = (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3));
                    else if (tfmt == 3) { int l = (r * 77 + g * 150 + b * 29) >> 8; v = (uint16_t)(a << 8 | l); }
                    else v = a >= 224 ? (uint16_t)(0x8000 | (r >> 3) << 10 | (g >> 3) << 5 | (b >> 3))
                                      : (uint16_t)((a >> 5) << 12 | (r >> 4) << 8 | (g >> 4) << 4 | (b >> 4));
                    dst[o + 2 * i] = v >> 8; dst[o + 2 * i + 1] = v & 255;
                }
                o += 32;
            }
        }
    if (flags & (1u << 11)) efb_clear(flags);
    tex_cache_invalidate(dest, (uint32_t)o);
}

static const RtGxBackend k_soft = {
    .draw = soft_draw,
    .bp_changed = soft_bp_changed,
    .xf_changed = NULL,
    .copy_display = soft_copy_display,
    .copy_texture = soft_copy_texture,
};

void rt_gx_soft_init(int threads) {
    const char *e = getenv("RT_RENDER_THREADS");
    g_nworkers = e ? atoi(e) : threads;
    g_trace_lit = getenv("RT_GX_TRACE_LIGHT") != NULL;
    g_trace_stats = getenv("RT_GX_STATS") != NULL;
    if (g_nworkers > 32) g_nworkers = 32;
    if (g_nworkers < 1) g_nworkers = 1;
    for (int w = 1; w < g_nworkers; w++) pthread_create(&g_workers[w], NULL, raster_worker, (void *)(intptr_t)w);
    rt_log(1, "renderer: software, %d raster threads", g_nworkers);
    for (int i = 0; i < EFB_W * EFB_H; i++) { g_efb[i] = 0xFF000000u; g_zbuf[i] = 1.0f; }
    rt_gx_backend = &k_soft;
}
