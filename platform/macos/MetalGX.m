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
 *
 * Enhancements (Settings > Graphics):
 *   - internal resolution: the EFB is rendered at N x 640x528. GPU-side
 *     texture copies are made at N x too; CPU readbacks are box-filtered down
 *     to 1x; display copies are kept at full resolution and handed to the
 *     presenter (rt_metal_gx_frame) for the XFB the VI is showing,
 *   - MSAA (2x/4x) on the EFB, resolved before every copy,
 *   - anisotropic filtering: uploaded textures get a mip chain and samplers
 *     use trilinear + anisotropy (gradients come from the un-warped texture
 *     coordinates, so indirect warps do not cause blurry seams).
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>

#include <stdlib.h>
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
"struct TScale { float s[8][2]; int grad; int pad[3]; };\n"
"static float4 tap(int k, float2 uv, float2 gx, float2 gy, bool grad, texture2d<float> t0, texture2d<float> t1, texture2d<float> t2, texture2d<float> t3,\n"
"                 texture2d<float> t4, texture2d<float> t5, texture2d<float> t6, texture2d<float> t7,\n"
"                 sampler s0, sampler s1, sampler s2, sampler s3, sampler s4, sampler s5, sampler s6, sampler s7) {\n"
"  if (!grad) switch (k) {\n"
"  case 0: return t0.sample(s0, uv, level(0)); case 1: return t1.sample(s1, uv, level(0));\n"
"  case 2: return t2.sample(s2, uv, level(0)); case 3: return t3.sample(s3, uv, level(0));\n"
"  case 4: return t4.sample(s4, uv, level(0)); case 5: return t5.sample(s5, uv, level(0));\n"
"  case 6: return t6.sample(s6, uv, level(0)); default: return t7.sample(s7, uv, level(0));\n"
"  }\n"
"  gradient2d g = gradient2d(gx, gy);\n"
"  switch (k) {\n"
"  case 0: return t0.sample(s0, uv, g); case 1: return t1.sample(s1, uv, g);\n"
"  case 2: return t2.sample(s2, uv, g); case 3: return t3.sample(s3, uv, g);\n"
"  case 4: return t4.sample(s4, uv, g); case 5: return t5.sample(s5, uv, g);\n"
"  case 6: return t6.sample(s6, uv, g); default: return t7.sample(s7, uv, g);\n"
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
"fragment float4 gx_fs(VOut in [[stage_in]], constant Tev &T [[buffer(0)]], constant TScale &TS [[buffer(1)]],\n"
"  texture2d<float> x0 [[texture(0)]], texture2d<float> x1 [[texture(1)]], texture2d<float> x2 [[texture(2)]], texture2d<float> x3 [[texture(3)]],\n"
"  texture2d<float> x4 [[texture(4)]], texture2d<float> x5 [[texture(5)]], texture2d<float> x6 [[texture(6)]], texture2d<float> x7 [[texture(7)]],\n"
"  sampler s0 [[sampler(0)]], sampler s1 [[sampler(1)]], sampler s2 [[sampler(2)]], sampler s3 [[sampler(3)]],\n"
"  sampler s4 [[sampler(4)]], sampler s5 [[sampler(5)]], sampler s6 [[sampler(6)]], sampler s7 [[sampler(7)]]) {\n"
"  int4 R[4];\n"
"  for (int r = 0; r < 4; r++) R[r] = int4(T.regs[r][0], T.regs[r][1], T.regs[r][2], T.regs[r][3]);\n"
"  int4 ras0 = clamp(int4(in.c0 + 0.5), 0, 255), ras1 = clamp(int4(in.c1 + 0.5), 0, 255);\n"
"  float2 tc[8] = { in.t0, in.t1, in.t2, in.t3, in.t4, in.t5, in.t6, in.t7 };\n"
"  bool grad = TS.grad != 0;\n"
"  float2 gdx[8], gdy[8];\n"
"  for (int i = 0; i < 8; i++) { gdx[i] = grad ? dfdx(tc[i]) : float2(0.0); gdy[i] = grad ? dfdy(tc[i]) : float2(0.0); }\n"
"  int3 ind[4] = { int3(0), int3(0), int3(0), int3(0) };\n"
"  for (int i = 0; i < T.nind; i++) {\n"
"    if (T.ind_slot[i] < 0) continue;\n"
"    float2 dv = float2(T.ind_div_s[i], T.ind_div_t[i]);\n"
"    int ic = T.ind_coord[i] & 7;\n"
"    float2 uv = tc[ic] * dv;\n"
"    int4 c = int4(tap(T.ind_slot[i], uv, gdx[ic] * dv, gdy[ic] * dv, grad, x0, x1, x2, x3, x4, x5, x6, x7, s0, s1, s2, s3, s4, s5, s6, s7) * 255.0 + 0.5);\n"
"    ind[i] = int3(c.w, c.z, c.y);\n"
"  }\n"
"  float2 prevc = float2(0.0);\n"
"  for (int s = 0; s < T.ntev; s++) {\n"
"    constant Stage &S = T.st[s];\n"
"    int4 tx = int4(255);\n"
"    int bump = 0;\n"
"    float2 suv = tc[S.coord & 7];\n"
"    if (T.ind_active != 0) {\n"
"      float2 td = S.tex_slot >= 0 ? dims(S.tex_slot, x0, x1, x2, x3, x4, x5, x6, x7) /\n"
"                  float2(TS.s[S.tex_slot & 7][0], TS.s[S.tex_slot & 7][1]) : float2(1.0);\n"
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
"      float4 c = tap(S.tex_slot, suv, gdx[S.coord & 7], gdy[S.coord & 7], grad, x0, x1, x2, x3, x4, x5, x6, x7, s0, s1, s2, s3, s4, s5, s6, s7);\n"
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
"}\n"
"struct DownIn { int scale, x, y, pad; };\n"
"fragment float4 down_fs(float4 pos [[position]], constant DownIn &c [[buffer(0)]], texture2d<float> efb [[texture(0)]]) {\n"
"  int2 s0 = (int2(pos.xy) + int2(c.x, c.y)) * c.scale;\n"
"  float4 v = float4(0.0);\n"
"  for (int j = 0; j < c.scale; j++) for (int i = 0; i < c.scale; i++) v += efb.read(uint2(s0 + int2(i, j)));\n"
"  return v / float(c.scale * c.scale);\n"
"}\n";

