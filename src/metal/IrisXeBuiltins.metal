// IrisXeBuiltins.metal - shaders the driver itself draws with (v0.37.0), compiled with the bundle into
// Contents/Resources/irisxe-builtins.metallib and then by our own compiler like any app's shaders.
#include <metal_stdlib>
using namespace metal;

struct XeBOut { float4 pos [[position]]; float2 uv; };

// one triangle that covers the whole render target: (0,0) (2,0) (0,2) in units of the target
vertex XeBOut irisxe_fullscreen_vs(uint vid [[vertex_id]]) {
    float2 p = float2(float((vid << 1) & 2), float(vid & 2));
    XeBOut o;
    o.pos = float4(p * 2.0f - 1.0f, 0.0f, 1.0f);
    o.uv = float2(p.x, 1.0f - p.y);                 // texture v grows downwards
    return o;
}

// generateMipmapsForTexture: level k - 1 seen alone, linear filter at the centre of 2 x 2 texels
fragment float4 irisxe_copy_fs(XeBOut in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {
    return t.sample(s, in.uv);
}

// v0.57.9: blit copies with Y-tiled textures, drawn: the destination rectangle is the scissor, each pixel reads the
// source texel at (pixel + o.xy) of the source level (bound alone)
fragment float4 irisxe_copyrect_fs(XeBOut in [[stage_in]], texture2d<float> t [[texture(0)]], constant int4 &o [[buffer(0)]]) {
    return t.read(uint2(int2(in.pos.xy) + o.xy));
}
