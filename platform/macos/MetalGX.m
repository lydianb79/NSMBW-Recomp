/*
 * MetalGX.m - hardware rasteriser for the GX pipeline.
 *
 * gx_soft.c keeps doing vertex processing, lighting, texgen and texture
 * decoding on the CPU; this file draws the resulting triangles with Metal:
 *   - the EFB is a 640x528 RGBA8 colour texture plus a 32-bit depth texture,
 *   - TEV (all 16 stages, colour/alpha combiners, konst, registers, compare
 *     modes) and alpha test run in one uber fragment shader driven by a
 *     per-draw constant block, so no shader is ever compiled mid-game,
 *   - blending, colour/alpha write masks, depth test/update and scissor map
 *     onto Metal pipeline / depth-stencil state (cached by key),
 *   - decoded textures are uploaded once and re-uploaded only when the
 *     front end's content hash changes.
 * Work is batched into one command buffer until the game copies the EFB
 * (display or render-to-texture); then the batch is submitted, and the copied
 * rectangle is read back for the CPU-side copy encoder.
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <string.h>

#include "MetalGX.h"
#include "rt.h"
#include "gx_gpu.h"

#define EFB_W 640
#define EFB_H 528
#define TEX_SLOTS 512
#define VBUF_SIZE (32u << 20)
#define IBUF_SIZE (16u << 20)

/* ------------------------------------------------------------ shaders */
static NSString *const kShaderSource = @"#include <metal_stdlib>\n"
"using namespace metal;\n"
"struct Vtx { float x, y, z, w; float col[2][4]; float tex[8][2]; int ntex; };\n"
"struct Stage { int csel[4]; int asel[4];\n"
"  int cbias, csub, cclamp, cshift, cdst, ccmp; int abias, asub, aclamp, ashift, adst, acmp;\n"
"  int ras, tex_slot, coord, pad; int kc[4]; int ibt, ifmt, ibias, ibs, imid, isw, itw, ifb; };\n"
"struct Tev { int ntev, aref0, aref1, afunc0, afunc1, aop, pad0, pad1; int regs[4][4];\n"
"  int nind, ind_active, ipad0, ipad1; int ind_slot[4]; int ind_coord[4]; float ind_div_s[4]; float ind_div_t[4];\n"
"  float imtx[3][8]; Stage st[16]; };\n"
"struct VOut { float4 pos [[position]]; float4 c0; float4 c1;\n"
"  float2 t0; float2 t1; float2 t2; float2 t3; float2 t4; float2 t5; float2 t6; float2 t7; };\n"
"vertex VOut gx_vs(uint vid [[vertex_id]], const device Vtx *v [[buffer(0)]]) {\n"
"  Vtx p = v[vid]; VOut o;\n"
"  float W = p.w > 1e-7 ? 1.0 / p.w : 1.0;\n"
"  float nx = p.x / 640.0 * 2.0 - 1.0, ny = 1.0 - p.y / 528.0 * 2.0;\n"
"  o.pos = float4(nx * W, ny * W, clamp(p.z, 0.0, 1.0) * W, W);\n"
"  o.c0 = float4(p.col[0][0], p.col[0][1], p.col[0][2], p.col[0][3]);\n"
"  o.c1 = float4(p.col[1][0], p.col[1][1], p.col[1][2], p.col[1][3]);\n"
"  o.t0 = float2(p.tex[0][0], p.tex[0][1]); o.t1 = float2(p.tex[1][0], p.tex[1][1]);\n"
"  o.t2 = float2(p.tex[2][0], p.tex[2][1]); o.t3 = float2(p.tex[3][0], p.tex[3][1]);\n"
"  o.t4 = float2(p.tex[4][0], p.tex[4][1]); o.t5 = float2(p.tex[5][0], p.tex[5][1]);\n"
"  o.t6 = float2(p.tex[6][0], p.tex[6][1]); o.t7 = float2(p.tex[7][0], p.tex[7][1]);\n"
"  return o;\n"
"}\n"
"static float4 tap(int k, float2 uv, texture2d<float> t0, texture2d<float> t1, texture2d<float> t2, texture2d<float> t3,\n"
"                 texture2d<float> t4, texture2d<float> t5, texture2d<float> t6, texture2d<float> t7,\n"
"                 sampler s0, sampler s1, sampler s2, sampler s3, sampler s4, sampler s5, sampler s6, sampler s7) {\n"
"  switch (k) {\n"
"  case 0: return t0.sample(s0, uv, level(0)); case 1: return t1.sample(s1, uv, level(0));\n"
"  case 2: return t2.sample(s2, uv, level(0)); case 3: return t3.sample(s3, uv, level(0));\n"
"  case 4: return t4.sample(s4, uv, level(0)); case 5: return t5.sample(s5, uv, level(0));\n"
"  case 6: return t6.sample(s6, uv, level(0)); default: return t7.sample(s7, uv, level(0));\n"
"  }\n"
"}\n"
"static float2 dims(int k, texture2d<float> t0, texture2d<float> t1, texture2d<float> t2, texture2d<float> t3,\n"
"                   texture2d<float> t4, texture2d<float> t5, texture2d<float> t6, texture2d<float> t7) {\n"
"  switch (k) {\n"
"  case 0: return float2(t0.get_width(), t0.get_height()); case 1: return float2(t1.get_width(), t1.get_height());\n"
"  case 2: return float2(t2.get_width(), t2.get_height()); case 3: return float2(t3.get_width(), t3.get_height());\n"
"  case 4: return float2(t4.get_width(), t4.get_height()); case 5: return float2(t5.get_width(), t5.get_height());\n"
"  case 6: return float2(t6.get_width(), t6.get_height()); default: return float2(t7.get_width(), t7.get_height());\n"
"  }\n"
"}\n"
"static float wrap_ind(float c, int w) {\n"
"  if (w == 0) return c;\n"
"  if (w == 6) return 0.0;\n"
"  float size = float(256 >> (w - 1));\n"
"  return c - size * floor(c / size);\n"
"}\n"
"static int tev_lerp(int a, int b, int c, int d, int bias, int sub, int shift) {\n"
"  int cc = c + (c >> 7);\n"
"  int lerp = (a * (256 - cc) + b * cc) >> 8;\n"
"  int v = sub != 0 ? d - lerp : d + lerp;\n"
"  v += bias == 1 ? 128 : (bias == 2 ? -128 : 0);\n"
"  return shift == 0 ? v : (shift == 1 ? v * 2 : (shift == 2 ? v * 4 : (v >> 1)));\n"
"}\n"
"static int tev_clamp(int v, int c) { return c != 0 ? clamp(v, 0, 255) : clamp(v, -1024, 1023); }\n"
"static bool acmp(int f, int a, int r) {\n"
"  switch (f) { case 0: return false; case 1: return a < r; case 2: return a == r; case 3: return a <= r;\n"
"  case 4: return a > r; case 5: return a != r; case 6: return a >= r; default: return true; }\n"
"}\n"
"fragment float4 gx_fs(VOut in [[stage_in]], constant Tev &T [[buffer(0)]],\n"
"  texture2d<float> x0 [[texture(0)]], texture2d<float> x1 [[texture(1)]], texture2d<float> x2 [[texture(2)]], texture2d<float> x3 [[texture(3)]],\n"
"  texture2d<float> x4 [[texture(4)]], texture2d<float> x5 [[texture(5)]], texture2d<float> x6 [[texture(6)]], texture2d<float> x7 [[texture(7)]],\n"
"  sampler s0 [[sampler(0)]], sampler s1 [[sampler(1)]], sampler s2 [[sampler(2)]], sampler s3 [[sampler(3)]],\n"
"  sampler s4 [[sampler(4)]], sampler s5 [[sampler(5)]], sampler s6 [[sampler(6)]], sampler s7 [[sampler(7)]]) {\n"
"  int4 R[4];\n"
"  for (int r = 0; r < 4; r++) R[r] = int4(T.regs[r][0], T.regs[r][1], T.regs[r][2], T.regs[r][3]);\n"
"  int4 ras0 = clamp(int4(in.c0 + 0.5), 0, 255), ras1 = clamp(int4(in.c1 + 0.5), 0, 255);\n"
"  float2 tc[8] = { in.t0, in.t1, in.t2, in.t3, in.t4, in.t5, in.t6, in.t7 };\n"
"  int3 ind[4] = { int3(0), int3(0), int3(0), int3(0) };\n"
"  for (int i = 0; i < T.nind; i++) {\n"
"    if (T.ind_slot[i] < 0) continue;\n"
"    float2 uv = tc[T.ind_coord[i] & 7] * float2(T.ind_div_s[i], T.ind_div_t[i]);\n"
"    int4 c = int4(tap(T.ind_slot[i], uv, x0, x1, x2, x3, x4, x5, x6, x7, s0, s1, s2, s3, s4, s5, s6, s7) * 255.0 + 0.5);\n"
"    ind[i] = int3(c.w, c.z, c.y);\n"
"  }\n"
"  float2 prevc = float2(0.0);\n"
"  for (int s = 0; s < T.ntev; s++) {\n"
"    constant Stage &S = T.st[s];\n"
"    int4 tx = int4(255);\n"
"    int bump = 0;\n"
"    float2 suv = tc[S.coord & 7];\n"
"    if (T.ind_active != 0) {\n"
"      float2 td = S.tex_slot >= 0 ? dims(S.tex_slot, x0, x1, x2, x3, x4, x5, x6, x7) : float2(1.0);\n"
"      float2 texel = suv * td;\n"
"      float2 w = float2(wrap_ind(texel.x, S.isw), wrap_ind(texel.y, S.itw));\n"
"      float2 off = float2(0.0);\n"
"      if (S.ibt < T.nind) {\n"
"        int fm = S.ifmt == 0 ? 255 : (S.ifmt == 1 ? 31 : (S.ifmt == 2 ? 15 : 7));\n"
"        int bm = S.ifmt == 0 ? 0xF8 : (S.ifmt == 1 ? 0xE0 : (S.ifmt == 2 ? 0xF0 : 0xF8));\n"
"        int3 raw = ind[S.ibt];\n"
"        int3 crd = raw & fm;\n"
"        if (S.ibs != 0) bump = raw[S.ibs - 1] & bm;\n"
"        int badd = S.ifmt == 0 ? -128 : 1;\n"
"        if ((S.ibias & 1) != 0) crd.x += badd;\n"
"        if ((S.ibias & 2) != 0) crd.y += badd;\n"
"        if ((S.ibias & 4) != 0) crd.z += badd;\n"
"        float3 fc = float3(crd);\n"
"        if (S.imid >= 1 && S.imid <= 3) {\n"
"          int m = S.imid - 1;\n"
"          off = float2(T.imtx[m][0] * fc.x + T.imtx[m][2] * fc.y + T.imtx[m][4] * fc.z,\n"
"                       T.imtx[m][1] * fc.x + T.imtx[m][3] * fc.y + T.imtx[m][5] * fc.z) * T.imtx[m][6];\n"
"        } else if (S.imid >= 5 && S.imid <= 7) {\n"
"          off = texel * fc.x / 256.0 * T.imtx[S.imid - 5][6];\n"
"        } else if (S.imid >= 9 && S.imid <= 11) {\n"
"          off = texel * fc.y / 256.0 * T.imtx[S.imid - 9][6];\n"
"        }\n"
"      }\n"
"      float2 f = w + off;\n"
"      if (S.ifb != 0) f += prevc;\n"
"      prevc = f;\n"
"      suv = f / td;\n"
"    }\n"
"    if (S.tex_slot >= 0) {\n"
"      float4 c = tap(S.tex_slot, suv, x0, x1, x2, x3, x4, x5, x6, x7, s0, s1, s2, s3, s4, s5, s6, s7);\n"
"      tx = int4(c * 255.0 + 0.5);\n"
"    }\n"
"    int ab = S.ras == 4 ? (bump | (bump >> 5)) : bump;\n"
"    int4 rc = S.ras == 0 ? ras0 : (S.ras == 1 ? ras1 : (S.ras >= 3 ? int4(ab) : int4(0)));\n"
"    int3 cin[4];\n"
"    for (int i = 0; i < 4; i++) {\n"
"      int sel = S.csel[i];\n"
"      int4 src = int4(0);\n"
"      if (sel <= 1) src = R[0]; else if (sel <= 3) src = R[1]; else if (sel <= 5) src = R[2]; else if (sel <= 7) src = R[3];\n"
"      else if (sel <= 9) src = tx; else if (sel <= 11) src = rc;\n"
"      if (sel <= 11) cin[i] = (sel & 1) != 0 ? int3(src.w) : src.xyz;\n"
"      else if (sel == 12) cin[i] = int3(255);\n"
"      else if (sel == 13) cin[i] = int3(128);\n"
"      else if (sel == 14) cin[i] = int3(S.kc[0], S.kc[1], S.kc[2]);\n"
"      else cin[i] = int3(0);\n"
"    }\n"
"    int ain[4];\n"
"    for (int i = 0; i < 4; i++) {\n"
"      int sel = S.asel[i];\n"
"      ain[i] = sel == 0 ? R[0].w : sel == 1 ? R[1].w : sel == 2 ? R[2].w : sel == 3 ? R[3].w :\n"
"               sel == 4 ? tx.w : sel == 5 ? rc.w : sel == 6 ? S.kc[3] : 0;\n"
"    }\n"
"    int3 res;\n"
"    for (int k = 0; k < 3; k++) {\n"
"      int v = S.ccmp != 0 ? cin[3][k] + (((cin[0][k] & 255) > (cin[1][k] & 255)) ? (cin[2][k] & 255) : 0)\n"
"                          : tev_lerp(cin[0][k] & 255, cin[1][k] & 255, cin[2][k] & 255, cin[3][k], S.cbias, S.csub, S.cshift);\n"
"      res[k] = tev_clamp(v, S.cclamp);\n"
"    }\n"
"    int va = S.acmp != 0 ? ain[3] + (((ain[0] & 255) > (ain[1] & 255)) ? (ain[2] & 255) : 0)\n"
"                         : tev_lerp(ain[0] & 255, ain[1] & 255, ain[2] & 255, ain[3], S.abias, S.asub, S.ashift);\n"
"    va = tev_clamp(va, S.aclamp);\n"
"    int cd = S.cdst & 3, ad = S.adst & 3;\n"
"    R[cd] = int4(res, R[cd].w);\n"
"    R[ad].w = va;\n"
"  }\n"
"  int4 o = clamp(R[0], 0, 255);\n"
"  bool t0 = acmp(T.afunc0, o.w, T.aref0), t1 = acmp(T.afunc1, o.w, T.aref1);\n"
"  bool pass = T.aop == 0 ? (t0 && t1) : T.aop == 1 ? (t0 || t1) : T.aop == 2 ? (t0 != t1) : (t0 == t1);\n"
"  if (!pass) discard_fragment();\n"
"  return float4(o) / 255.0;\n"
"}\n"
"struct ClearIn { float4 color; float z; };\n"
"vertex float4 clear_vs(uint vid [[vertex_id]], constant ClearIn &c [[buffer(0)]]) {\n"
"  float2 p = float2((vid & 1) ? 1.0 : -1.0, (vid & 2) ? 1.0 : -1.0);\n"
"  return float4(p, c.z, 1.0);\n"
"}\n"
"fragment float4 clear_fs(constant ClearIn &c [[buffer(0)]]) { return c.color; }\n"
"struct CopyIn { int x, y, half_, fmt, no_alpha, pad0, pad1, pad2; };\n"
"vertex float4 copy_vs(uint vid [[vertex_id]]) {\n"
"  return float4((vid & 1) ? 1.0 : -1.0, (vid & 2) ? 1.0 : -1.0, 0.0, 1.0);\n"
"}\n"
"fragment float4 copy_fs(float4 pos [[position]], constant CopyIn &c [[buffer(0)]], texture2d<float> efb [[texture(0)]]) {\n"
"  uint2 d = uint2(pos.xy);\n"
"  int sc = c.half_ != 0 ? 2 : 1;\n"
"  int2 s0 = int2(c.x, c.y) + int2(d) * sc;\n"
"  int2 lim = int2(efb.get_width() - 1, efb.get_height() - 1);\n"
"  float4 v = float4(0.0);\n"
"  for (int j = 0; j < sc; j++) for (int i = 0; i < sc; i++) v += efb.read(uint2(clamp(s0 + int2(i, j), int2(0), lim)));\n"
"  v /= float(sc * sc);\n"
"  if (c.no_alpha != 0) v.a = 1.0;\n"
"  int4 q = int4(v * 255.0 + 0.5);\n"
"  if (c.fmt == 4) { q.rb = (q.rb >> 3) * 255 / 31; q.g = (q.g >> 2) * 255 / 63; q.a = 255; }\n"
"  else if (c.fmt == 5) {\n"
"    if (q.a >= 224) { q.rgb = (q.rgb >> 3) * 255 / 31; q.a = 255; }\n"
"    else { q.rgb = (q.rgb >> 4) * 17; q.a = (q.a >> 5) * 255 / 7; }\n"
"  } else if (c.fmt == 3 || c.fmt == 1) {\n"
"    int l = (q.r * 77 + q.g * 150 + q.b * 29) >> 8;\n"
"    q = c.fmt == 3 ? int4(l, l, l, q.a) : int4(l);\n"
"  }\n"
"  return float4(q) / 255.0;\n"
"}\n";