/* ------------------------------------------------------------ state */
typedef struct { uint64_t hash; uint32_t w, h; uint64_t used_pass; int mips; } MTex;
/* each cache slot has two textures, so a changed texture can be written while
 * the GPU still samples the other one from the previous frame */   /* texture objects live in g_mtex_tex */

static id<MTLDevice> g_dev;
static id<MTLCommandQueue> g_queue;
static id<MTLLibrary> g_lib;
static id<MTLTexture> g_efb, g_depth, g_white;
/* vertex/index data: a ring of buffers, one per submitted command buffer, so
 * the CPU can fill the next while the GPU still reads the previous ones */
#define RING 3
static id<MTLBuffer> g_vbufs[RING], g_ibufs[RING];
static id<MTLCommandBuffer> g_ring_cb[RING];
static int g_ring;
#define g_vbuf g_vbufs[g_ring]
#define g_ibuf g_ibufs[g_ring]
static volatile uint64_t g_done_pass;     /* highest pass whose command buffer has completed */
static id<MTLCommandBuffer> g_last_disp_cb;
/* timing for the title bar (Show FPS) */
static volatile double g_stat_gpu_s, g_stat_wait_s, g_stat_busy_s;
static volatile uint64_t g_stat_frames;
static double now_s(void) { return CACurrentMediaTime(); }
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
static MTex g_mtex_b[TEX_SLOTS];
static id<MTLTexture> g_mtex_tex_b[TEX_SLOTS];
static NSMutableArray<id<MTLTexture>> *g_need_mips;   /* mip chains to build before the frame runs */
static uint64_t g_draws, g_flushes, g_copies_gpu;
static id<MTLRenderPipelineState> g_copy_pipe, g_down_pipe;
static NSMutableDictionary<NSNumber *, id<MTLTexture>> *g_copy_tex;   /* guest address -> copy */

/* enhancement state; render targets are rebuilt between frames when the settings change */
static int g_scale = 1, g_msaa = 1, g_aniso = 1;
static id<MTLTexture> g_efb_ms;       /* MSAA colour (resolves into g_efb), nil without MSAA */
static id<MTLTexture> g_efb1;         /* 1x CPU-readable copy when g_scale > 1 */

/* full-resolution display copies, by XFB address (ring of 3 so the presenter
 * never samples one the game is writing) */
#define DISP_RING 3
typedef struct { id<MTLTexture> tex; uint32_t xfb, w, h; uint64_t seq; } DispCopy;
static DispCopy g_disp[DISP_RING];
static int g_disp_next;
static uint64_t g_disp_seq;
static volatile bool g_display_active;   /* display copies go to the presenter, not the XFB */
static NSLock *g_disp_lock;

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
static MTLRenderPipelineDescriptor *pipe_desc(uint32_t key) {
    MTLRenderPipelineDescriptor *d = [[MTLRenderPipelineDescriptor alloc] init];
    bool clear = (key >> 16) & 1;
    d.vertexFunction = [g_lib newFunctionWithName:clear ? @"clear_vs" : @"gx_vs"];
    d.fragmentFunction = [g_lib newFunctionWithName:clear ? @"clear_fs" : @"gx_fs"];
    d.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    d.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
    d.rasterSampleCount = (NSUInteger)g_msaa;
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
    return d;
}

static id<MTLRenderPipelineState> pipeline(uint32_t key) {
    NSNumber *k = @(key);
    id<MTLRenderPipelineState> p = g_pipes[k];
    if (p) return p;
    NSError *err = nil;
    p = [g_dev newRenderPipelineStateWithDescriptor:pipe_desc(key) error:&err];
    if (!p) { rt_log(0, "metal: pipeline %08X failed: %s", key, err.localizedDescription.UTF8String); return nil; }
    g_pipes[k] = p;
    return p;
}

/* ------------------------------------------------------------ specialised shaders
 * The uber shader above handles any TEV setup, but it loops over 16 stages and
 * picks every input by run-time index, which Apple GPUs execute slowly
 * (registers spill to memory). Like Dolphin, each TEV setup the game uses gets
 * its own straight-line shader with all selectors baked in (colours, konst
 * values and matrices still come from the constant buffer). They are compiled
 * in the background; until one is ready the uber shader draws. */
static NSString *g_spec_prelude;                       /* structs + helpers + gx_vs */
static NSMutableDictionary<NSNumber *, id> *g_spec_fn;  /* tev hash -> MTLFunction | NSNull (compiling / failed) */
static NSMutableDictionary<NSNumber *, id> *g_spec_pipe;/* (hash, key, msaa) -> pipeline | NSNull */
static NSLock *g_spec_lock;
static int g_spec_pending;
static uint64_t g_spec_ready, g_spec_draws, g_uber_draws;

static uint64_t tev_hash(const RtGpuTev *t, int grad) {
    uint64_t h = 1469598103934665603ull;
#define H(v) do { h ^= (uint64_t)(uint32_t)(v); h *= 1099511628211ull; } while (0)
    H(t->ntev); H(t->afunc0); H(t->afunc1); H(t->aop); H(t->nind); H(t->ind_active); H(grad);
    for (int i = 0; i < 4 && i < t->nind; i++) { H(t->ind_slot[i]); H(t->ind_coord[i]); }
    for (int s = 0; s < t->ntev && s < 16; s++) {
        const RtGpuStage *S = &t->st[s];
        for (int i = 0; i < 4; i++) { H(S->csel[i]); H(S->asel[i]); }
        H(S->cbias); H(S->csub); H(S->cclamp); H(S->cshift); H(S->cdst); H(S->ccmp);
        H(S->abias); H(S->asub); H(S->aclamp); H(S->ashift); H(S->adst); H(S->acmp);
        H(S->ras); H(S->tex_slot); H(S->coord);
        if (t->ind_active) { H(S->ibt); H(S->ifmt); H(S->ibias); H(S->ibs); H(S->imid); H(S->isw); H(S->itw); H(S->ifb); }
    }
#undef H
    return h ? h : 1;
}

static NSString *spec_tap(int k, NSString *uv, NSString *gx, NSString *gy, int grad) {
    k &= 7;
    return grad ? [NSString stringWithFormat:@"x%d.sample(s%d, %@, gradient2d(%@, %@))", k, k, uv, gx, gy]
                : [NSString stringWithFormat:@"x%d.sample(s%d, %@, level(0))", k, k, uv];
}

