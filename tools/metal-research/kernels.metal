// Compute kernels for our first compiler (milestone 3c): simplest first.
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

kernel void loop_sum(device const uint *in [[buffer(0)]], device uint *out [[buffer(1)]],
                     constant uint &n [[buffer(2)]], uint i [[thread_position_in_grid]]) {
    uint s = 0;
    for (uint k = 0; k < n; k++) s += in[i * n + k];
    out[i] = s;
}
