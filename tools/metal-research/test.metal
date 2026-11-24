// Tiny Metal shaders used to look at AIR (Apple's LLVM-based shader format)
#include <metal_stdlib>
using namespace metal;

kernel void write_lanes(device uint *out [[buffer(0)]], uint id [[thread_position_in_grid]]) {
    out[id] = 0xC0DE0000u + id;
}

struct VOut { float4 pos [[position]]; float4 color; };

vertex VOut tri_vs(uint vid [[vertex_id]]) {
    float2 p[3] = { float2(0, 0.7), float2(-0.7, -0.7), float2(0.7, -0.7) };
    VOut o; o.pos = float4(p[vid], 0, 1); o.color = float4(vid == 0, vid == 1, vid == 2, 1); return o;
}

fragment float4 tri_fs(VOut in [[stage_in]]) { return in.color; }
