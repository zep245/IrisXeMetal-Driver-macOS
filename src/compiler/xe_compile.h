// xe_compile.h - IrisXe shader compiler (milestone 3c): Metal AIR (LLVM
// bitcode) compute kernel -> Iris Xe (Gen12) SIMD8 machine code for our
// dispatch command (see src/IrisXeSpaceABI.h for the register layout).
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// returns 0 on success: *outDW dwords of code in out (max maxDW), and in
// (v0.29.1: bits 29:24 = private bytes per thread / 16, for the scratch buffer in slot 5)
// *outInfo the threadgroup memory size in bytes (bits 23:0) | 0x80000000 if
// the kernel uses barriers (v0.28.0, milestone 3f).
// On failure: < 0 and a message in log.
// (the rest of the compiler and LLVM are hidden: built with -fvisibility=hidden)
__attribute__((visibility("default")))
int irisxe_compile_air(const void *bitcode, size_t len, const char *function,
                       uint32_t *out, uint32_t maxDW, uint32_t *outDW, uint32_t *outInfo,
                       uint64_t *outSamplers, char *log, size_t logLen);
// v0.30.1: constexpr samplers (written in the shader) get sampler slots 7, 6, 5, 4:
// their count is in *outInfo bits 19:17, their Metal encoding (AIR __air_sampler_state,
// first word) in outSamplers[0..3] (outSamplers[k] is slot 7 - k).

// ---- milestone 5b: vertex and fragment functions ------------------------------
// One SIMD8 program per stage. Register layout (what the pipeline must program):
//  vertex:   g0 header, g1 URB handles, then from grfStart (= 2): [push constants
//            = buffer 0..7 GPU addresses, pushRegs registers], then vertex input
//            slot s at grfStart + pushRegs + 4 s (x, y, z, w registers):
//            3DSTATE_VS Dispatch GRF Start = grfStart (the hardware puts the
//            constants there and the vertex data after them), URB Entry Read
//            Length = urbReadLength, offset 0.
//            Output VUE: slot 0 header (dw3 = point size), slot 1 position,
//            slot 2 + k = varying k. vueSlots slots in all.
//  fragment: g0, g1, perspective barycentrics (2 registers, if flags & PERSP),
//            linear barycentrics (2, if flags & LINEAR), then [push constants],
//            then attribute setup data (2 registers per varying): 3DSTATE_PS
//            Dispatch GRF Start 0 = grfStart. Render target k = binding table k,
//            texture k = binding table 8 + k (v0.32.0).
#define XE_SHADER_INFO_VERSION 1
#define XE_MAX_VS_INPUTS 16
#define XE_MAX_VARYINGS  16
enum { XE_STAGE_VERTEX = 1, XE_STAGE_FRAGMENT = 2 };
enum { XE_INTERP_PERSPECTIVE = 0, XE_INTERP_LINEAR = 1, XE_INTERP_FLAT = 2 };
enum {
    XE_VS_POINT_SIZE  = 1u << 0,     // vertex: writes the point size (VUE header dw3)
    XE_FS_DISCARD     = 1u << 0,     // fragment: discard_fragment (PS kills pixels)
    XE_FS_PERSP       = 1u << 1,     // perspective pixel barycentrics in the payload
    XE_FS_LINEAR      = 1u << 2,     // linear (no perspective) barycentrics
    XE_FS_POSITION    = 1u << 3,     // reads [[position]].xy
    XE_FS_FRONTFACING = 1u << 4,
    XE_FS_SRCDEPTH    = 1u << 6,     // reads [[position]].z (v0.35.0): 3DSTATE_PS_EXTRA Pixel Shader Uses Source Depth
    XE_FS_DEPTH       = 1u << 5,     // writes [[depth(any)]] (v0.34.0): every render target write carries it
    XE_FS_SRCW        = 1u << 7,     // reads [[position]].w (v0.51.23): 3DSTATE_PS_EXTRA Pixel Shader Uses Source W
    XE_FS_SIDEFX      = 1u << 8,     // v0.62.1: writes memory, textures or does atomics (no helper pixels then)
    XE_FS_NOCOLOR     = 1u << 9,     // v0.68.30: writes no color (depth / stencil passes): a zero write to RT0, masked off
};
enum { XE_SYS_VERTEX_ID = 1u << 0, XE_SYS_INSTANCE_ID = 1u << 1 };
enum { XE_RT_FLOAT = 0, XE_RT_SINT = 1, XE_RT_UINT = 2, XE_RT_HALF = 3 };
struct XeShaderInfo {
    uint32_t version;                      // XE_SHADER_INFO_VERSION
    uint32_t stage;                        // XE_STAGE_*
    uint32_t grfStart;                     // see above
    uint32_t pushRegs;                     // 0, 1 or 2 (v0.51.8: buffers 8..15): buffer addresses pushed (3DSTATE_CONSTANT_*)
    uint32_t flags;                        // XE_VS_* / XE_FS_*
    uint32_t usesTextures;                 // textures / samplers are bound (binding table, sampler states)
    // vertex
    uint32_t numInputs;                    // vertex input slots (vertex elements), system slot included
    uint32_t urbReadLength;                // 256-bit units
    int32_t  inputLocation[XE_MAX_VS_INPUTS];   // [[attribute(n)]] of slot s; -1 = system values slot
    uint32_t inputComps[XE_MAX_VS_INPUTS];
    uint32_t sysValues;                    // XE_SYS_*: VertexID in sysSlot.x, InstanceID in sysSlot.y (3DSTATE_VF_SGVS)
    uint32_t sysSlot;
    uint32_t vueSlots;
    // varyings: vertex outputs (in VUE order) / fragment inputs (in attribute order)
    uint32_t numVaryings;
    uint32_t varyingInterp[XE_MAX_VARYINGS];    // fragment: XE_INTERP_*
    uint32_t varyingComps[XE_MAX_VARYINGS];
    char     varyingName[XE_MAX_VARYINGS][64];  // AIR name, e.g. "generated(5colorDv4_f)"
    // fragment outputs
    uint32_t rtMask;                       // render targets written
    uint32_t rtType[8];                    // XE_RT_*
    // constexpr samplers: slots 7, 6, .. (Metal encoding, like irisxe_compile_air outSamplers)
    uint32_t numConstSamplers;
    uint64_t constSampler[4];
    // v0.48.0: private arrays (render stages): privBytes per lane in a scratch buffer of
    // XE_SCRATCH_IDS * 8 * privBytes bytes; its GPU address replaces the dword XE_SCRATCH_MAGIC in the code
    uint32_t privBytes;
    // v0.68.26: buffer slots the GPU sees are 0..15; a used [[buffer(16..30)]] (Dawn / Chromium put their uniforms at
    // buffer 30) goes into a free slot: bufSrc[slot] = Metal buffer index + 1 (0 = the slot's own index)
    uint8_t  bufSrc[16];
};
#define XE_SCRATCH_MAGIC 0x5CA7C4D1u
#define XE_SCRATCH_IDS   4096
__attribute__((visibility("default")))
int irisxe_compile_air_render(const void *bitcode, size_t len, const char *function,
                              uint32_t *out, uint32_t maxDW, uint32_t *outDW,
                              struct XeShaderInfo *info, char *log, size_t logLen);

#ifdef __cplusplus
}
#endif