static NSString *spec_csel(int sel, int s) {
    static NSString *const r[] = { @"R0.xyz", @"int3(R0.w)", @"R1.xyz", @"int3(R1.w)", @"R2.xyz", @"int3(R2.w)",
                                   @"R3.xyz", @"int3(R3.w)", @"tx.xyz", @"int3(tx.w)", @"rc.xyz", @"int3(rc.w)",
                                   @"int3(255)", @"int3(128)" };
    if (sel >= 0 && sel <= 13) return r[sel];
    if (sel == 14) return [NSString stringWithFormat:@"int3(T.st[%d].kc[0], T.st[%d].kc[1], T.st[%d].kc[2])", s, s, s];
    return @"int3(0)";
}
static NSString *spec_asel(int sel, int s) {
    static NSString *const r[] = { @"R0.w", @"R1.w", @"R2.w", @"R3.w", @"tx.w", @"rc.w" };
    if (sel >= 0 && sel <= 5) return r[sel];
    if (sel == 6) return [NSString stringWithFormat:@"T.st[%d].kc[3]", s];
    return @"0";
}

static NSString *spec_source(const RtGpuTev *t, int grad) {
    NSMutableString *m = [NSMutableString stringWithString:g_spec_prelude];
    [m appendString:@"static int3 lerp3(int3 a, int3 b, int3 c, int3 d, int bias, int sub, int shift) {\n"
                     "  int3 cc = c + (c >> 7);\n"
                     "  int3 l = (a * (256 - cc) + b * cc) >> 8;\n"
                     "  int3 v = sub != 0 ? d - l : d + l;\n"
                     "  v += bias == 1 ? 128 : (bias == 2 ? -128 : 0);\n"
                     "  return shift == 0 ? v : (shift == 1 ? v * 2 : (shift == 2 ? v * 4 : (v >> 1)));\n"
                     "}\n"
                     "fragment float4 spec_fs(VOut in [[stage_in]], constant Tev &T [[buffer(0)]], constant TScale &TS [[buffer(1)]],\n"
                     "  texture2d<float> x0 [[texture(0)]], texture2d<float> x1 [[texture(1)]], texture2d<float> x2 [[texture(2)]], texture2d<float> x3 [[texture(3)]],\n"
                     "  texture2d<float> x4 [[texture(4)]], texture2d<float> x5 [[texture(5)]], texture2d<float> x6 [[texture(6)]], texture2d<float> x7 [[texture(7)]],\n"
                     "  sampler s0 [[sampler(0)]], sampler s1 [[sampler(1)]], sampler s2 [[sampler(2)]], sampler s3 [[sampler(3)]],\n"
                     "  sampler s4 [[sampler(4)]], sampler s5 [[sampler(5)]], sampler s6 [[sampler(6)]], sampler s7 [[sampler(7)]]) {\n"
                     "  int4 R0 = int4(T.regs[0][0], T.regs[0][1], T.regs[0][2], T.regs[0][3]);\n"
                     "  int4 R1 = int4(T.regs[1][0], T.regs[1][1], T.regs[1][2], T.regs[1][3]);\n"
                     "  int4 R2 = int4(T.regs[2][0], T.regs[2][1], T.regs[2][2], T.regs[2][3]);\n"
                     "  int4 R3 = int4(T.regs[3][0], T.regs[3][1], T.regs[3][2], T.regs[3][3]);\n"
                     "  int4 ras0 = clamp(int4(in.c0 + 0.5), 0, 255), ras1 = clamp(int4(in.c1 + 0.5), 0, 255);\n"
                     "  float2 prevc = float2(0.0); (void)prevc;\n"];
    /* indirect lookups */
    int nind = t->nind < 0 ? 0 : t->nind > 4 ? 4 : t->nind;
    for (int i = 0; i < nind; i++) {
        if (t->ind_slot[i] < 0) { [m appendFormat:@"  int3 ind%d = int3(0);\n", i]; continue; }
        int ic = t->ind_coord[i] & 7;
        [m appendFormat:@"  float2 dv%d = float2(T.ind_div_s[%d], T.ind_div_t[%d]);\n", i, i, i];
        NSString *uv = [NSString stringWithFormat:@"in.t%d * dv%d", ic, i];
        NSString *gx = [NSString stringWithFormat:@"dfdx(in.t%d) * dv%d", ic, i], *gy = [NSString stringWithFormat:@"dfdy(in.t%d) * dv%d", ic, i];
        [m appendFormat:@"  int4 ic%d = int4(%@ * 255.0 + 0.5);\n  int3 ind%d = int3(ic%d.w, ic%d.z, ic%d.y);\n",
                        i, spec_tap(t->ind_slot[i], uv, gx, gy, grad), i, i, i, i];
    }
    for (int s = 0; s < t->ntev && s < 16; s++) {
        const RtGpuStage *S = &t->st[s];
        int co = S->coord & 7;
        [m appendFormat:@"  { /* stage %d */\n    int4 tx = int4(255); int bump = 0; (void)bump;\n    float2 suv = in.t%d;\n", s, co];
        if (t->ind_active) {
            if (S->tex_slot >= 0)
                [m appendFormat:@"    float2 td = float2(x%d.get_width(), x%d.get_height()) / float2(TS.s[%d][0], TS.s[%d][1]);\n",
                                S->tex_slot & 7, S->tex_slot & 7, S->tex_slot & 7, S->tex_slot & 7];
            else [m appendString:@"    float2 td = float2(1.0);\n"];
            [m appendFormat:@"    float2 texel = suv * td;\n    float2 w = float2(wrap_ind(texel.x, %d), wrap_ind(texel.y, %d));\n    float2 off = float2(0.0);\n", S->isw, S->itw];
            if (S->ibt >= 0 && S->ibt < nind) {
                int fm = S->ifmt == 0 ? 255 : S->ifmt == 1 ? 31 : S->ifmt == 2 ? 15 : 7;
                int bm = S->ifmt == 0 ? 0xF8 : S->ifmt == 1 ? 0xE0 : S->ifmt == 2 ? 0xF0 : 0xF8;
                int badd = S->ifmt == 0 ? -128 : 1;
                [m appendFormat:@"    int3 raw = ind%d; int3 crd = raw & %d;\n", S->ibt, fm];
                if (S->ibs >= 1 && S->ibs <= 3) [m appendFormat:@"    bump = raw.%c & %d;\n", "xyz"[S->ibs - 1], bm];
                if (S->ibias & 1) [m appendFormat:@"    crd.x += %d;\n", badd];
                if (S->ibias & 2) [m appendFormat:@"    crd.y += %d;\n", badd];
                if (S->ibias & 4) [m appendFormat:@"    crd.z += %d;\n", badd];
                [m appendString:@"    float3 fc = float3(crd);\n"];
                if (S->imid >= 1 && S->imid <= 3) {
                    int k = S->imid - 1;
                    [m appendFormat:@"    off = float2(T.imtx[%d][0] * fc.x + T.imtx[%d][2] * fc.y + T.imtx[%d][4] * fc.z,\n"
                                     "                 T.imtx[%d][1] * fc.x + T.imtx[%d][3] * fc.y + T.imtx[%d][5] * fc.z) * T.imtx[%d][6];\n",
                                    k, k, k, k, k, k, k];
                } else if (S->imid >= 5 && S->imid <= 7) {
                    [m appendFormat:@"    off = texel * fc.x / 256.0 * T.imtx[%d][6];\n", S->imid - 5];
                } else if (S->imid >= 9 && S->imid <= 11) {
                    [m appendFormat:@"    off = texel * fc.y / 256.0 * T.imtx[%d][6];\n", S->imid - 9];
                }
            }
            [m appendString:@"    float2 f = w + off;\n"];
            if (S->ifb) [m appendString:@"    f += prevc;\n"];
            [m appendString:@"    prevc = f;\n    suv = f / td;\n"];
        }
        if (S->tex_slot >= 0) {
            NSString *gx = [NSString stringWithFormat:@"dfdx(in.t%d)", co], *gy = [NSString stringWithFormat:@"dfdy(in.t%d)", co];
            [m appendFormat:@"    tx = int4(%@ * 255.0 + 0.5);\n", spec_tap(S->tex_slot, @"suv", gx, gy, grad)];
        }
        switch (S->ras) {
        case 0: [m appendString:@"    int4 rc = ras0;\n"]; break;
        case 1: [m appendString:@"    int4 rc = ras1;\n"]; break;
        case 3: [m appendString:@"    int4 rc = int4(bump);\n"]; break;
        case 4: [m appendString:@"    int4 rc = int4(bump | (bump >> 5));\n"]; break;
        case 2: [m appendString:@"    int4 rc = int4(0);\n"]; break;
        default: [m appendString:@"    int4 rc = int4(bump);\n"]; break;      /* 5..7: as the uber shader */
        }
        [m appendString:@"    (void)rc; (void)tx;\n"];
        [m appendFormat:@"    int3 ca = %@ & 255, cb = %@ & 255, cc = %@ & 255, cd = %@;\n",
                        spec_csel(S->csel[0], s), spec_csel(S->csel[1], s), spec_csel(S->csel[2], s), spec_csel(S->csel[3], s)];
        if (S->ccmp) [m appendString:@"    int3 cr = cd + select(int3(0), cc, ca > cb);\n"];
        else [m appendFormat:@"    int3 cr = lerp3(ca, cb, cc, cd, %d, %d, %d);\n", S->cbias, S->csub, S->cshift];
        [m appendFormat:@"    cr = %@;\n", S->cclamp ? @"clamp(cr, 0, 255)" : @"clamp(cr, -1024, 1023)"];
        [m appendFormat:@"    int aa = %@ & 255, ab = %@ & 255, ac = %@ & 255, ad = %@;\n",
                        spec_asel(S->asel[0], s), spec_asel(S->asel[1], s), spec_asel(S->asel[2], s), spec_asel(S->asel[3], s)];
        if (S->acmp) [m appendString:@"    int ar = ad + (aa > ab ? ac : 0);\n"];
        else [m appendFormat:@"    int ar = tev_lerp(aa, ab, ac, ad, %d, %d, %d);\n", S->abias, S->asub, S->ashift];
        [m appendFormat:@"    ar = %@;\n", S->aclamp ? @"clamp(ar, 0, 255)" : @"clamp(ar, -1024, 1023)"];
        [m appendFormat:@"    R%d.xyz = cr;\n    R%d.w = ar;\n  }\n", S->cdst & 3, S->adst & 3];
    }
    [m appendFormat:@"  int4 o = clamp(R0, 0, 255);\n"
                     "  bool t0 = acmp(%d, o.w, T.aref0), t1 = acmp(%d, o.w, T.aref1);\n", t->afunc0, t->afunc1];
    static NSString *const ops[] = { @"t0 && t1", @"t0 || t1", @"t0 != t1", @"t0 == t1" };
    [m appendFormat:@"  if (!(%@)) discard_fragment();\n  return float4(o) / 255.0;\n}\n", ops[t->aop & 3]];
    return m;
}