/* ------------------------------------------------------------ state */
typedef struct { uint64_t hash; uint32_t w, h; uint64_t used_pass; } MTex;   /* texture objects live in g_mtex_tex */

static id<MTLDevice> g_dev;
static id<MTLCommandQueue> g_queue;
static id<MTLLibrary> g_lib;
static id<MTLTexture> g_efb, g_depth, g_white;
static id<MTLBuffer> g_vbuf, g_ibuf;
static uint32_t g_voff, g_ioff;
static id<MTLCommandBuffer> g_cb;
static id<MTLRenderCommandEncoder> g_enc;
static bool g_efb_ready;
static uint64_t g_pass = 1;
static NSMutableDictionary<NSNumber *, id<MTLRenderPipelineState>> *g_pipes;
static NSMutableDictionary<NSNumber *, id<MTLDepthStencilState>> *g_depths;
static id<MTLSamplerState> g_samplers[3][3][2];
static MTex g_mtex[TEX_SLOTS];
static id<MTLTexture> g_mtex_tex[TEX_SLOTS];
static uint64_t g_draws, g_flushes, g_copies_gpu;
static id<MTLRenderPipelineState> g_copy_pipe;
static NSMutableDictionary<NSNumber *, id<MTLTexture>> *g_copy_tex;   /* guest address -> copy */

