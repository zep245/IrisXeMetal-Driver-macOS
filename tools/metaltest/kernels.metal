// Metal compute kernels for irisxe-metaltest step [12] (milestone 3c): real
// Metal shaders, compiled to AIR by Apple's offline compiler (in the build)
// and to Iris Xe code by our driver's compiler at run time.
#include <metal_stdlib>
using namespace metal;

kernel void add1(device const uint *in [[buffer(0)]], device uint *out [[buffer(1)]],
                 uint i [[thread_position_in_grid]]) { out[i] = in[i] + 1; }

kernel void saxpy(device const float *x [[buffer(0)]], device float *y [[buffer(1)]],
                  constant float &a [[buffer(2)]], uint i [[thread_position_in_grid]]) { y[i] = a * x[i] + y[i]; }

kernel void clamp_branch(device int *v [[buffer(0)]], uint i [[thread_position_in_grid]]) {
    int x = v[i];
    if (x < 0) x = -x; else x = x * 2;
    v[i] = x;
}

kernel void int_ops(device const uint *in [[buffer(0)]], device uint *out [[buffer(1)]],
                    uint i [[thread_position_in_grid]]) {
    uint x = in[i];
    out[i] = ((x * 7u + (x >> 3)) ^ 0x55u) * x;
}

kernel void to_float(device const int *in [[buffer(0)]], device float *out [[buffer(1)]],
                     uint i [[thread_position_in_grid]]) { out[i] = float(in[i]) * 0.5f + 1.0f; }

// ---- milestone 3d: branches and loops ----
kernel void loop_sum(device const uint *in [[buffer(0)]], device uint *out [[buffer(1)]],
                     constant uint &n [[buffer(2)]], uint i [[thread_position_in_grid]]) {
    uint s = 0;
    for (uint k = 0; k < n; k++) s += in[i * n + k];
    out[i] = s;
}

kernel void collatz(device uint *v [[buffer(0)]], uint i [[thread_position_in_grid]]) {
    uint x = v[i], n = 0;
    while (x > 1) { x = (x & 1) ? 3 * x + 1 : x / 2; n++; }
    v[i] = n;
}

kernel void if_chain(device int *v [[buffer(0)]], device int *side [[buffer(1)]], uint i [[thread_position_in_grid]]) {
    int x = v[i], r;
    if (x > 10) { r = x * 3; if (x > 100) r += 1000; else r -= x << 2; }
    else { r = -x; side[i] = r + 7; }
    v[i] = r;
}

kernel void nested_loops(device const uint *in [[buffer(0)]], device uint *out [[buffer(1)]],
                         constant uint &n [[buffer(2)]], uint i [[thread_position_in_grid]]) {
    uint s = 0;
    for (uint a = 0; a < n; a++)
        for (uint b = 0; b <= a; b++) s += in[(i + a * 7 + b) & 1023] ^ b;
    out[i] = s;
}

kernel void loop_if(device const int *in [[buffer(0)]], device int *out [[buffer(1)]],
                    constant uint &n [[buffer(2)]], uint i [[thread_position_in_grid]]) {
    int acc = 0, hits = 0;
    for (uint k = 0; k < n; k++) {
        int x = in[(i + k * 13) & 1023];
        if (x > 0) { acc += x; hits++; }
        else if (x < -150) { acc -= 1; }
    }
    out[i] = acc * 16 + hits;
}

// ---- milestone 3e: maths and types ----
kernel void math_f(device const float *in [[buffer(0)]], device float *out [[buffer(1)]],
                   uint i [[thread_position_in_grid]]) {
    float x = in[i];
    out[i * 4 + 0] = sqrt(x) + rsqrt(x + 1.0f) + x / (x + 3.0f);
    out[i * 4 + 1] = sin(x) * cos(x * 0.5f);
    out[i * 4 + 2] = exp2(x * 0.1f) + log2(x + 1.0f) + exp(x * 0.05f) + log(x + 2.0f);
    out[i * 4 + 3] = pow(x, 1.5f) + floor(x * 1.7f) + ceil(x * 0.3f) + fract(x * 3.3f) + trunc(-x * 0.7f);
}

kernel void div_mod(device const int *in [[buffer(0)]], device int *out [[buffer(1)]],
                    uint i [[thread_position_in_grid]]) {
    int x = in[i];
    int y = int(i % 13) + 1;
    uint ux = uint(x);
    out[i * 4 + 0] = x / y;
    out[i * 4 + 1] = x % y;
    out[i * 4 + 2] = int(ux / 7u) + int(ux % 16u);
    out[i * 4 + 3] = x / 5 + x % 3;
}