/* returns the specialised pipeline for this draw if it is ready, else nil
 * (and starts building it) */
static id<MTLRenderPipelineState> spec_pipeline(const RtGpuTev *t, uint32_t key, int grad) {
    if (!g_spec_lock) return nil;
    uint64_t h = tev_hash(t, grad);
    NSNumber *pk = @(h ^ ((uint64_t)key << 1) ^ ((uint64_t)g_msaa << 40));
    [g_spec_lock lock];
    id p = g_spec_pipe[pk];
    id fn = p ? nil : g_spec_fn[@(h)];
    [g_spec_lock unlock];
    if (p) return p == (id)[NSNull null] ? nil : p;
    if (fn == (id)[NSNull null]) return nil;                 /* compiling, or failed */
    if (!fn) {
        if (g_spec_pending >= 8) return nil;                 /* don't flood the compiler */
        [g_spec_lock lock]; g_spec_fn[@(h)] = [NSNull null]; g_spec_pending++; [g_spec_lock unlock];
        NSString *src = spec_source(t, grad);
        [g_dev newLibraryWithSource:src options:[[MTLCompileOptions alloc] init]
                  completionHandler:^(id<MTLLibrary> lib, NSError *err) {
            id<MTLFunction> f = [lib newFunctionWithName:@"spec_fs"];
            [g_spec_lock lock];
            g_spec_pending--;
            if (f) g_spec_fn[@(h)] = f;                      /* else stays NSNull: uber shader for this one */
            [g_spec_lock unlock];
            if (!f) rt_log(0, "metal: specialised shader failed (uber shader keeps drawing it): %s",
                           err.localizedDescription.UTF8String);
        }];
        return nil;
    }
    /* function ready: build the pipeline in the background too */
    [g_spec_lock lock]; g_spec_pipe[pk] = [NSNull null]; [g_spec_lock unlock];
    MTLRenderPipelineDescriptor *d = pipe_desc(key);
    d.fragmentFunction = fn;
    [g_dev newRenderPipelineStateWithDescriptor:d completionHandler:^(id<MTLRenderPipelineState> ps, NSError *err) {
        if (!ps) return;                                     /* stays NSNull: uber shader */
        [g_spec_lock lock]; g_spec_pipe[pk] = ps; g_spec_ready++; [g_spec_lock unlock];
    }];
    return nil;
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
    int mips = g_aniso > 1 && (t->w > 1 || t->h > 1);
    if (g_mtex_tex[slot] && e->hash == t->hash && e->w == t->w && e->h == t->h && e->mips == mips) {
        e->used_pass = g_pass; return g_mtex_tex[slot];
    }
    /* never overwrite a texture that queued or in-flight draws still sample:
     * swap to the slot's second texture, or make a new one */
    if (g_mtex_tex[slot] && e->used_pass > g_done_pass) {
        MTex tmp = *e; *e = g_mtex_b[slot]; g_mtex_b[slot] = tmp;
        id<MTLTexture> tt = g_mtex_tex[slot]; g_mtex_tex[slot] = g_mtex_tex_b[slot]; g_mtex_tex_b[slot] = tt;
        if (g_mtex_tex[slot] && e->used_pass > g_done_pass) g_mtex_tex[slot] = nil;
    }
    if (!g_mtex_tex[slot] || e->w != t->w || e->h != t->h || e->mips != mips) {
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                     width:t->w height:t->h mipmapped:mips ? YES : NO];
        td.usage = MTLTextureUsageShaderRead;
        td.storageMode = MTLStorageModeShared;
        g_mtex_tex[slot] = [g_dev newTextureWithDescriptor:td];
        e->w = t->w; e->h = t->h; e->mips = mips;
    }
    id<MTLTexture> tex = g_mtex_tex[slot];
    [tex replaceRegion:MTLRegionMake2D(0, 0, t->w, t->h) mipmapLevel:0 withBytes:t->rgba bytesPerRow:t->w * 4];
    if (mips) [g_need_mips addObject:tex];   /* built on the GPU just before this frame's work */
    e->hash = t->hash;
    e->used_pass = g_pass;
    return tex;
}

