// IrisXeDrawABI.h - milestone 5c: one Metal draw, from IrisXeMTLDriver.bundle
// (render encoder) through IrisXeAccel (kernel command XE_KCMD_DRAW) to
// IrisXeCore (kXeSpaceDraw, IrisXeGPU.cpp mtlRunDraw3D). Plain C, shared by the
// bundle and both kexts. The core checks everything and builds the GPU
// commands itself from the template in IrisXeRender.inc.
#pragma once
#include <stdint.h>

#define XE_DRAW_VERSION   7       // 7 (v0.51.8): 16 shader buffers (was 8), push constants of 1 or 2 registers
#define XE_DRAW_VERSION_6 6       // 6 (v0.48.0): private array scratch buffers
#define XE_DRAW_VERSION_5 5       // 5 (v0.47.0): vertex textures / samplers
#define XE_DRAW_VERSION_4 4       // 2 (v0.32.5): vsLen/psLen; 3 (v0.33.0): blending, scissor, index buffer; 4 (v0.34.0): depth / stencil
#define XE_DRAW_MAX_RT    8
#define XE_DRAW_MAX_VB    8
#define XE_DRAW_MAX_VE    16
#define XE_DRAW_MAX_TEX   8
#define XE_DRAW_MAX_FTEX  16      // v0.51.8: fragment textures (was 8)
#define XE_DRAW_MAX_SAMP  8