kernel void vec_ops(device const float4 *in [[buffer(0)]], device float4 *out [[buffer(1)]],
                    device uint2 *iout [[buffer(2)]], uint i [[thread_position_in_grid]]) {
    float4 v = in[i];
    float4 w = v.wzyx * float4(1.0f, 2.0f, 3.0f, 4.0f) + v;
    float d = dot(v, w);
    out[i] = float4(w.xy, d, length(v.xyz));
    iout[i] = uint2(uint(v.x), uint(w.w)) + uint2(i, 1u);
}

kernel void small_types(device const uchar *b [[buffer(0)]], device const ushort *s [[buffer(1)]],
                        device uchar *bo [[buffer(2)]], device ushort *so [[buffer(3)]],
                        uint i [[thread_position_in_grid]]) {
    uchar x = b[i];
    char sx = char(x);
    bo[i] = sx < 0 ? uchar(x * 3u + 1u) : uchar(x + 77u);
    ushort y = s[i];
    so[i] = ushort(y * 5u) ^ ushort(x);
}

kernel void half_math(device const half *in [[buffer(0)]], device half *out [[buffer(1)]],
                      uint i [[thread_position_in_grid]]) {
    half x = in[i];
    half y = x * half(0.75h) + half(1.0h);
    out[i] = y * y - half(0.5h) * x;
}

kernel void bit_ops(device const uint *in [[buffer(0)]], device uint *out [[buffer(1)]],
                    uint i [[thread_position_in_grid]]) {
    uint x = in[i];
    out[i * 4 + 0] = popcount(x);
    out[i * 4 + 1] = clz(x);
    out[i * 4 + 2] = ctz(x);
    out[i * 4 + 3] = reverse_bits(x);
}

kernel void pos_2d(device uint *out [[buffer(0)]], uint2 p [[thread_position_in_grid]]) {
    out[p.y * 64 + p.x] = p.x * 1000 + p.y;
}

// ---- milestone 3f: threadgroup memory, barriers, atomics ---------------------
// reverse each group of 64 through threadgroup memory (needs the barrier:
// the value read was written by another hardware thread)
kernel void tg_reverse(device const float *in [[buffer(0)]], device float *out [[buffer(1)]],
                       uint gid [[thread_position_in_grid]], uint lid [[thread_position_in_threadgroup]]) {
    threadgroup float tmp[64];
    tmp[lid] = in[gid];
    threadgroup_barrier(mem_flags::mem_threadgroup);
    out[gid] = tmp[63 - lid];
}

// tree sum of each group of 256: a barrier inside a loop
kernel void tg_sum(device const uint *in [[buffer(0)]], device uint *out [[buffer(1)]],
                   uint gid [[thread_position_in_grid]], uint lid [[thread_position_in_threadgroup]],
                   uint tg [[threadgroup_position_in_grid]], uint n [[threads_per_threadgroup]]) {
    threadgroup uint s[256];
    s[lid] = in[gid];
    threadgroup_barrier(mem_flags::mem_threadgroup);
    for (uint k = n / 2; k > 0; k >>= 1) {
        if (lid < k) s[lid] += s[lid + k];
        threadgroup_barrier(mem_flags::mem_threadgroup);
    }
    if (lid == 0) out[tg] = s[0];
}

// device-memory atomics from every thread
kernel void atomic_hist(device const uint *in [[buffer(0)]], device atomic_uint *bins [[buffer(1)]],
                        device atomic_uint *total [[buffer(2)]], uint gid [[thread_position_in_grid]]) {
    uint v = in[gid];
    atomic_fetch_add_explicit(&bins[v & 15], 1u, memory_order_relaxed);
    atomic_fetch_add_explicit(&total[0], v & 255u, memory_order_relaxed);
    atomic_fetch_max_explicit(&total[1], v, memory_order_relaxed);
    atomic_fetch_min_explicit(&total[2], v, memory_order_relaxed);
}

// threadgroup atomics + barriers, run with a partly filled last group
kernel void tg_count(device const uint *in [[buffer(0)]], device uint *out [[buffer(1)]],
                     device atomic_uint *grand [[buffer(2)]], uint gid [[thread_position_in_grid]],
                     uint lid [[thread_position_in_threadgroup]], uint tg [[threadgroup_position_in_grid]]) {
    threadgroup atomic_uint cnt;
    if (lid == 0) atomic_store_explicit(&cnt, 0u, memory_order_relaxed);
    threadgroup_barrier(mem_flags::mem_threadgroup);
    if (in[gid] & 1) atomic_fetch_add_explicit(&cnt, 1u, memory_order_relaxed);
    threadgroup_barrier(mem_flags::mem_threadgroup);
    if (lid == 0) {
        uint c = atomic_load_explicit(&cnt, memory_order_relaxed);
        out[tg] = c;
        atomic_fetch_add_explicit(grand, c, memory_order_relaxed);
    }
}