static void begin_pass(void) {
    if (g_enc) return;
    if (!g_cb) g_cb = [g_queue commandBuffer];
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    MTLLoadAction load = g_efb_ready ? MTLLoadActionLoad : MTLLoadActionClear;
    if (g_efb_ms) {
        rp.colorAttachments[0].texture = g_efb_ms;
        rp.colorAttachments[0].resolveTexture = g_efb;
        rp.colorAttachments[0].storeAction = MTLStoreActionStoreAndMultisampleResolve;
    } else {
        rp.colorAttachments[0].texture = g_efb;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    }
    rp.colorAttachments[0].loadAction = load;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
    rp.depthAttachment.texture = g_depth;
    rp.depthAttachment.loadAction = load;
    rp.depthAttachment.clearDepth = 1.0;
    rp.depthAttachment.storeAction = MTLStoreActionStore;
    g_efb_ready = true;
    g_enc = [g_cb renderCommandEncoderWithDescriptor:rp];
    [g_enc setViewport:(MTLViewport){ 0, 0, (double)(EFB_W * g_scale), (double)(EFB_H * g_scale), 0, 1 }];
    [g_enc setDepthClipMode:MTLDepthClipModeClamp];
    for (int i = 0; i < 8; i++) {
        [g_enc setFragmentTexture:g_white atIndex:i];
        [g_enc setFragmentSamplerState:g_samplers[0][0][0] atIndex:i];
    }
}

/* end the open EFB pass (resolving MSAA) so the EFB can be read by a copy */
static void end_pass(void) {
    begin_pass();
    [g_enc endEncoding];
    g_enc = nil;
}

/* Commit the queued work. wait: block until the GPU is done (CPU readbacks).
 * publish >= 0: display copy slot to hand to the presenter once complete. */
static void submit(bool wait, int publish) {
    if (g_enc || g_cb) {
        if (!g_cb) g_cb = [g_queue commandBuffer];
        if (g_enc) [g_enc endEncoding];
        g_enc = nil;
        if (wait && g_efb.storageMode == MTLStorageModeManaged) {   /* Intel: make the GPU copy visible to the CPU */
            id<MTLBlitCommandEncoder> b = [g_cb blitCommandEncoder];
            [b synchronizeResource:g_efb];
            [b endEncoding];
        }
        if (g_need_mips.count) {
            /* committed before g_cb, so it runs first (one queue runs command buffers in commit order) */
            id<MTLCommandBuffer> mc = [g_queue commandBuffer];
            id<MTLBlitCommandEncoder> b = [mc blitCommandEncoder];
            for (id<MTLTexture> t in g_need_mips) [b generateMipmapsForTexture:t];
            [b endEncoding];
            [mc commit];
            [g_need_mips removeAllObjects];
        }
        uint64_t pass = g_pass;
        [g_cb addCompletedHandler:^(id<MTLCommandBuffer> cb) {
            if (cb.error) rt_log(0, "metal: command buffer error: %s", cb.error.localizedDescription.UTF8String);
            double gt = cb.GPUEndTime - cb.GPUStartTime;
            if (gt > 0 && gt < 1.0) g_stat_gpu_s += gt;
            if (pass > g_done_pass) g_done_pass = pass;            /* completions arrive in order */
            if (publish >= 0) {
                [g_disp_lock lock];
                g_disp[publish].seq = ++g_disp_seq;
                [g_disp_lock unlock];
            }
        }];
        [g_cb commit];
        g_ring_cb[g_ring] = g_cb;
        if (wait) [g_cb waitUntilCompleted];
        g_cb = nil;
        g_flushes++;
        g_ring = (g_ring + 1) % RING;
        if (g_ring_cb[g_ring]) { [g_ring_cb[g_ring] waitUntilCompleted]; g_ring_cb[g_ring] = nil; }
    }
    g_pass++;
    g_voff = g_ioff = 0;
}
static void flush(void) { submit(true, -1); }