static MTLBlendFactor gx_factor(int f, bool is_src, bool no_alpha) {
    switch (f) {
    case 0: return MTLBlendFactorZero;
    case 1: return MTLBlendFactorOne;
    case 2: return is_src ? MTLBlendFactorDestinationColor : MTLBlendFactorSourceColor;
    case 3: return is_src ? MTLBlendFactorOneMinusDestinationColor : MTLBlendFactorOneMinusSourceColor;
    case 4: return MTLBlendFactorSourceAlpha;
    case 5: return MTLBlendFactorOneMinusSourceAlpha;
    case 6: return no_alpha ? MTLBlendFactorOne : MTLBlendFactorDestinationAlpha;
    default: return no_alpha ? MTLBlendFactorZero : MTLBlendFactorOneMinusDestinationAlpha;
    }
}

/* key: bit0 blend, bit1 subtract, bits2-4 src, bits5-7 dst, bit8 rgb write, bit9 alpha write,
 *      bit10 no-alpha EFB, bit16 clear pipeline */
static id<MTLRenderPipelineState> pipeline(uint32_t key) {
    NSNumber *k = @(key);
    id<MTLRenderPipelineState> p = g_pipes[k];
    if (p) return p;
    MTLRenderPipelineDescriptor *d = [[MTLRenderPipelineDescriptor alloc] init];
    bool clear = (key >> 16) & 1;
    d.vertexFunction = [g_lib newFunctionWithName:clear ? @"clear_vs" : @"gx_vs"];
    d.fragmentFunction = [g_lib newFunctionWithName:clear ? @"clear_fs" : @"gx_fs"];
    d.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    d.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
    MTLColorWriteMask m = MTLColorWriteMaskNone;
    if ((key >> 8) & 1) m |= MTLColorWriteMaskRed | MTLColorWriteMaskGreen | MTLColorWriteMaskBlue;
    if ((key >> 9) & 1) m |= MTLColorWriteMaskAlpha;
    d.colorAttachments[0].writeMask = m;
    if (key & 1) {
        bool sub = (key >> 1) & 1, na = (key >> 10) & 1;
        MTLRenderPipelineColorAttachmentDescriptor *c = d.colorAttachments[0];
        c.blendingEnabled = YES;
        if (sub) {
            c.rgbBlendOperation = c.alphaBlendOperation = MTLBlendOperationReverseSubtract;   /* dst - src */
            c.sourceRGBBlendFactor = c.sourceAlphaBlendFactor = MTLBlendFactorOne;
            c.destinationRGBBlendFactor = c.destinationAlphaBlendFactor = MTLBlendFactorOne;
        } else {
            c.rgbBlendOperation = c.alphaBlendOperation = MTLBlendOperationAdd;
            c.sourceRGBBlendFactor = c.sourceAlphaBlendFactor = gx_factor((key >> 2) & 7, true, na);
            c.destinationRGBBlendFactor = c.destinationAlphaBlendFactor = gx_factor((key >> 5) & 7, false, na);
        }
    }
    NSError *err = nil;
    p = [g_dev newRenderPipelineStateWithDescriptor:d error:&err];
    if (!p) { rt_log(0, "metal: pipeline %08X failed: %s", key, err.localizedDescription.UTF8String); return nil; }
    g_pipes[k] = p;
    return p;
}