// ---- v0.29.0: register reuse and big kernels (> 248 instructions) -----------
// 40 values live through a long unrolled chain and a loop; the code is far
// bigger than 3968 bytes, so it runs from its own GPU buffer
kernel void big_mix(device const uint *in [[buffer(0)]], device uint *out [[buffer(1)]],
                    constant uint &n [[buffer(2)]], uint gid [[thread_position_in_grid]]) {
    uint x = in[gid];
    uint a[40];
    #pragma unroll
    for (int i = 0; i < 40; i++) { x = x * 1664525u + 1013904223u + uint(i); a[i] = x ^ (x >> 13); }
    uint t = a[0];
    #pragma unroll
    for (int i = 0; i < 300; i++) {
        uint b = a[(i * 7) % 40];
        if (i % 3 == 0) t = t * 3u + b;
        else if (i % 3 == 1) t = (t ^ b) + (t >> 5);
        else t = (t - b) ^ (t << 3);
    }
    uint p = t, q = 5u, w = 0u;
    for (uint j = 0; j < n; j++) {
        uint pa = p + a[3] * j;
        uint qx = q ^ (a[17] >> (j & 7u));
        if (pa & 1u) { qx += a[29]; w++; }
        q = qx; p = pa ^ q;
    }
    uint r = (p + q) ^ w;
    #pragma unroll
    for (int i = 0; i < 40; i++) r = (r ^ a[i]) + (r >> 7);
    out[gid] = r;
}

// 64 values all live across a loop
kernel void wide_live(device const uint *in [[buffer(0)]], device uint *out [[buffer(1)]],
                      constant uint &n [[buffer(2)]], uint gid [[thread_position_in_grid]]) {
    uint x = in[gid];
    uint a[64];
    #pragma unroll
    for (int i = 0; i < 64; i++) { x = (x ^ (x << 5)) + 0x9e3779b9u * uint(i + 1); a[i] = x; }
    uint s = 0u;
    for (uint j = 0; j < n; j++) {
        #pragma unroll
        for (int i = 0; i < 64; i += 8) s = (s + a[i] * (j + 1u)) ^ (a[i + 3] >> (j & 15u));
    }
    #pragma unroll
    for (int i = 0; i < 64; i++) s ^= a[i] * uint(i + 1);
    out[gid] = s;
}

// v0.29.3: 480 distinct statements (no loop to fold them back into): the
// compiled kernel is far bigger than 3968 bytes, so it runs from its own GPU buffer
kernel void big_straight(device const uint *in [[buffer(0)]], device uint *out [[buffer(1)]],
                         uint gid [[thread_position_in_grid]]) {
    uint x = in[gid], y = x ^ 0x9e3779b9u;
#include "big_straight.inc"
    out[gid] = x ^ y;
}

// ---- milestone 4 (textures): compiled now so the build's irisxe-kernels.ll
// shows Apple's texture AIR; the test tool doesn't run these yet ---------------
kernel void tex_copy(texture2d<float, access::read> src [[texture(0)]],
                     texture2d<float, access::write> dst [[texture(1)]],
                     uint2 gid [[thread_position_in_grid]]) {
    dst.write(src.read(gid) * 0.5f, gid);
}
kernel void tex_uint_rw(texture2d<uint, access::read_write> t [[texture(0)]],
                        uint2 gid [[thread_position_in_grid]]) {
    t.write(t.read(gid) + uint4(1, 2, 3, 4), gid);
}
kernel void tex_size(texture2d<float, access::read> t [[texture(0)]], device uint *out [[buffer(0)]],
                     uint gid [[thread_position_in_grid]]) {
    out[gid] = t.get_width() * 65536u + t.get_height();
}
kernel void tex_sample(texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]],
                       device float4 *out [[buffer(0)]], uint2 gid [[thread_position_in_grid]]) {
    out[gid.y * 64 + gid.x] = t.sample(s, (float2(gid) + 0.5f) / 64.0f);
}
kernel void tex_sample_const(texture2d<float> t [[texture(0)]], device float4 *out [[buffer(0)]],
                             uint2 gid [[thread_position_in_grid]]) {
    constexpr sampler s(coord::normalized, address::clamp_to_edge, filter::linear);
    out[gid.y * 64 + gid.x] = t.sample(s, (float2(gid) + 0.25f) / 64.0f);
}
kernel void tex_half_write(texture2d<half, access::write> t [[texture(0)]],
                           uint2 gid [[thread_position_in_grid]]) {
    t.write(half4(half(gid.x) / 64.0h, half(gid.y) / 64.0h, 0.25h, 1.0h), gid);
}
kernel void tex_short_coord(texture2d<float, access::read> src [[texture(0)]],
                            device float4 *out [[buffer(0)]], ushort2 gid [[thread_position_in_grid]]) {
    out[gid.y * 64 + gid.x] = src.read(gid);
}
kernel void tex_array_write(texture2d_array<float, access::write> t [[texture(0)]],
                            uint3 gid [[thread_position_in_grid]]) {
    t.write(float4(gid.x, gid.y, gid.z, 1.0f), gid.xy, gid.z);
}