struct XeDrawTex {          // same layout as XeTexDesc (IrisXeSpaceABI.h)
    uint64_t va;
    uint32_t width, height, layers, pitch, qpitch, format, array, pad;
};
struct XeDrawSamp {         // same layout as XeSampDesc
    uint32_t used;
    uint8_t  minFilter, magFilter, mipFilter, addrU, addrV, addrW, normalized, pad[5];
};
struct XeDrawVB { uint64_t va; uint32_t size, pitch; };
struct XeDrawVE {
    uint32_t vb;            // vertex buffer index (0 .. nvb-1)
    uint32_t format;        // Gen surface format (ISL_FORMAT_*), e.g. 0 = R32G32B32A32_FLOAT
    uint32_t offset;        // bytes
    uint32_t ctrl;          // 4 x 4 bits: component k control (VFCOMP_*) in bits 4k+3:4k
    uint32_t instanced, stepRate;
};
struct XeDrawBlend {        // one color attachment, Metal's own enums (the core maps them)
    uint32_t enable;        // blendingEnabled
    uint32_t src, dst, op;  // MTLBlendFactor 0..14 (no dual source), MTLBlendOperation 0..4: RGB
    uint32_t srcA, dstA, opA;   // the same for alpha
    uint32_t writeMask;     // MTLColorWriteMask: red 8, green 4, blue 2, alpha 1
};
struct XeDrawStencil {      // one face, Metal's enums (MTLCompareFunction, MTLStencilOperation)
    uint32_t func, fail, depthFail, pass, readMask, writeMask;
};
struct XeDrawDS {           // v0.34.0: depth / stencil attachment and state
    uint64_t depthVA;       // 0 = no depth buffer; Y-tiled, 4 KB aligned
    uint32_t depthPitch;    // bytes, multiple of 128
    uint32_t depthFormat;   // MTLPixelFormat: 250 Depth16Unorm, 252 Depth32Float, 260 Depth32Float_Stencil8
    uint32_t depthRows;     // rows allocated (multiple of 32)
    uint32_t depthSize;     // bytes the GPU may touch
    uint64_t stencilVA;     // 0 = no stencil buffer; Y-tiled (Gen12), 4 KB aligned
    uint32_t stencilPitch;  // bytes, multiple of 128
    uint32_t stencilRows;   // rows allocated (multiple of 32)
    uint32_t stencilSize;
    uint32_t width, height; // of the depth / stencil texture
    uint32_t clearDepth, clearStencil;            // load action clear
    uint32_t depthClearValue;                     // float bits
    uint32_t stencilClearValue;
    uint32_t depthWrite, depthFunc;               // MTLDepthStencilState
    struct XeDrawStencil front, back;
    uint32_t frontRef, backRef;
    uint32_t biasConstant, biasSlope, biasClamp;  // float bits (setDepthBias:slopeScale:clamp:)
    uint32_t depthClamp;                          // MTLDepthClipModeClamp
    uint32_t psDepth;                             // the fragment function writes [[depth]]
    uint32_t psSrcDepth;                          // v0.35.0: bit 0 = it reads [[position]].z (was pad, 0); v0.51.23: bit 1 = .w
};
struct XeDrawDesc {
    uint32_t version;       // XE_DRAW_VERSION
    uint32_t size;          // sizeof(struct XeDrawDesc)
    // shaders (our compiler; code in the process's GPU memory, 64-byte aligned, 256 zero bytes after it)
    uint64_t vsVA, psVA;
    uint32_t vsDW, psDW;
    uint32_t vsGrf, vsReadLen, vsPush;
    uint32_t psGrf, psPush;
    uint32_t numAttr, sbeReadLen, constInterp, baryMode, nonPersp, kill;
    uint32_t swiz[16];
    // vertex input: buffers, elements (element k = VS input slot k); VertexID / InstanceID
    uint32_t nvb, nve;
    struct XeDrawVB vb[XE_DRAW_MAX_VB];
    struct XeDrawVE ve[XE_DRAW_MAX_VE];
    uint32_t sysValues, sysSlot;                  // XE_SYS_* (xe_compile.h): VertexID in .x, InstanceID in .y
    uint32_t vsBuf[16], psBuf[16];                // push constants: GPU addresses of buffers 0..15 (v0.51.8: 0..7 before)
    uint32_t vsLen[16], psLen[16];                  // their lengths in bytes: IrisXeAccel maps them for the GPU
    // render targets: rt[k] (va 0 = none); clear before drawing: bit k of clearMask, value clear[k] (raw bits)
    uint32_t nrt, clearMask;
    struct XeDrawTex rt[XE_DRAW_MAX_RT];
    uint32_t clear[XE_DRAW_MAX_RT][4];
    uint32_t width, height;                       // render area (pixels)
    // fragment textures / samplers (binding table 8 + k / sampler k)
    uint32_t ntex, nsamp;
    struct XeDrawTex tex[XE_DRAW_MAX_FTEX];
    struct XeDrawSamp samp[XE_DRAW_MAX_SAMP];
    // viewport (float bits): m00 m11 m22 m30 m31 m32, guardband xmin xmax ymin ymax, viewport xmin xmax ymin ymax
    uint32_t vp[14];
    uint32_t cull;                                // 3DSTATE_RASTER CullMode: 1 none, 2 front, 3 back
    uint32_t frontCCW;                            // 3DSTATE_RASTER FrontWinding (window coordinates)
    uint32_t fillLines;                           // MTLTriangleFillModeLines
    uint32_t topology;                            // _3DPRIM_*: 1 points, 2 lines, 3 line strip, 4 triangles, 5 strip
    uint32_t vertexStart, vertexCount, instanceCount, baseInstance;
    // v0.33.0
    struct XeDrawBlend blend[XE_DRAW_MAX_RT];
    uint32_t blendColor[4];                       // float bits (setBlendColorRed:green:blue:alpha:)
    uint32_t scissor[4];                          // x0, y0, x1, y1 (pixels, x1/y1 exclusive; empty if x1 <= x0 or y1 <= y0)
    uint64_t ibVA;                                // indexed draw: index buffer (+ offset); vertexStart = 0, vertexCount = index count
    uint32_t ibSize, ibType;                      // bytes; 0 = not indexed, 1 = uint16, 2 = uint32 (restart index 0xFFFF / 0xFFFFFFFF)
    int32_t  baseVertex;
    uint32_t pad;
    struct XeDrawDS ds;                           // v0.34.0
    // v0.47.0: vertex textures / samplers (VS binding table entry k / VS sampler k)
    uint32_t nvtex, nvsamp;
    struct XeDrawTex vtex[XE_DRAW_MAX_TEX];
    struct XeDrawSamp vsamp[XE_DRAW_MAX_SAMP];
    // v0.48.0: private arrays: the shaders' scratch buffers (IrisXeAccel maps them; the address is in the code)
    uint64_t vsScratchVA, psScratchVA;
    uint32_t vsScratchLen, psScratchLen;
};