static id<MTLDepthStencilState> depth_state(int enable, int func, int update) {
    uint32_t key = enable ? (1u | (uint32_t)(func & 7) << 1 | (uint32_t)(update & 1) << 4) : 0;
    NSNumber *k = @(key);
    id<MTLDepthStencilState> s = g_depths[k];
    if (s) return s;
    MTLDepthStencilDescriptor *d = [[MTLDepthStencilDescriptor alloc] init];
    /* GX compare codes 0..7 have the same order as MTLCompareFunction */
    d.depthCompareFunction = enable ? (MTLCompareFunction)(func & 7) : MTLCompareFunctionAlways;
    d.depthWriteEnabled = enable && update;
    s = [g_dev newDepthStencilStateWithDescriptor:d];
    g_depths[k] = s;
    return s;
}

static id<MTLTexture> upload(const RtGpuTex *t) {
    int slot = t->cache_slot;
    if (slot < 0 || slot >= TEX_SLOTS) slot = 0;
    MTex *e = &g_mtex[slot];
    if (g_mtex_tex[slot] && e->hash == t->hash && e->w == t->w && e->h == t->h) { e->used_pass = g_pass; return g_mtex_tex[slot]; }
    /* never overwrite a texture that queued (uncommitted) draws still sample */
    if (!g_mtex_tex[slot] || e->w != t->w || e->h != t->h || e->used_pass == g_pass) {
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                     width:t->w height:t->h mipmapped:NO];
        td.usage = MTLTextureUsageShaderRead;
        td.storageMode = MTLStorageModeShared;
        g_mtex_tex[slot] = [g_dev newTextureWithDescriptor:td];
        e->w = t->w; e->h = t->h;
    }
    [g_mtex_tex[slot] replaceRegion:MTLRegionMake2D(0, 0, t->w, t->h) mipmapLevel:0 withBytes:t->rgba bytesPerRow:t->w * 4];
    e->hash = t->hash;
    e->used_pass = g_pass;
    return g_mtex_tex[slot];
}