static void set_scissor(int x0, int y0, int x1, int y1) {
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > EFB_W - 1) x1 = EFB_W - 1;
    if (y1 > EFB_H - 1) y1 = EFB_H - 1;
    if (x1 < x0 || y1 < y0) { x0 = y0 = 0; x1 = y1 = 0; }
    NSUInteger S = (NSUInteger)g_scale;
    [g_enc setScissorRect:(MTLScissorRect){ (NSUInteger)x0 * S, (NSUInteger)y0 * S,
                                            (NSUInteger)(x1 - x0 + 1) * S, (NSUInteger)(y1 - y0 + 1) * S }];
}

/* ------------------------------------------------------------ render targets / settings */
static void make_samplers(void) {
    static const MTLSamplerAddressMode modes[3] = { MTLSamplerAddressModeClampToEdge, MTLSamplerAddressModeRepeat,
                                                   MTLSamplerAddressModeMirrorRepeat };
    for (int s = 0; s < 3; s++)
        for (int t = 0; t < 3; t++)
            for (int l = 0; l < 2; l++) {
                MTLSamplerDescriptor *sd = [[MTLSamplerDescriptor alloc] init];
                sd.sAddressMode = modes[s];
                sd.tAddressMode = modes[t];
                sd.minFilter = sd.magFilter = l ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
                if (g_aniso > 1) {
                    sd.mipFilter = l ? MTLSamplerMipFilterLinear : MTLSamplerMipFilterNearest;
                    if (l) sd.maxAnisotropy = (NSUInteger)g_aniso;
                } else {
                    sd.mipFilter = MTLSamplerMipFilterNotMipmapped;
                }
                g_samplers[s][t][l] = [g_dev newSamplerStateWithDescriptor:sd];
            }
}

static bool make_targets(int scale, int msaa) {
    NSUInteger W = (NSUInteger)(EFB_W * scale), H = (NSUInteger)(EFB_H * scale);
    MTLTextureDescriptor *cd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                  width:W height:H mipmapped:NO];
    cd.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    id<MTLTexture> efb = nil, efb1 = nil, ms = nil, depth = nil;
    /* Apple silicon: CPU readback straight from a shared texture; Intel Macs
     * do not allow shared textures, so use managed + synchronizeResource */
    MTLStorageMode cpu_mode = g_dev.hasUnifiedMemory ? MTLStorageModeShared : MTLStorageModeManaged;
    if (scale == 1) {
        cd.storageMode = cpu_mode;
        efb = [g_dev newTextureWithDescriptor:cd];
    } else {
        cd.storageMode = MTLStorageModePrivate;
        efb = [g_dev newTextureWithDescriptor:cd];
        MTLTextureDescriptor *d1 = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                      width:EFB_W height:EFB_H mipmapped:NO];
        d1.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        d1.storageMode = cpu_mode;
        efb1 = [g_dev newTextureWithDescriptor:d1];
        if (!efb1) return false;
    }
    if (!efb) return false;
    if (msaa > 1) {
        MTLTextureDescriptor *md = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                      width:W height:H mipmapped:NO];
        md.textureType = MTLTextureType2DMultisample;
        md.sampleCount = (NSUInteger)msaa;
        md.usage = MTLTextureUsageRenderTarget;
        md.storageMode = MTLStorageModePrivate;
        ms = [g_dev newTextureWithDescriptor:md];
        if (!ms) return false;
    }
    MTLTextureDescriptor *dd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                                                                  width:W height:H mipmapped:NO];
    dd.usage = MTLTextureUsageRenderTarget;
    dd.storageMode = MTLStorageModePrivate;
    if (msaa > 1) { dd.textureType = MTLTextureType2DMultisample; dd.sampleCount = (NSUInteger)msaa; }
    depth = [g_dev newTextureWithDescriptor:dd];
    if (!depth) return false;
    g_efb = efb; g_efb1 = efb1; g_efb_ms = ms; g_depth = depth;
    if (msaa != g_msaa) [g_pipes removeAllObjects];      /* sample count is baked into pipelines */
    g_scale = scale; g_msaa = msaa;
    g_efb_ready = false;
    return true;
}

/* called between frames: picks up changed Settings > Graphics values */
static void apply_settings(void) {
    int scale = rt_settings.internal_res < 1 ? 1 : rt_settings.internal_res > 8 ? 8 : rt_settings.internal_res;
    const char *e = getenv("RT_INTERNAL_RES");
    if (e && atoi(e) >= 1 && atoi(e) <= 8) scale = atoi(e);
    if (!g_down_pipe) scale = 1;                     /* no way to read a scaled EFB back */
    int msaa = rt_settings.msaa >= 4 ? 4 : rt_settings.msaa >= 2 ? 2 : 1;
    while (msaa > 1 && ![g_dev supportsTextureSampleCount:(NSUInteger)msaa]) msaa /= 2;
    while (msaa > 1 && scale * scale * msaa > 64) msaa /= 2;      /* keep the EFB within ~350 MB */
    int aniso = rt_settings.anisotropy >= 16 ? 16 : rt_settings.anisotropy >= 8 ? 8 : rt_settings.anisotropy >= 4 ? 4 :
                rt_settings.anisotropy >= 2 ? 2 : 1;
    if (aniso != g_aniso) { g_aniso = aniso; make_samplers(); }
    static int failed_s, failed_m;                   /* don't retry a failed size every frame */
    if ((scale != g_scale || msaa != g_msaa || !g_efb) && !(scale == failed_s && msaa == failed_m && g_efb)) {
        int old_s = g_scale, old_m = g_msaa;
        if (!make_targets(scale, msaa)) {
            rt_log(0, "metal: %dx / %dx MSAA render targets failed, keeping %dx / %dx", scale, msaa, old_s, old_m);
            failed_s = scale; failed_m = msaa;
            if (!g_efb) make_targets(1, 1);
        } else {
            rt_log(1, "metal: internal resolution %dx (%dx%d), MSAA %dx, anisotropy %dx",
                   g_scale, EFB_W * g_scale, EFB_H * g_scale, g_msaa, g_aniso);
        }
    }
}