// constexpr samplers with different settings: not run, their AIR (in
// irisxe-kernels.ll) shows how Metal encodes them (to support them later)
#define CS_KERNEL(name, ...) \
kernel void name(texture2d<float> t [[texture(0)]], device float4 *out [[buffer(0)]], uint gid [[thread_position_in_grid]]) { \
    constexpr sampler s(__VA_ARGS__); out[gid] = t.sample(s, float2(gid) / 64.0f); }
CS_KERNEL(cs_nearest_edge, coord::normalized, address::clamp_to_edge, filter::nearest)
CS_KERNEL(cs_linear_repeat, coord::normalized, address::repeat, filter::linear)
CS_KERNEL(cs_nearest_mirror, coord::normalized, address::mirrored_repeat, filter::nearest)
CS_KERNEL(cs_pixel_coords, coord::pixel, address::clamp_to_edge, filter::nearest)
CS_KERNEL(cs_clamp_zero, coord::normalized, address::clamp_to_zero, filter::linear)
CS_KERNEL(cs_mixed, s_address::repeat, t_address::clamp_to_edge, mag_filter::linear, min_filter::nearest, mip_filter::linear)
CS_KERNEL(cs_border_white, address::clamp_to_border, border_color::opaque_white, filter::linear)

// ---- milestone 5 (render pipeline): compiled now so the build's
// irisxe-kernels.ll shows Apple's AIR for vertex and fragment shaders; the
// test tool doesn't run these yet ----------------------------------------------
struct RVOut { float4 pos [[position]]; float4 color; float2 uv; };
vertex RVOut rv_basic(uint vid [[vertex_id]], const device float4 *pos [[buffer(0)]],
                      const device float4 *col [[buffer(1)]]) {
    RVOut o; o.pos = pos[vid]; o.color = col[vid]; o.uv = pos[vid].xy * 0.5f + 0.5f; return o;
}
fragment float4 rf_color(RVOut in [[stage_in]]) { return in.color; }