static void begin_pass(void) {
    if (g_enc) return;
    if (!g_cb) g_cb = [g_queue commandBuffer];
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = g_efb;
    rp.colorAttachments[0].loadAction = g_efb_ready ? MTLLoadActionLoad : MTLLoadActionClear;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    rp.depthAttachment.texture = g_depth;
    rp.depthAttachment.loadAction = g_efb_ready ? MTLLoadActionLoad : MTLLoadActionClear;
    rp.depthAttachment.clearDepth = 1.0;
    rp.depthAttachment.storeAction = MTLStoreActionStore;
    g_efb_ready = true;
    g_enc = [g_cb renderCommandEncoderWithDescriptor:rp];
    [g_enc setViewport:(MTLViewport){ 0, 0, EFB_W, EFB_H, 0, 1 }];
    [g_enc setDepthClipMode:MTLDepthClipModeClamp];
    for (int i = 0; i < 8; i++) {
        [g_enc setFragmentTexture:g_white atIndex:i];
        [g_enc setFragmentSamplerState:g_samplers[0][0][0] atIndex:i];
    }
}

static void flush(void) {
    if (g_enc || g_cb) {
        if (!g_cb) g_cb = [g_queue commandBuffer];
        if (g_enc) [g_enc endEncoding];
        g_enc = nil;
        if (g_efb.storageMode == MTLStorageModeManaged) {   /* Intel: make the GPU copy visible to the CPU */
            id<MTLBlitCommandEncoder> b = [g_cb blitCommandEncoder];
            [b synchronizeResource:g_efb];
            [b endEncoding];
        }
        [g_cb commit];
        [g_cb waitUntilCompleted];
        if (g_cb.error) rt_log(0, "metal: command buffer error: %s", g_cb.error.localizedDescription.UTF8String);
        g_cb = nil;
        g_flushes++;
    }
    g_pass++;
    g_voff = g_ioff = 0;
}