/* ------------------------------------------------------------ sink */
static void mtl_draw(const RtGpuDraw *d) {
    @autoreleasepool {
        uint32_t vbytes = d->nv * (uint32_t)sizeof(Vtx), ibytes = d->ntri * 3u * 4u;
        if (vbytes > VBUF_SIZE || ibytes > IBUF_SIZE) return;
        if (g_voff + vbytes > VBUF_SIZE || g_ioff + ibytes > IBUF_SIZE) submit(false, -1);
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
        static int uber_only = -1;
        if (uber_only < 0) uber_only = getenv("RT_UBERSHADER_ONLY") != NULL;
        id<MTLRenderPipelineState> sp = uber_only ? nil : spec_pipeline(&d->tev, key, g_aniso > 1);
        if (sp) { p = sp; g_spec_draws++; } else g_uber_draws++;
        [g_enc setRenderPipelineState:p];
        [g_enc setDepthStencilState:depth_state(d->z_enable, d->z_func, d->z_update)];
        [g_enc setCullMode:MTLCullModeNone];
        set_scissor(d->sx0, d->sy0, d->sx1, d->sy1);
        [g_enc setVertexBuffer:g_vbuf offset:g_voff atIndex:0];
        [g_enc setFragmentBytes:&d->tev length:sizeof d->tev atIndex:0];
        /* physical / logical size of each texture: EFB copies are stored at the internal resolution */
        struct { float s[8][2]; int32_t grad, pad[3]; } ts;
        float (*tscale)[2] = ts.s;
        ts.grad = g_aniso > 1; ts.pad[0] = ts.pad[1] = ts.pad[2] = 0;
        for (int i = 0; i < 8; i++) tscale[i][0] = tscale[i][1] = 1.0f;
        for (int i = 0; i < d->ntex && i < 8; i++) {
            const RtGpuTex *t = &d->tex[i];
            if (!t->w || !t->h) continue;
            id<MTLTexture> tex = nil;
            if (!t->rgba) tex = g_copy_tex[@(t->efb_copy)];
            else tex = upload(t);
            if (!tex) continue;
            tscale[i][0] = (float)tex.width / (float)t->w;
            tscale[i][1] = (float)tex.height / (float)t->h;
            [g_enc setFragmentTexture:tex atIndex:(NSUInteger)i];
            int ws = t->wrap_s < 0 || t->wrap_s > 2 ? 0 : t->wrap_s, wt = t->wrap_t < 0 || t->wrap_t > 2 ? 0 : t->wrap_t;
            [g_enc setFragmentSamplerState:g_samplers[ws][wt][t->linear ? 1 : 0] atIndex:(NSUInteger)i];
        }
        [g_enc setFragmentBytes:&ts length:sizeof ts atIndex:1];
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
        id<MTLTexture> src = g_efb;
        if (g_scale > 1 && g_efb_ready && w > 0 && h > 0 && g_down_pipe) {
            /* box-filter the scaled EFB down to 1x for the CPU */
            end_pass();
            MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture = g_efb1;
            rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            id<MTLRenderCommandEncoder> e = [g_cb renderCommandEncoderWithDescriptor:rp];
            struct { int32_t scale, x, y, pad; } c = { g_scale, 0, 0, 0 };
            [e setRenderPipelineState:g_down_pipe];
            [e setScissorRect:(MTLScissorRect){ (NSUInteger)x, (NSUInteger)y, (NSUInteger)w, (NSUInteger)h }];
            [e setFragmentBytes:&c length:sizeof c atIndex:0];
            [e setFragmentTexture:g_efb atIndex:0];
            [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
            [e endEncoding];
            if (g_efb1.storageMode == MTLStorageModeManaged) {
                id<MTLBlitCommandEncoder> b = [g_cb blitCommandEncoder];
                [b synchronizeResource:g_efb1];
                [b endEncoding];
            }
            src = g_efb1;
        }
        flush();
        if (g_efb_ready && w > 0 && h > 0) {
            if (stride == w) {
                [src getBytes:dst bytesPerRow:(NSUInteger)w * 4 fromRegion:MTLRegionMake2D((NSUInteger)x, (NSUInteger)y, (NSUInteger)w, (NSUInteger)h) mipmapLevel:0];
            } else {
                for (int r = 0; r < h; r++)
                    [src getBytes:dst + (size_t)r * stride bytesPerRow:(NSUInteger)w * 4
                       fromRegion:MTLRegionMake2D((NSUInteger)x, (NSUInteger)(y + r), (NSUInteger)w, 1) mipmapLevel:0];
            }
        }
        apply_settings();      /* nothing is queued here: safe point to rebuild targets */
    }
}

/* EFB -> texture on the GPU: end the EFB pass, render the converted rectangle
 * into the copy's own texture, continue. No CPU wait. */
static int mtl_copy_tex(int x, int y, int w, int h, int half, int fmt, int no_alpha, uint32_t dest, uint32_t tw, uint32_t th) {
    if (!g_copy_pipe || !tw || !th || tw > 1024 || th > 1024) return 0;
    @autoreleasepool {
        (void)w; (void)h;
        NSUInteger pw = tw * (NSUInteger)g_scale, ph = th * (NSUInteger)g_scale;
        NSNumber *key = @(dest);
        id<MTLTexture> t = g_copy_tex[key];
        if (!t || t.width != pw || t.height != ph) {
            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                          width:pw height:ph mipmapped:NO];
            td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
            td.storageMode = MTLStorageModePrivate;
            t = [g_dev newTextureWithDescriptor:td];
            if (!t) return 0;
            g_copy_tex[key] = t;
        }
        end_pass();                        /* make sure everything drawn so far is encoded (and resolved) */
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = t;
        rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> e = [g_cb renderCommandEncoderWithDescriptor:rp];
        struct { int32_t x, y, half_, fmt, no_alpha, pad[3]; } c = { x * g_scale, y * g_scale, half, fmt, no_alpha, { 0, 0, 0 } };
        [e setRenderPipelineState:g_copy_pipe];
        [e setFragmentBytes:&c length:sizeof c atIndex:0];
        [e setFragmentTexture:g_efb atIndex:0];
        [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
        [e endEncoding];
        g_copies_gpu++;
        return 1;
    }
}

/* EFB -> XFB, kept on the GPU: the EFB rectangle is copied (at the internal
 * resolution) into a display texture that the presenter shows for this XFB
 * address. Nothing is read back and the CPU does not wait for the GPU: the
 * frame's work is committed and the game goes on with the next frame (at most
 * one frame ahead). Returns 1 when handled, so the front end skips the
 * readback and YUV encoding. */
static int mtl_display(int x, int y, int w, int h, uint32_t xfb) {
    static int cpu_xfb = -1;
    if (cpu_xfb < 0) cpu_xfb = getenv("RT_XFB_READBACK") != NULL;
    if (cpu_xfb || w <= 0 || h <= 0) return 0;
    @autoreleasepool {
        int i = g_disp_next;
        g_disp_next = (g_disp_next + 1) % DISP_RING;
        NSUInteger pw = (NSUInteger)(w * g_scale), ph = (NSUInteger)(h * g_scale);
        [g_disp_lock lock];
        g_disp[i].seq = 0;                 /* hidden from the presenter until it is complete */
        id<MTLTexture> t = g_disp[i].tex;
        [g_disp_lock unlock];
        if (!t || t.width != pw || t.height != ph) {
            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                          width:pw height:ph mipmapped:g_scale > 1];
            td.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
            td.storageMode = MTLStorageModePrivate;
            t = [g_dev newTextureWithDescriptor:td];
            if (!t) return 0;
        }
        end_pass();
        id<MTLBlitCommandEncoder> b = [g_cb blitCommandEncoder];
        [b copyFromTexture:g_efb sourceSlice:0 sourceLevel:0
              sourceOrigin:MTLOriginMake((NSUInteger)(x * g_scale), (NSUInteger)(y * g_scale), 0)
                sourceSize:MTLSizeMake(pw, ph, 1)
                 toTexture:t destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
        if (t.mipmapLevelCount > 1) [b generateMipmapsForTexture:t];
        [b endEncoding];
        [g_disp_lock lock];
        g_disp[i].tex = t;
        g_disp[i].xfb = xfb & 0x1FFFFFFFu;
        g_disp[i].w = (uint32_t)w; g_disp[i].h = (uint32_t)h;
        [g_disp_lock unlock];
        /* keep the CPU at most one frame ahead of the GPU */
        id<MTLCommandBuffer> prev = g_last_disp_cb;
        g_last_disp_cb = g_cb;
        submit(false, i);
        double t0 = now_s();
        if (prev) [prev waitUntilCompleted];
        double t1 = now_s();
        static double last_end;
        if (last_end > 0) { g_stat_busy_s += t0 - last_end; g_stat_wait_s += t1 - t0; }
        last_end = t1;
        g_stat_frames++;
        g_display_active = true;
        static uint64_t n;
        if ((++n % 600) == 0) rt_log(1, "metal: %llu draws (%llu specialised, %llu uber), %llu submits, %llu GPU texture copies, %llu shaders",
                                     (unsigned long long)g_draws, (unsigned long long)g_spec_draws, (unsigned long long)g_uber_draws,
                                     (unsigned long long)g_flushes, (unsigned long long)g_copies_gpu, (unsigned long long)g_spec_ready);
        apply_settings();      /* nothing is queued here: safe point to rebuild targets */
        return 1;
    }
}

static const RtGpuSink k_metal = { mtl_draw, mtl_clear, mtl_readback, mtl_copy_tex, mtl_display };

id<MTLDevice> rt_metal_gx_device(void) { return g_dev; }
bool rt_metal_gx_display_active(void) { return g_display_active; }

bool rt_metal_gx_stats(double *gpu_ms, double *gx_ms, double *wait_ms, uint64_t *frames) {
    static double pg, pw, pb; static uint64_t pf;
    uint64_t f = g_stat_frames;
    if (f == pf) return false;
    double n = (double)(f - pf);
    if (gpu_ms) *gpu_ms = (g_stat_gpu_s - pg) * 1000.0 / n;
    if (gx_ms) *gx_ms = (g_stat_busy_s - pb) * 1000.0 / n;
    if (wait_ms) *wait_ms = (g_stat_wait_s - pw) * 1000.0 / n;
    if (frames) *frames = f;
    pg = g_stat_gpu_s; pw = g_stat_wait_s; pb = g_stat_busy_s; pf = f;
    return true;
}

id<MTLTexture> rt_metal_gx_frame(uint32_t xfb, uint32_t *w, uint32_t *h) {
    if (!g_disp_lock) return nil;
    xfb &= 0x1FFFFFFFu;
    id<MTLTexture> best = nil;
    uint64_t best_seq = 0;
    [g_disp_lock lock];
    for (int i = 0; i < DISP_RING; i++)
        if (g_disp[i].seq > best_seq && g_disp[i].xfb == xfb && g_disp[i].tex) {
            best = g_disp[i].tex; best_seq = g_disp[i].seq;
            if (w) *w = g_disp[i].w;
            if (h) *h = g_disp[i].h;
        }
    [g_disp_lock unlock];
    return best;
}

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
        g_disp_lock = [[NSLock alloc] init];
        g_need_mips = [NSMutableArray array];
        {
            NSRange r = [kShaderSource rangeOfString:@"fragment float4 gx_fs"];
            g_spec_prelude = r.location != NSNotFound ? [kShaderSource substringToIndex:r.location] : nil;
            if (g_spec_prelude) {
                g_spec_fn = [NSMutableDictionary dictionary];
                g_spec_pipe = [NSMutableDictionary dictionary];
                g_spec_lock = [[NSLock alloc] init];
            }
        }

        MTLTextureDescriptor *wd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                      width:1 height:1 mipmapped:NO];
        wd.usage = MTLTextureUsageShaderRead;
        g_white = [g_dev newTextureWithDescriptor:wd];
        uint32_t white = 0xFFFFFFFFu;
        [g_white replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:&white bytesPerRow:4];

        for (int r = 0; r < RING; r++) {
            g_vbufs[r] = [g_dev newBufferWithLength:VBUF_SIZE options:MTLResourceStorageModeShared];
            g_ibufs[r] = [g_dev newBufferWithLength:IBUF_SIZE options:MTLResourceStorageModeShared];
            if (!g_vbufs[r] || !g_ibufs[r]) { rt_log(0, "metal: buffer allocation failed"); return false; }
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
            cp.fragmentFunction = [g_lib newFunctionWithName:@"down_fs"];
            g_down_pipe = [g_dev newRenderPipelineStateWithDescriptor:cp error:&err];
            if (!g_down_pipe) rt_log(0, "metal: downsample pipeline failed (%s)", err.localizedDescription.UTF8String);
        }
        g_scale = 0;               /* force apply_settings to build the targets */
        g_aniso = 0;
        apply_settings();

        if (!g_efb || !g_depth || !g_vbuf || !g_ibuf || !pipeline(0x300) || !pipeline(1u << 16 | 0x300)) {
            rt_log(0, "metal: resource creation failed, using the software renderer");
            return false;
        }
        rt_gx_set_gpu(&k_metal);
        rt_log(1, "renderer: Metal on %s", g_dev.name.UTF8String);
        return true;
    }
}
