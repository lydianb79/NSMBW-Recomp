// Shaders.metal - output presentation for the NSMBW recomp.
//
// One full-screen triangle; the fragment shader implements the output
// filters selectable in Preferences > Graphics. Compiled at run time from the
// app bundle (no full Xcode / metal toolchain needed to build the app).
#include <metal_stdlib>
using namespace metal;

struct VOut { float4 pos [[position]]; float2 uv; };

struct Params {
    float2 src_size;      // game image in pixels
    float2 out_size;      // destination viewport in pixels
    int    filter;        // RtFilter
    float  scanline;      // CRT scanline strength 0..1
    float  sharpness;     // sharp-bilinear blend 0..1
    float  pad;
};

vertex VOut present_vs(uint vid [[vertex_id]]) {
    float2 p = float2((vid << 1) & 2, vid & 2);
    VOut o;
    o.pos = float4(p * 2.0 - 1.0, 0.0, 1.0);
    o.uv = float2(p.x, 1.0 - p.y);
    return o;
}

constexpr sampler s_nearest(filter::nearest, address::clamp_to_edge);
constexpr sampler s_linear(filter::linear, address::clamp_to_edge);

static float4 sharp_bilinear(texture2d<float> t, float2 uv, constant Params &p) {
    float2 texel = uv * p.src_size;
    float2 scale = max(floor(p.out_size / p.src_size), float2(1.0));
    float2 tf = floor(texel);
    float2 s = fract(texel);
    float2 region = 0.5 - 0.5 / scale;
    float2 cd = s - 0.5;
    float2 f = (cd - clamp(cd, -region, region)) * scale + 0.5;
    float4 sharp = t.sample(s_linear, (tf + f) / p.src_size);
    float4 soft = t.sample(s_linear, uv);
    return mix(soft, sharp, clamp(0.5 + p.sharpness, 0.0, 1.0));
}

// Scale2x / EPX evaluated per output pixel
static bool same(float4 a, float4 b) { return all(abs(a.rgb - b.rgb) < 0.02); }

static float4 scale2x(texture2d<float> t, float2 uv, constant Params &p) {
    float2 px = 1.0 / p.src_size;
    float2 texel = uv * p.src_size;
    float2 sub = fract(texel);
    float2 c = (floor(texel) + 0.5) * px;
    float4 E = t.sample(s_nearest, c);
    float4 B = t.sample(s_nearest, c + float2(0, -px.y));
    float4 D = t.sample(s_nearest, c + float2(-px.x, 0));
    float4 F = t.sample(s_nearest, c + float2(px.x, 0));
    float4 H = t.sample(s_nearest, c + float2(0, px.y));
    bool left = sub.x < 0.5, top = sub.y < 0.5;
    if (!same(B, H) && !same(D, F)) {
        if (top && left && same(D, B)) return D;
        if (top && !left && same(B, F)) return F;
        if (!top && left && same(D, H)) return D;
        if (!top && !left && same(H, F)) return F;
    }
    return E;
}

static float4 crt(texture2d<float> t, float2 uv, constant Params &p) {
    float2 px = 1.0 / p.src_size;
    // slight horizontal beam spread
    float3 c = t.sample(s_linear, uv).rgb * 0.6
             + t.sample(s_linear, uv + float2(px.x * 0.5, 0)).rgb * 0.2
             + t.sample(s_linear, uv - float2(px.x * 0.5, 0)).rgb * 0.2;
    float line = fract(uv.y * p.src_size.y);
    float scan = mix(1.0, 0.55 + 0.45 * sin(line * M_PI_F), p.scanline);
    // aperture-grille mask at output resolution
    float mx = fmod(floor(uv.x * p.out_size.x), 3.0);
    float3 mask = float3(mx == 0.0 ? 1.0 : 0.82, mx == 1.0 ? 1.0 : 0.82, mx == 2.0 ? 1.0 : 0.82);
    c = c * scan * mask * 1.18;
    return float4(c, 1.0);
}

static float4 lcd(texture2d<float> t, float2 uv, constant Params &p) {
    float4 c = t.sample(s_nearest, uv);
    float2 f = fract(uv * p.src_size);
    float grid = (f.x < 0.08 || f.y < 0.08) ? 0.78 : 1.0;
    return float4(c.rgb * grid, 1.0);
}

fragment float4 present_fs(VOut in [[stage_in]], texture2d<float> tex [[texture(0)]],
                           constant Params &p [[buffer(0)]]) {
    switch (p.filter) {
        case 0: return tex.sample(s_nearest, in.uv);
        case 1: return tex.sample(s_linear, in.uv);
        case 2: return sharp_bilinear(tex, in.uv, p);
        case 3: return scale2x(tex, in.uv, p);
        case 4: return crt(tex, in.uv, p);
        case 5: return lcd(tex, in.uv, p);
        default: return tex.sample(s_linear, in.uv);
    }
}