static void set_scissor(int x0, int y0, int x1, int y1) {
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > EFB_W - 1) x1 = EFB_W - 1;
    if (y1 > EFB_H - 1) y1 = EFB_H - 1;
    if (x1 < x0 || y1 < y0) { x0 = y0 = 0; x1 = y1 = 0; }
    [g_enc setScissorRect:(MTLScissorRect){ (NSUInteger)x0, (NSUInteger)y0, (NSUInteger)(x1 - x0 + 1), (NSUInteger)(y1 - y0 + 1) }];
}

/* ------------------------------------------------------------ sink */
static void mtl_draw(const RtGpuDraw *d) {
    @autoreleasepool {
        uint32_t vbytes = d->nv * (uint32_t)sizeof(Vtx), ibytes = d->ntri * 3u * 4u;
        if (vbytes > VBUF_SIZE || ibytes > IBUF_SIZE) return;
        if (g_voff + vbytes > VBUF_SIZE || g_ioff + ibytes > IBUF_SIZE) flush();
        begin_pass();
        uint32_t key = (uint32_t)d->blend_on | (uint32_t)d->blend_sub << 1 | (uint32_t)(d->src_f & 7) << 2 |
                       (uint32_t)(d->dst_f & 7) << 5 | (uint32_t)d->cupd << 8 |
                       (uint32_t)(d->aupd && !d->no_alpha) << 9 | (uint32_t)d->no_alpha << 10;
        if (!d->blend_on) key &= ~0x4FFu & ~0x2u;   /* blend factors irrelevant */
        id<MTLRenderPipelineState> p = pipeline(key);
        if (!p) return;
        if (!(key & 0x300) && !(d->z_enable && d->z_update)) return;   /* writes nothing */
        memcpy((uint8_t *)g_vbuf.contents + g_voff, d->v, vbytes);
        memcpy((uint8_t *)g_ibuf.contents + g_ioff, d->idx, ibytes);
        [g_enc setRenderPipelineState:p];
        [g_enc setDepthStencilState:depth_state(d->z_enable, d->z_func, d->z_update)];
        [g_enc setCullMode:MTLCullModeNone];
        set_scissor(d->sx0, d->sy0, d->sx1, d->sy1);
        [g_enc setVertexBuffer:g_vbuf offset:g_voff atIndex:0];
        [g_enc setFragmentBytes:&d->tev length:sizeof d->tev atIndex:0];
        for (int i = 0; i < d->ntex && i < 8; i++) {
            const RtGpuTex *t = &d->tex[i];
            if (!t->w || !t->h) continue;
            id<MTLTexture> tex = nil;
            if (!t->rgba) tex = g_copy_tex[@(t->efb_copy)];
            else tex = upload(t);
            if (!tex) continue;
            [g_enc setFragmentTexture:tex atIndex:(NSUInteger)i];
            int ws = t->wrap_s < 0 || t->wrap_s > 2 ? 0 : t->wrap_s, wt = t->wrap_t < 0 || t->wrap_t > 2 ? 0 : t->wrap_t;
            [g_enc setFragmentSamplerState:g_samplers[ws][wt][t->linear ? 1 : 0] atIndex:(NSUInteger)i];
        }
        [g_enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:d->ntri * 3 indexType:MTLIndexTypeUInt32
                         indexBuffer:g_ibuf indexBufferOffset:g_ioff];
        g_voff += (vbytes + 255) & ~255u;
        g_ioff += (ibytes + 255) & ~255u;
        g_draws++;
    }
}

