// irisxe-mtkdemo shaders (v0.39.0): a textured, lit-looking spinning cube, the way a small Xcode Metal app draws
#include <metal_stdlib>
using namespace metal;

struct DemoIn  { float3 pos [[attribute(0)]]; float3 col [[attribute(1)]]; float2 uv [[attribute(2)]]; };
struct DemoOut { float4 pos [[position]]; float3 col; float2 uv; };
struct DemoUniforms { float4x4 mvp; float4 tint; };

vertex DemoOut demo_vs(DemoIn v [[stage_in]], constant DemoUniforms &u [[buffer(1)]]) {
    DemoOut o;
    o.pos = u.mvp * float4(v.pos, 1.0f);
    o.col = v.col * u.tint.rgb;
    o.uv = v.uv;
    return o;
}

fragment float4 demo_fs(DemoOut in [[stage_in]], texture2d<float> tex [[texture(0)]], sampler s [[sampler(0)]]) {
    return float4(in.col, 1.0f) * tex.sample(s, in.uv);
}

fragment float4 demo_fs_plain(DemoOut in [[stage_in]]) { return float4(in.col, 1.0f); }   // v0.40.3: --steps, no texture

// v0.42.6: a copy of Maps' Grid shaders (rebuilt from their AIR) for --steps grid
struct GridIn { float4 position [[attribute(0)]]; };
struct GridOut { float4 position [[position]]; float4 mm; };
struct GridView { float4x4 m; float f; };
struct GridClip { float4x4 m; float4 c; };
struct GridStyle { float4 a; float b, c, d; float4 e; float f; };
vertex GridOut grid_vs(GridIn in [[stage_in]], constant GridView &v [[buffer(1)]], constant GridClip &c [[buffer(2)]],
                       constant GridStyle &s [[buffer(3)]]) {
    GridOut o;
    float4 p = v.m * in.position;
    p.y += s.f * 2.0f * p.w;
    o.position = p;
    float2 q = (c.m * in.position).xy;
    o.mm = float4(q * s.b, s.c * q);
    return o;
}
fragment half4 grid_fs(GridOut in [[stage_in]], constant GridClip &c [[buffer(2)]], constant GridStyle &s [[buffer(3)]]) {
    float4 g = abs(fract(in.mm - 0.5f) - 0.5f);
    float4 w = fwidth(in.mm);
    float4 lo = s.e.xxzz, hi = s.e.yyww;
    half4 t = clamp(half4((g - lo * w) / ((hi - lo) * w)), 0.0h, 1.0h);
    half a = min(t.x, t.y), b = min(t.z, t.w);
    half m = mix(b, a, half(s.d));
    half3 col = half3(s.a.xyz) * (1.0h - m) + half3(c.c.xyz) * m;
    return half4(col, 1.0h);
}

// v0.42.8: the same with Maps' buffer indices (vertex 1, 3, 4; fragment 0, 1)
vertex GridOut grid_vs_m(GridIn in [[stage_in]], constant GridView &v [[buffer(1)]], constant GridClip &c [[buffer(3)]],
                         constant GridStyle &s [[buffer(4)]]) {
    GridOut o;
    float4 p = v.m * in.position;
    p.y += s.f * 2.0f * p.w;
    o.position = p;
    float2 q = (c.m * in.position).xy;
    o.mm = float4(q * s.b, s.c * q);
    return o;
}
fragment half4 grid_fs_m(GridOut in [[stage_in]], constant GridClip &c [[buffer(0)]], constant GridStyle &s [[buffer(1)]]) {
    float4 g = abs(fract(in.mm - 0.5f) - 0.5f);
    float4 w = fwidth(in.mm);
    float4 lo = s.e.xxzz, hi = s.e.yyww;
    half4 t = clamp(half4((g - lo * w) / ((hi - lo) * w)), 0.0h, 1.0h);
    half a = min(t.x, t.y), b = min(t.z, t.w);
    half m = mix(b, a, half(s.d));
    half3 col = half3(s.a.xyz) * (1.0h - m) + half3(c.c.xyz) * m;
    return half4(col, 1.0h);
}