struct RVIn { float3 position [[attribute(0)]]; float2 uv [[attribute(1)]]; uchar4 color [[attribute(2)]]; };
struct RUniforms { float4x4 mvp; float4 tint; };
vertex RVOut rv_stagein(RVIn in [[stage_in]], constant RUniforms &u [[buffer(1)]], uint iid [[instance_id]]) {
    RVOut o; o.pos = u.mvp * float4(in.position + float3(iid, 0, 0), 1.0f);
    o.color = float4(in.color) / 255.0f * u.tint; o.uv = in.uv; return o;
}
fragment float4 rf_texture(RVOut in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {
    return t.sample(s, in.uv) * in.color;
}
// v0.57.1: --fillrate: a 9-tap vertical blur, like WindowServer's shadow / background blurs
fragment float4 rf_blur9(RVOut in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {
    float4 c = 0.0f;
    for (int k = -4; k <= 4; k++) c += t.sample(s, in.uv + float2(0.0f, k * (1.0f / 1600.0f))) * (1.0f / 9.0f);
    return c;
}
// v0.37.0: depth textures in fragment functions
fragment float4 rf_depth_tex(RVOut in [[stage_in]], depth2d<float> d [[texture(0)]], sampler s [[sampler(0)]]) {
    return float4(d.sample(s, in.uv), 0.0f, 0.0f, 1.0f);
}
fragment float4 rf_shadow(RVOut in [[stage_in]], depth2d<float> d [[texture(0)]], sampler s [[sampler(0)]]) {
    return float4(d.sample_compare(s, in.uv, 0.5f), 0.0f, 0.0f, 1.0f);
}
// v0.41.0: texturecube: column strips of the 64x64 target look along +X, -X, +Y, -Y, +Z, -Z
fragment float4 rf_cube(RVOut in [[stage_in]], texturecube<float> t [[texture(0)]], sampler s [[sampler(0)]]) {
    int f = min(int(in.uv.x * 6.0f), 5);
    float a = in.uv.y * 1.2f - 0.6f;
    float3 d = f == 0 ? float3(1, a, 0.3f) : f == 1 ? float3(-1, a, 0.3f) : f == 2 ? float3(0.3f, 1, a) :
               f == 3 ? float3(0.3f, -1, a) : f == 4 ? float3(a, 0.3f, 1) : float3(a, 0.3f, -1);
    return t.sample(s, d);
}
// v0.42.0: derivatives, mix, smoothstep: expect (1, -1, 1, ~1) everywhere on a 64x64 target
fragment float4 rf_deriv(RVOut in [[stage_in]]) {
    float s = smoothstep(0.0f, 1.0f, 0.5f + in.uv.y * 0.0001f);
    return float4(dfdx(in.uv.x) * 64.0f, dfdy(in.uv.y) * 64.0f, fwidth(in.uv.x) * 64.0f, mix(0.0f, 2.0f, s));
}
// v0.42.1: explicit level: every pixel from level 2
fragment float4 rf_level(RVOut in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {
    return t.sample(s, in.uv, level(2.0f));
}
struct RFOut { float4 c0 [[color(0)]]; float4 c1 [[color(1)]]; float depth [[depth(any)]]; };
fragment RFOut rf_mrt(RVOut in [[stage_in]], bool front [[front_facing]]) {
    RFOut o; o.c0 = in.color; o.c1 = float4(in.pos.xy / 256.0f, front ? 1.0f : 0.0f, 1.0f);
    o.depth = in.pos.z * 0.5f; return o;
}
fragment float4 rf_discard(RVOut in [[stage_in]], constant float &cut [[buffer(0)]]) {
    if (in.uv.x < cut) discard_fragment();
    return float4(in.uv, 0.0f, 1.0f);
}
struct RFlat { float4 pos [[position]]; uint id [[flat]]; float k [[center_no_perspective]]; float ps [[point_size]]; };
vertex RFlat rv_flat(uint vid [[vertex_id]], const device float2 *p [[buffer(0)]]) {
    RFlat o; o.pos = float4(p[vid], 0.0f, 1.0f); o.id = vid; o.k = float(vid); o.ps = 4.0f; return o;
}
fragment uint4 rf_flat(RFlat in [[stage_in]]) { return uint4(in.id, uint(in.k * 16.0f), 0, 1); }
fragment half4 rf_half(RVOut in [[stage_in]]) { return half4(in.color * 0.5f); }

// ---- milestone 5b: our compiler's vertex / fragment code, drawn by IrisXeCore
// (draw C: the triangle of draw B, vertices = float4 position + float4 colour) --
struct R5In { float4 pos [[attribute(0)]]; float4 color [[attribute(1)]]; };
struct R5Out { float4 pos [[position]]; float4 color; };
vertex R5Out r5_vs(R5In in [[stage_in]]) { R5Out o; o.pos = in.pos; o.color = in.color; return o; }
fragment float4 r5_fs(R5Out in [[stage_in]]) { return in.color; }
// v0.34.0: writes its own depth (= the red channel)
struct R5DOut { float4 c [[color(0)]]; float d [[depth(any)]]; };
fragment R5DOut r5_fs_depth(R5Out in [[stage_in]]) { R5DOut o; o.c = in.color; o.d = in.color.r; return o; }
// [[position]], discard_fragment and arithmetic: left half discarded, colours inverted
fragment float4 r5_fs_cut(R5Out in [[stage_in]]) {
    if (in.pos.x < 32.0f) discard_fragment();
    return float4(1.0f - in.color.rgb, 1.0f);
}
// flat (first vertex) and screen-linear varyings, a vertex shader that computes
struct R5Out2 { float4 pos [[position]]; float4 flatc [[flat]]; float4 lin [[center_no_perspective]]; float k; };
vertex R5Out2 r5_vs2(R5In in [[stage_in]]) {
    R5Out2 o; o.pos = in.pos; o.flatc = in.color; o.lin = in.color; o.k = in.color.g * 0.5f + 0.25f; return o;
}
fragment float4 r5_fs2(R5Out2 in [[stage_in]]) { return float4(in.lin.r, in.k, in.flatc.r, 1.0f); }
// ---- milestone 5c: fragment functions for the Metal draw test (with r5_vs) ----
fragment float4 r5_fs_tex(R5Out in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {
    return t.sample(s, in.color.rg);
}
fragment float4 r5_fs_k(R5Out in [[stage_in]], constant float4 &k [[buffer(0)]]) { return in.color * k; }