static void mtl_clear(int x, int y, int w, int h, uint32_t rgba, float z, int cu, int au, int zu) {
    if (!cu && !au && !zu) return;
    @autoreleasepool {
        begin_pass();
        uint32_t key = 1u << 16 | (uint32_t)(cu ? 1 : 0) << 8 | (uint32_t)(au ? 1 : 0) << 9;
        id<MTLRenderPipelineState> p = pipeline(key);
        if (!p) return;
        struct { float color[4]; float z; float pad[3]; } c = {
            { (rgba & 255) / 255.0f, ((rgba >> 8) & 255) / 255.0f, ((rgba >> 16) & 255) / 255.0f, (rgba >> 24) / 255.0f },
            z, { 0, 0, 0 } };
        [g_enc setRenderPipelineState:p];
        [g_enc setDepthStencilState:depth_state(zu ? 1 : 0, 7, zu ? 1 : 0)];
        [g_enc setCullMode:MTLCullModeNone];
        set_scissor(x, y, x + w - 1, y + h - 1);
        [g_enc setVertexBytes:&c length:sizeof c atIndex:0];
        [g_enc setFragmentBytes:&c length:sizeof c atIndex:0];
        [g_enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    }
}

static void mtl_readback(int x, int y, int w, int h, uint32_t *dst, int stride) {
    @autoreleasepool {
        flush();
        if (!g_efb_ready || w <= 0 || h <= 0) return;
        if (stride == w) {
            [g_efb getBytes:dst bytesPerRow:(NSUInteger)w * 4 fromRegion:MTLRegionMake2D((NSUInteger)x, (NSUInteger)y, (NSUInteger)w, (NSUInteger)h) mipmapLevel:0];
        } else {
            for (int r = 0; r < h; r++)
                [g_efb getBytes:dst + (size_t)r * stride bytesPerRow:(NSUInteger)w * 4
                     fromRegion:MTLRegionMake2D((NSUInteger)x, (NSUInteger)(y + r), (NSUInteger)w, 1) mipmapLevel:0];
        }
        static uint64_t n;
        if ((++n % 600) == 0) rt_log(1, "metal: %llu draws, %llu submits, %llu GPU texture copies",
                                     (unsigned long long)g_draws, (unsigned long long)g_flushes, (unsigned long long)g_copies_gpu);
    }
}

/* EFB -> texture on the GPU: end the EFB pass, render the converted rectangle
 * into the copy's own texture, continue. No CPU wait. */
static int mtl_copy_tex(int x, int y, int w, int h, int half, int fmt, int no_alpha, uint32_t dest, uint32_t tw, uint32_t th) {
    if (!g_copy_pipe || !tw || !th || tw > 1024 || th > 1024) return 0;
    @autoreleasepool {
        (void)w; (void)h;
        NSNumber *key = @(dest);
        id<MTLTexture> t = g_copy_tex[key];
        if (!t || t.width != tw || t.height != th) {
            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                          width:tw height:th mipmapped:NO];
            td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
            td.storageMode = MTLStorageModePrivate;
            t = [g_dev newTextureWithDescriptor:td];
            if (!t) return 0;
            g_copy_tex[key] = t;
        }
        begin_pass();                      /* make sure everything drawn so far is encoded */
        [g_enc endEncoding];
        g_enc = nil;
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = t;
        rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> e = [g_cb renderCommandEncoderWithDescriptor:rp];
        struct { int32_t x, y, half_, fmt, no_alpha, pad[3]; } c = { x, y, half, fmt, no_alpha, { 0, 0, 0 } };
        [e setRenderPipelineState:g_copy_pipe];
        [e setFragmentBytes:&c length:sizeof c atIndex:0];
        [e setFragmentTexture:g_efb atIndex:0];
        [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
        [e endEncoding];
        g_copies_gpu++;
        return 1;
    }
}

static const RtGpuSink k_metal = { mtl_draw, mtl_clear, mtl_readback, mtl_copy_tex };

bool rt_metal_gx_install(void) {
    @autoreleasepool {
        g_dev = MTLCreateSystemDefaultDevice();
        if (!g_dev) { rt_log(0, "metal: no device, using the software renderer"); return false; }
        NSError *err = nil;
        MTLCompileOptions *opt = [[MTLCompileOptions alloc] init];
        g_lib = [g_dev newLibraryWithSource:kShaderSource options:opt error:&err];
        if (!g_lib) {
            rt_log(0, "metal: shader compile failed, using the software renderer:\n%s", err.localizedDescription.UTF8String);
            return false;
        }
        g_queue = [g_dev newCommandQueue];
        g_pipes = [NSMutableDictionary dictionary];
        g_depths = [NSMutableDictionary dictionary];

        MTLTextureDescriptor *cd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                      width:EFB_W height:EFB_H mipmapped:NO];
        cd.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        cd.storageMode = MTLStorageModeShared;      /* Apple silicon: CPU readback without a blit */
        g_efb = [g_dev newTextureWithDescriptor:cd];
        if (!g_efb) {                               /* Intel Macs: shared render targets are not allowed */
            cd.storageMode = MTLStorageModeManaged;
            g_efb = [g_dev newTextureWithDescriptor:cd];
        }
        MTLTextureDescriptor *dd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                                                                      width:EFB_W height:EFB_H mipmapped:NO];
        dd.usage = MTLTextureUsageRenderTarget;
        dd.storageMode = MTLStorageModePrivate;
        g_depth = [g_dev newTextureWithDescriptor:dd];

        MTLTextureDescriptor *wd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                      width:1 height:1 mipmapped:NO];
        wd.usage = MTLTextureUsageShaderRead;
        g_white = [g_dev newTextureWithDescriptor:wd];
        uint32_t white = 0xFFFFFFFFu;
        [g_white replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:&white bytesPerRow:4];

        g_vbuf = [g_dev newBufferWithLength:VBUF_SIZE options:MTLResourceStorageModeShared];
        g_ibuf = [g_dev newBufferWithLength:IBUF_SIZE options:MTLResourceStorageModeShared];

        static const MTLSamplerAddressMode modes[3] = { MTLSamplerAddressModeClampToEdge, MTLSamplerAddressModeRepeat,
                                                       MTLSamplerAddressModeMirrorRepeat };
        for (int s = 0; s < 3; s++)
            for (int t = 0; t < 3; t++)
                for (int l = 0; l < 2; l++) {
                    MTLSamplerDescriptor *sd = [[MTLSamplerDescriptor alloc] init];
                    sd.sAddressMode = modes[s];
                    sd.tAddressMode = modes[t];
                    sd.minFilter = sd.magFilter = l ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
                    sd.mipFilter = MTLSamplerMipFilterNotMipmapped;
                    g_samplers[s][t][l] = [g_dev newSamplerStateWithDescriptor:sd];
                }

        if (!g_efb || !g_depth || !g_vbuf || !g_ibuf || !pipeline(0x300) || !pipeline(1u << 16 | 0x300)) {
            rt_log(0, "metal: resource creation failed, using the software renderer");
            return false;
        }
        g_copy_tex = [NSMutableDictionary dictionary];
        {
            MTLRenderPipelineDescriptor *cp = [[MTLRenderPipelineDescriptor alloc] init];
            cp.vertexFunction = [g_lib newFunctionWithName:@"copy_vs"];
            cp.fragmentFunction = [g_lib newFunctionWithName:@"copy_fs"];
            cp.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
            g_copy_pipe = [g_dev newRenderPipelineStateWithDescriptor:cp error:&err];
            if (!g_copy_pipe) rt_log(0, "metal: copy pipeline failed (%s), EFB copies go through the CPU",
                                     err.localizedDescription.UTF8String);
        }
        rt_gx_set_gpu(&k_metal);
        rt_log(1, "renderer: Metal on %s", g_dev.name.UTF8String);
        return true;
    }
}
