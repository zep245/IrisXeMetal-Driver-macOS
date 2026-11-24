// IrisXeDrawTest.h - milestone 5b: shaders from our compiler drawn by IrisXeCore
// (draw C, IrisXeRender.inc kRender3Cmds). irisxe-metaltest compiles a vertex
// and a fragment function, fills this struct and sets it as the OSData property
// IrisXe-DrawShaders on IrisXeCore's GPU core; the core draws the triangle of
// draw B with them and returns the 64x64 RGBA8 image as IrisXe-DrawPixels.
#pragma once
#define XE_DRAWTEST_VERSION 1
struct XeDrawTestCfg {
    uint32_t version;                // XE_DRAWTEST_VERSION
    uint32_t vsDW, psDW;             // code sizes (dwords): vs <= 256, ps <= 512
    uint32_t vsGrf, vsReadLen;       // 3DSTATE_VS: Dispatch GRF Start, URB Entry Read Length
    uint32_t psGrf;                  // 3DSTATE_PS: Dispatch GRF Start 0
    uint32_t numAttr, sbeReadLen;    // 3DSTATE_SBE: attributes, VUE read length (offset 1)
    uint32_t constInterp;            // 3DSTATE_SBE: flat attributes (bit mask)
    uint32_t baryMode;               // 3DSTATE_WM: Barycentric Interpolation Mode (bit 0 perspective, bit 3 linear)
    uint32_t nonPersp;               // 3DSTATE_CLIP: non-perspective barycentrics
    uint32_t kill;                   // the PS discards pixels
    uint32_t swiz[16];               // 3DSTATE_SBE_SWIZ: source attribute of attribute k
    uint32_t vs[256];
    uint32_t ps[512];
};
