// Milestone 5a/5b: packs the Gen12 (TGL) 3D commands for IrisXeCore's draw
// tests with Mesa's genxml packer (gen120_pack.h). The state sequence follows
// Mesa blorp (blorp_genX_exec_brw.h) and iris (iris_state.c); TGL GT2 values
// come from Mesa devinfo for 8086:9a49 (tools/shadergen/README.md).
//
// Draw A (5a, kRenderCmds): the vertex shader is off - the vertex fetcher
//   writes the VUE (header + screen-space position) straight into the URB,
//   like blorp. Clipping and the viewport transform are off. A rectangle
//   (RECTLIST) and a triangle in one constant colour (render_ps.asm).
// Draw B (5b, kRender2Cmds): the real pipeline. A vertex shader
//   (render_vs.asm) passes clip-space position and a colour per vertex; the
//   clipper, perspective divide and viewport transform are on (Metal
//   conventions: depth 0..1, y up, framebuffer origin top left); the pixel
//   shader (render_ps2.asm) interpolates the colour (perspective-correct).
//
// GPU memory (all in the render block, mapped by gpuBuildTables):
//   0x20003000  shaders: PS A +0, VS B +0x400, PS B +0x800   (render page 17)
//   0x20004000  dynamic state: blend, CC, CC viewport, SF_CLIP viewport (page 18)
//   0x20005000  surface state base: binding table, RT surface state, vertices (page 19)
//   0x20006000  render target 64x64 RGBA8, pitch 256  (pages 28..31)
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <math.h>
#define __gen_address_type uint64_t
#define __gen_user_data void
static inline uint64_t __gen_combine_address(void *d, void *loc, uint64_t a, uint32_t delta) { return a + delta; }
#define __gen_unpack_address(...) 0
#include "gen120_pack.h"
#define GFX_VER 12
#include "common/intel_guardband.h"

#define SH_VA    0x20003000u
#define PSA_OFF  0x000
#define VSB_OFF  0x400
#define PSB_OFF  0x800
#define DYN_VA   0x20004000u
#define SURF_VA  0x20005000u
#define RT_VA    0x20006000u
#define DSB      0x20000000u          // dynamic state base (same as Metal compute jobs)
#define RT_W     64
#define RT_H     64
#define RT_PITCH (RT_W * 4)
#define BLEND_OFF 0x000               // inside the dynamic page
#define CC_OFF    0x080
#define CCVP_OFF  0x0C0
#define SFVP_OFF  0x100               // SF_CLIP_VIEWPORT (64-byte aligned)
#define BT_OFF    0x000               // inside the surface page
#define SS_OFF    0x040
#define VB0_OFF   0x400               // draw A: 6 vertices x 12 bytes
#define VB1_OFF   0x480               // draw A: 16 zero bytes (VUE header, stride 0)
#define VBB_OFF   0x500               // draw B: 3 vertices x (float4 position, float4 colour)

static uint32_t out[1024]; static int n = 0;
#define EMIT(cmd, name) for (struct cmd name = { cmd##_header }, *_p = (void*)1; _p; \
    ({ cmd##_pack(NULL, &out[n], &name); n += cmd##_length; _p = NULL; }))

static uint32_t dyn[1024], surf[1024], sh[1024];
static const uint32_t mocs = 2 << 1;

// draw B vertices: clip space (x, y, z, w) + colour; the top vertex has w = 2
// (same screen point) to test perspective-correct interpolation
const float kVertsB[3][8] = {
    { -0.75f, -0.75f, 0.5f, 1.0f,   1, 0, 0, 1 },
    {  0.75f, -0.75f, 0.5f, 1.0f,   0, 1, 0, 1 },
    {  0.0f,   1.5f,  1.0f, 2.0f,   0, 0, 1, 1 },
};

static void lri(uint32_t reg, uint32_t val) {
    EMIT(GFX12_MI_LOAD_REGISTER_IMM, l) { l.DWordLength = 1; l.RegisterOffset = reg; l.DataDWord = val; }
}

static void prefix(void) {
    // switch to the 3D pipeline (iris emit_pipeline_select, anv MaskBits for gfx12)
    EMIT(GFX12_PIPE_CONTROL, p) { p.CommandStreamerStallEnable = true; p.HDCPipelineFlushEnable = true;
                                  p.DCFlushEnable = true; p.VFCacheInvalidationEnable = true; }
    EMIT(GFX12_PIPELINE_SELECT, ps) { ps.PipelineSelection = _3D; ps.MaskBits = 0x13; ps.MediaSamplerDOPClockGateEnable = true; }

    // L3: Mesa's default TGL config (all 88 ways, URB 32)
    EMIT(GFX12_PIPE_CONTROL, p) { p.CommandStreamerStallEnable = true; p.DCFlushEnable = true; }
    {
        struct GFX12_L3ALLOC r = { .URBAllocation = 32, .AllAllocation = 88 };
        uint32_t v; GFX12_L3ALLOC_pack(NULL, &v, &r);
        lri(GFX12_L3ALLOC_num, v);
    }

    EMIT(GFX12_PIPE_CONTROL, p) { p.CommandStreamerStallEnable = true; p.RenderTargetCacheFlushEnable = true;
                                  p.DepthCacheFlushEnable = true; p.HDCPipelineFlushEnable = true; }
    EMIT(GFX12_STATE_BASE_ADDRESS, s) {
        uint32_t size = (1 << 20) - 1;
        s.GeneralStateBaseAddress = 0; s.GeneralStateBaseAddressModifyEnable = true;
        s.GeneralStateBufferSize = size; s.GeneralStateBufferSizeModifyEnable = true; s.GeneralStateMOCS = mocs;
        s.SurfaceStateBaseAddress = SURF_VA; s.SurfaceStateBaseAddressModifyEnable = true; s.SurfaceStateMOCS = mocs;
        s.DynamicStateBaseAddress = DSB; s.DynamicStateBaseAddressModifyEnable = true;
        s.DynamicStateBufferSize = size; s.DynamicStateBufferSizeModifyEnable = true; s.DynamicStateMOCS = mocs;
        s.InstructionBaseAddress = 0; s.InstructionBaseAddressModifyEnable = true;
        s.InstructionBufferSize = size; s.InstructionBuffersizeModifyEnable = true; s.InstructionMOCS = mocs;
        s.IndirectObjectBaseAddress = 0; s.IndirectObjectBaseAddressModifyEnable = true;
        s.IndirectObjectBufferSize = size; s.IndirectObjectBufferSizeModifyEnable = true; s.IndirectObjectMOCS = mocs;
        s.StatelessDataPortAccessMOCS = mocs; s.BindlessSamplerStateMOCS = mocs; s.BindlessSurfaceStateMOCS = mocs;
    }
    EMIT(GFX12_PIPE_CONTROL, p) { p.CommandStreamerStallEnable = true; p.StateCacheInvalidationEnable = true;
                                  p.TextureCacheInvalidationEnable = true; p.ConstantCacheInvalidationEnable = true;
                                  p.InstructionCacheInvalidateEnable = true; }

    // context-level state (iris_init_render_context)
    EMIT(GFX12_3DSTATE_DRAWING_RECTANGLE, r) { r.ClippedDrawingRectangleXMax = RT_W - 1; r.ClippedDrawingRectangleYMax = RT_H - 1; }
    EMIT(GFX12_3DSTATE_AA_LINE_PARAMETERS, a) { }
    EMIT(GFX12_3DSTATE_WM_CHROMAKEY, c) { }
    EMIT(GFX12_3DSTATE_WM_HZ_OP, h) { }
    EMIT(GFX12_3DSTATE_POLY_STIPPLE_OFFSET, o) { }
    for (int i = 0; i < 5; i++)                     // 32 KB push constant space: 6 KB per stage, PS gets 8
        EMIT(GFX12_3DSTATE_PUSH_CONSTANT_ALLOC_VS, a) { a._3DCommandSubOpcode = 18 + i;
            a.ConstantBufferOffset = 6 * i; a.ConstantBufferSize = i == 4 ? 8 : 6; }
}

static void vertexBuffers(int nvb, const struct GFX12_VERTEX_BUFFER_STATE *vb) {
    struct GFX12_3DSTATE_VERTEX_BUFFERS h = { GFX12_3DSTATE_VERTEX_BUFFERS_header };
    h.DWordLength = 1 + nvb * GFX12_VERTEX_BUFFER_STATE_length - 2;
    GFX12_3DSTATE_VERTEX_BUFFERS_pack(NULL, &out[n], &h); n += 1;
    for (int i = 0; i < nvb; i++) { GFX12_VERTEX_BUFFER_STATE_pack(NULL, &out[n], &vb[i]); n += GFX12_VERTEX_BUFFER_STATE_length; }
}
static void vertexElements(int ne, const struct GFX12_VERTEX_ELEMENT_STATE *ve) {
    struct GFX12_3DSTATE_VERTEX_ELEMENTS h = { GFX12_3DSTATE_VERTEX_ELEMENTS_header };
    h.DWordLength = 1 + ne * GFX12_VERTEX_ELEMENT_STATE_length - 2;
    GFX12_3DSTATE_VERTEX_ELEMENTS_pack(NULL, &out[n], &h); n += 1;
    for (int i = 0; i < ne; i++) { GFX12_VERTEX_ELEMENT_STATE_pack(NULL, &out[n], &ve[i]); n += GFX12_VERTEX_ELEMENT_STATE_length; }
    EMIT(GFX12_3DSTATE_VF_STATISTICS, v) { v.StatisticsEnable = false; }
}

// blorp_emit_pipeline parts that both draws share
static void commonPipeline(void) {
    for (int i = 0; i < 4; i++)
        EMIT(GFX12_3DSTATE_URB_ALLOC_VS, u) { u._3DCommandSubOpcode += i;
            u.VSURBEntryAllocationSize = i == 0 ? 1 : 0;       // VS: 2 x 64 bytes (header, position, varyings)
            u.VSURBStartingAddressSlice0 = 4; u.VSURBStartingAddressSliceN = 4;
            u.VSNumberofURBEntriesSlice0 = i == 0 ? 3576 : 0; u.VSNumberofURBEntriesSliceN = i == 0 ? 3576 : 0; }
    EMIT(GFX12_3DSTATE_BLEND_STATE_POINTERS, b) { b.BlendStatePointer = DYN_VA - DSB + BLEND_OFF; b.BlendStatePointerValid = true; }
    EMIT(GFX12_3DSTATE_PS_BLEND, b) { b.HasWriteableRT = true; }
    EMIT(GFX12_3DSTATE_CC_STATE_POINTERS, c) { c.ColorCalcStatePointer = DYN_VA - DSB + CC_OFF; c.ColorCalcStatePointerValid = true; }
    EMIT(GFX12_3DSTATE_WM_DEPTH_STENCIL, d) { }
    EMIT(GFX12_3DSTATE_DEPTH_BOUNDS, d) { d.DepthBoundsTestEnable = false; d.DepthBoundsTestMinValue = 0.0f; d.DepthBoundsTestMaxValue = 1.0f; }
    EMIT(GFX12_3DSTATE_CONSTANT_ALL, c) { c.ShaderUpdateEnable = 0x1f; c.MOCS = mocs; }
    EMIT(GFX12_3DSTATE_MULTISAMPLE, m) { m.NumberofMultisamples = 0; m.PixelLocation = CENTER; }
    EMIT(GFX12_3DSTATE_SAMPLE_MASK, m) { m.SampleMask = 1; }
}

static void tail(void) {
    EMIT(GFX12_3DSTATE_VIEWPORT_STATE_POINTERS_CC, v) { v.CCViewportPointer = DYN_VA - DSB + CCVP_OFF; }
    EMIT(GFX12_3DSTATE_PRIMITIVE_REPLICATION, p) { }
    // binding tables: only the PS has one (blorp_emit_btp)
    EMIT(GFX12_3DSTATE_BINDING_TABLE_POINTERS_VS, b) { }
    EMIT(GFX12_3DSTATE_BINDING_TABLE_POINTERS_HS, b) { }
    EMIT(GFX12_3DSTATE_BINDING_TABLE_POINTERS_DS, b) { }
    EMIT(GFX12_3DSTATE_BINDING_TABLE_POINTERS_GS, b) { }
    EMIT(GFX12_3DSTATE_BINDING_TABLE_POINTERS_PS, b) { b.PointertoPSBindingTable = BT_OFF; }
    // no depth / stencil (isl_emit_depth_stencil_hiz_s, null case)
    EMIT(GFX12_PIPE_CONTROL, p) { p.DepthStallEnable = true; p.DepthCacheFlushEnable = true; p.CommandStreamerStallEnable = true; }
    EMIT(GFX12_3DSTATE_DEPTH_BUFFER, d) { d.SurfaceType = SURFTYPE_NULL; d.SurfaceFormat = D32_FLOAT; d.MOCS = mocs; }
    EMIT(GFX12_3DSTATE_STENCIL_BUFFER, s) { s.SurfaceType = SURFTYPE_NULL; s.MOCS = mocs; }
    EMIT(GFX12_3DSTATE_HIER_DEPTH_BUFFER, h) { h.MOCS = mocs; }
    EMIT(GFX12_3DSTATE_CLEAR_PARAMS, c) { c.DepthClearValueValid = true; }
}

static void finish(void) {
    EMIT(GFX12_PIPE_CONTROL, p) { p.RenderTargetCacheFlushEnable = true; p.DepthCacheFlushEnable = true;
                                  p.TileCacheFlushEnable = true; p.HDCPipelineFlushEnable = true;
                                  p.CommandStreamerStallEnable = true; }
}

static void drawA(void) {
    prefix();
    struct GFX12_VERTEX_BUFFER_STATE vb[2] = {
        { .VertexBufferIndex = 0, .AddressModifyEnable = true, .MOCS = mocs, .L3BypassDisable = true,
          .BufferStartingAddress = SURF_VA + VB0_OFF, .BufferSize = 6 * 12, .BufferPitch = 12 },
        { .VertexBufferIndex = 1, .AddressModifyEnable = true, .MOCS = mocs, .L3BypassDisable = true,
          .BufferStartingAddress = SURF_VA + VB1_OFF, .BufferSize = 16, .BufferPitch = 0 } };
    vertexBuffers(2, vb);
    struct GFX12_VERTEX_ELEMENT_STATE ve[2] = {
        { .VertexBufferIndex = 1, .Valid = true, .SourceElementFormat = 0x000 /* R32G32B32A32_FLOAT */,
          .Component0Control = VFCOMP_STORE_SRC, .Component1Control = VFCOMP_STORE_0,
          .Component2Control = VFCOMP_STORE_0, .Component3Control = VFCOMP_STORE_0 },
        { .VertexBufferIndex = 0, .Valid = true, .SourceElementFormat = 0x040 /* R32G32B32_FLOAT */,
          .Component0Control = VFCOMP_STORE_SRC, .Component1Control = VFCOMP_STORE_SRC,
          .Component2Control = VFCOMP_STORE_SRC, .Component3Control = VFCOMP_STORE_1_FP } };
    vertexElements(2, ve);
    EMIT(GFX12_3DSTATE_VF_SGVS, s) { s.InstanceIDEnable = true; s.InstanceIDComponentNumber = COMP_1; s.InstanceIDElementOffset = 0; }
    EMIT(GFX12_3DSTATE_VF_SGVS_2, s) { }
    for (int i = 0; i < 2; i++) EMIT(GFX12_3DSTATE_VF_INSTANCING, v) { v.VertexElementIndex = i; v.InstancingEnable = false; }
    EMIT(GFX12_3DSTATE_VF, v) { }

    commonPipeline();
    EMIT(GFX12_3DSTATE_VS, v) { }                         // disabled: the VF output is the VUE
    EMIT(GFX12_3DSTATE_HS, h) { }
    EMIT(GFX12_3DSTATE_TE, t) { }
    EMIT(GFX12_3DSTATE_DS, d) { }
    EMIT(GFX12_3DSTATE_STREAMOUT, s) { }
    EMIT(GFX12_3DSTATE_GS, g) { }
    EMIT(GFX12_3DSTATE_CLIP, c) { c.PerspectiveDivideDisable = true; }
    EMIT(GFX12_3DSTATE_SF, s) { s.DerefBlockSize = BlockDerefSize32; }
    EMIT(GFX12_3DSTATE_RASTER, r) { r.CullMode = CULLMODE_NONE; }
    EMIT(GFX12_3DSTATE_SBE, s) {
        s.VertexURBEntryReadOffset = 1; s.VertexURBEntryReadLength = 1;
        s.ForceVertexURBEntryReadLength = true; s.ForceVertexURBEntryReadOffset = true;
        s.NumberofSFOutputAttributes = 0;
        for (int i = 0; i < 32; i++) s.AttributeActiveComponentFormat[i] = ACF_XYZW;
    }
    EMIT(GFX12_3DSTATE_SBE_SWIZ, s) { }
    EMIT(GFX12_3DSTATE_WM, w) { }
    EMIT(GFX12_3DSTATE_PS, p) {
        p.KernelStartPointer0 = SH_VA + PSA_OFF; p._8PixelDispatchEnable = true;
        p.BindingTableEntryCount = 1; p.MaximumNumberofThreadsPerPSD = 63;
        p.DispatchGRFStartRegisterForConstantSetupData0 = 2;
    }
    EMIT(GFX12_3DSTATE_PS_EXTRA, x) { x.PixelShaderValid = true; }
    tail();

    // rectangle (blorp's RECTLIST), then one triangle
    EMIT(GFX12_3DSTATE_VF_TOPOLOGY, t) { t.PrimitiveTopologyType = _3DPRIM_RECTLIST; }
    EMIT(GFX12_3DPRIMITIVE, p) { p.VertexAccessType = SEQUENTIAL; p.PrimitiveTopologyType = _3DPRIM_RECTLIST;
                                 p.VertexCountPerInstance = 3; p.StartVertexLocation = 0; p.InstanceCount = 1; }
    EMIT(GFX12_3DSTATE_VF_TOPOLOGY, t) { t.PrimitiveTopologyType = _3DPRIM_TRILIST; }
    EMIT(GFX12_3DPRIMITIVE, p) { p.VertexAccessType = SEQUENTIAL; p.PrimitiveTopologyType = _3DPRIM_TRILIST;
                                 p.VertexCountPerInstance = 3; p.StartVertexLocation = 3; p.InstanceCount = 1; }
    finish();
}

// Draw B: VS on (SIMD8: g1 URB handles, inputs from g2: position g2-g5, colour g6-g9),
// VUE out: slot 0 header, slot 1 position, slot 2 colour. PS: perspective pixel
// barycentrics in g2-g3, colour plane parameters from g4 (16 bytes per component).
static int atVS, atCLIP, atSBE, atSWIZ, atWM, atPS, atPSX;
static void drawB(int tmpl) {
    prefix();
    struct GFX12_VERTEX_BUFFER_STATE vb[1] = {
        { .VertexBufferIndex = 0, .AddressModifyEnable = true, .MOCS = mocs, .L3BypassDisable = true,
          .BufferStartingAddress = SURF_VA + VBB_OFF, .BufferSize = sizeof(kVertsB), .BufferPitch = 32 } };
    vertexBuffers(1, vb);
    struct GFX12_VERTEX_ELEMENT_STATE ve[2] = {
        { .VertexBufferIndex = 0, .Valid = true, .SourceElementFormat = 0x000 /* R32G32B32A32_FLOAT */, .SourceElementOffset = 0,
          .Component0Control = VFCOMP_STORE_SRC, .Component1Control = VFCOMP_STORE_SRC,
          .Component2Control = VFCOMP_STORE_SRC, .Component3Control = VFCOMP_STORE_SRC },
        { .VertexBufferIndex = 0, .Valid = true, .SourceElementFormat = 0x000, .SourceElementOffset = 16,
          .Component0Control = VFCOMP_STORE_SRC, .Component1Control = VFCOMP_STORE_SRC,
          .Component2Control = VFCOMP_STORE_SRC, .Component3Control = VFCOMP_STORE_SRC } };
    vertexElements(2, ve);
    EMIT(GFX12_3DSTATE_VF_SGVS, s) { }
    EMIT(GFX12_3DSTATE_VF_SGVS_2, s) { }
    for (int i = 0; i < 2; i++) EMIT(GFX12_3DSTATE_VF_INSTANCING, v) { v.VertexElementIndex = i; v.InstancingEnable = false; }
    EMIT(GFX12_3DSTATE_VF, v) { }

    commonPipeline();
    EMIT(GFX12_PIPE_CONTROL, p) { p.CommandStreamerStallEnable = true; p.DepthStallEnable = true; p.PostSyncOperation = NoWrite; }
    atVS = n;
    EMIT(GFX12_3DSTATE_VS, v) {                           // iris INIT_THREAD_DISPATCH_FIELDS
        v.Enable = true; v.SIMD8DispatchEnable = true;
        v.MaximumNumberofThreads = 546 - 1;               // devinfo max_vs_threads
        v.VertexURBEntryReadOffset = 0;
        if (!tmpl) {
            v.KernelStartPointer = SH_VA + VSB_OFF;
            v.DispatchGRFStartRegisterForURBData = 2;     // g0 header, g1 URB handles
            v.VertexURBEntryReadLength = 1;               // 2 input slots (256 bits)
        }
    }
    EMIT(GFX12_3DSTATE_HS, h) { }
    EMIT(GFX12_3DSTATE_TE, t) { }
    EMIT(GFX12_3DSTATE_DS, d) { }
    EMIT(GFX12_3DSTATE_STREAMOUT, s) { }
    EMIT(GFX12_3DSTATE_GS, g) { }
    atCLIP = n;
    EMIT(GFX12_3DSTATE_CLIP, c) {                         // iris: normal clipping, D3D depth (0..1) like Metal
        c.EarlyCullEnable = true; c.APIMode = APIMODE_D3D; c.GuardbandClipTestEnable = true;
        c.ClipEnable = true; c.ClipMode = CLIPMODE_NORMAL; c.ViewportXYClipTestEnable = true;
        c.MinimumPointWidth = 0.125f; c.MaximumPointWidth = 255.875f;
        c.TriangleStripListProvokingVertexSelect = 0; c.ForceZeroRTAIndexEnable = true; c.MaximumVPIndex = 0;
    }
    EMIT(GFX12_3DSTATE_SF, s) { s.DerefBlockSize = BlockDerefSize32; s.ViewportTransformEnable = true;
                                s.PointWidthSource = State; s.PointWidth = 1.0f; s.AALineDistanceMode = AALINEDISTANCE_TRUE; }
    EMIT(GFX12_3DSTATE_RASTER, r) { r.CullMode = CULLMODE_NONE; r.FrontWinding = CounterClockwise;
                                    r.ViewportZNearClipTestEnable = true; r.ViewportZFarClipTestEnable = true; }
    atSBE = n;
    EMIT(GFX12_3DSTATE_SBE, s) {
        s.AttributeSwizzleEnable = true;
        if (!tmpl) s.NumberofSFOutputAttributes = 1;      // the colour
        s.VertexURBEntryReadOffset = 1;                   // skip header + position (256 bits)
        if (!tmpl) s.VertexURBEntryReadLength = 1;
        s.ForceVertexURBEntryReadLength = true; s.ForceVertexURBEntryReadOffset = true;
        for (int i = 0; i < 32; i++) s.AttributeActiveComponentFormat[i] = ACF_XYZW;
    }
    atSWIZ = n;
    EMIT(GFX12_3DSTATE_SBE_SWIZ, s) { }
    atWM = n;
    EMIT(GFX12_3DSTATE_WM, w) { if (!tmpl) w.BarycentricInterpolationMode = BIM_PERSPECTIVE_PIXEL;
                                w.LineAntialiasingRegionWidth = _10pixels; w.LineEndCapAntialiasingRegionWidth = _05pixels;
                                w.PointRasterizationRule = RASTRULE_UPPER_RIGHT; }
    atPS = n;
    EMIT(GFX12_3DSTATE_PS, p) {
        p._8PixelDispatchEnable = true;
        p.BindingTableEntryCount = 1; p.MaximumNumberofThreadsPerPSD = 63;
        if (!tmpl) {
            p.KernelStartPointer0 = SH_VA + PSB_OFF;
            p.DispatchGRFStartRegisterForConstantSetupData0 = 4;   // g0, g1, barycentrics g2-g3
        }
    }
    atPSX = n;
    EMIT(GFX12_3DSTATE_PS_EXTRA, x) { x.PixelShaderValid = true; if (!tmpl) x.AttributeEnable = true; }
    EMIT(GFX12_3DSTATE_VIEWPORT_STATE_POINTERS_SF_CLIP, v) { v.SFClipViewportPointer = DYN_VA - DSB + SFVP_OFF; }
    tail();

    EMIT(GFX12_3DSTATE_VF_TOPOLOGY, t) { t.PrimitiveTopologyType = _3DPRIM_TRILIST; }
    EMIT(GFX12_3DPRIMITIVE, p) { p.VertexAccessType = SEQUENTIAL; p.PrimitiveTopologyType = _3DPRIM_TRILIST;
                                 p.VertexCountPerInstance = 3; p.StartVertexLocation = 0; p.InstanceCount = 1; }
    finish();
    // leave the VS off again for later jobs that don't set it
    EMIT(GFX12_PIPE_CONTROL, p) { p.CommandStreamerStallEnable = true; p.DepthStallEnable = true; }
    EMIT(GFX12_3DSTATE_VS, v) { }
}

// where a field sits: pack the command with only that field = 1 (other fields
// of the template are 0, so the kext patches by or'ing value << shift in)
#define FIELDPOSB(PFX, cmd, at, NAME, BASE, SET) do { \
    struct cmd a_ = { cmd##_header }, b_ = { cmd##_header }; (void)a_; \
    { struct cmd *f_ = &a_; BASE; } { struct cmd *f_ = &b_; BASE; SET; } \
    uint32_t pa_[64] = { 0 }, pb_[64] = { 0 }; \
    cmd##_pack(NULL, pa_, &a_); cmd##_pack(NULL, pb_, &b_); \
    int i_; for (i_ = 0; i_ < cmd##_length && pa_[i_] == pb_[i_]; i_++) ; \
    assert(i_ < cmd##_length); \
    printf("#define %s_%-18s %4d, %2d\n", PFX, NAME, (at) + i_, __builtin_ctz(pa_[i_] ^ pb_[i_])); } while (0)
#define FIELDPOSP(PFX, cmd, at, NAME, SET) FIELDPOSB(PFX, cmd, at, NAME, (void)f_, SET)
#define FIELDPOS(cmd, at, NAME, SET) FIELDPOSP("R3", cmd, at, NAME, SET)
#define FIELDPOSE(PFX, st, NAME, SET) do { \
    struct st a_ = { 0 }, b_ = { 0 }; \
    { struct st *f_ = &b_; SET; } \
    uint32_t pa_[64] = { 0 }, pb_[64] = { 0 }; \
    st##_pack(NULL, pa_, &a_); st##_pack(NULL, pb_, &b_); \
    int i_; for (i_ = 0; i_ < st##_length && pa_[i_] == pb_[i_]; i_++) ; \
    printf("#define %s_%-18s %4d, %2d\n", PFX, NAME, i_, __builtin_ctz(pa_[i_] ^ pb_[i_])); } while (0)
// the same for a structure without a command header
#define FIELDPOSS(PFX, st, NAME, SET) do { \
    struct st a_ = { 0 }, b_ = { 0 }; a_.MOCS = b_.MOCS = mocs; \
    { struct st *f_ = &b_; SET; } \
    uint32_t pa_[64] = { 0 }, pb_[64] = { 0 }; \
    st##_pack(NULL, pa_, &a_); st##_pack(NULL, pb_, &b_); \
    int i_; for (i_ = 0; i_ < st##_length && pa_[i_] == pb_[i_]; i_++) ; \
    assert(i_ < st##_length); \
    printf("#define %s_%-18s %4d, %2d\n", PFX, NAME, i_, __builtin_ctz(pa_[i_] ^ pb_[i_])); } while (0)



// ---- milestone 5c: Metal draws (IrisXeCore builds them from this template) ----
// draw state in IrisXeCore's draw pages (GPU VA 0x20010000): see IrisXeRender.inc
#define DRW_VA      0x20010000u
#define DRW_CODE    (DRW_VA + 0x0000)       // clear pixel shader
#define DRW_CLRVTX  (DRW_VA + 0x0400)       // clear rectangle: 3 x uint3 (USCALED), then 16 zero bytes
#define DRW_DYN     (DRW_VA + 0x1000)
#define DRW_SURF    (DRW_VA + 0x2000)
#define DYN_BLEND   0x000                   // BLEND_STATE + 8 entries
#define DYN_CC      0x080
#define DYN_CCVP    0x0C0
#define DYN_SFVP    0x100
#define DYN_SAMP    0x200                   // 8 x SAMPLER_STATE
#define DYN_BORDER  0x300                   // one transparent black border colour
#define DYN_PUSHVS  0x400                   // 32 bytes: buffer addresses 0..7
#define DYN_PUSHPS  0x420
#define DYN_CLEAR   0x440                   // 8 x 32 bytes: clear colours
#define DYN_SCISSOR 0x180                   // SCISSOR_RECT (v0.33.0)
#define DYN_BLENDCLR 0x600                  // BLEND_STATE for the clears: no blending, all channels
#define SRF_BTPS    0x000                   // PS binding table: RT 0..7, textures 8..15
#define SRF_BTVS    0x040
#define SRF_BTCLR   0x080                   // 8 x 32 bytes: one entry (RT k) per clear
#define SRF_SS      0x200                   // 16 x RENDER_SURFACE_STATE: RT 0..7, textures 8..15
#define DRW_URB_SIZE    5                   // VS URB entry: 5 x 64 bytes (header, position, 16 varyings)
#define DRW_URB_ENTRIES 1432                // (480 KB URB - 32 KB push constants = 448 KB) / 320 bytes, multiple of 8
                                            // (v0.32.3: 3168 overflowed the URB: draws drew nothing or hung)

static int atPSB, atWMDS;                             // 3DSTATE_PS_BLEND, 3DSTATE_WM_DEPTH_STENCIL of the last commonPipeline2
static void commonPipeline2(uint32_t blendOff) {
    for (int i = 0; i < 4; i++)
        EMIT(GFX12_3DSTATE_URB_ALLOC_VS, u) { u._3DCommandSubOpcode += i;
            u.VSURBEntryAllocationSize = i == 0 ? DRW_URB_SIZE - 1 : 0;
            u.VSURBStartingAddressSlice0 = 4; u.VSURBStartingAddressSliceN = 4;
            u.VSNumberofURBEntriesSlice0 = i == 0 ? DRW_URB_ENTRIES : 0; u.VSNumberofURBEntriesSliceN = i == 0 ? DRW_URB_ENTRIES : 0; }
    EMIT(GFX12_3DSTATE_BLEND_STATE_POINTERS, b) { b.BlendStatePointer = DRW_DYN - DSB + blendOff; b.BlendStatePointerValid = true; }
    atPSB = n;
    EMIT(GFX12_3DSTATE_PS_BLEND, b) { b.HasWriteableRT = true; }
    EMIT(GFX12_3DSTATE_CC_STATE_POINTERS, c) { c.ColorCalcStatePointer = DRW_DYN - DSB + DYN_CC; c.ColorCalcStatePointerValid = true; }
    atWMDS = n;                                       // depth / stencil test (v0.34.0: patched)
    EMIT(GFX12_3DSTATE_WM_DEPTH_STENCIL, d) { }
    EMIT(GFX12_3DSTATE_DEPTH_BOUNDS, d) { d.DepthBoundsTestEnable = false; d.DepthBoundsTestMinValue = 0.0f; d.DepthBoundsTestMaxValue = 1.0f; }
    EMIT(GFX12_3DSTATE_CONSTANT_ALL, c) { c.ShaderUpdateEnable = 0x1f; c.MOCS = mocs; }
    EMIT(GFX12_3DSTATE_MULTISAMPLE, m) { m.NumberofMultisamples = 0; m.PixelLocation = CENTER; }
    EMIT(GFX12_3DSTATE_SAMPLE_MASK, m) { m.SampleMask = 1; }
}

static int atD[32];
enum { D_RECT, D_VB, D_VE, D_SGVS, D_INST, D_VS, D_CLIP, D_SF, D_RASTER, D_SBE, D_SWIZ, D_WM, D_PS, D_PSX,
       D_CVS, D_CPS, D_TOPO, D_PRIM, D_SAMPP, D_END, D_PSB, D_VF, D_IB, D_WMDS, D_DB, D_SB };

static void prefixD(void) {
    prefix();
    {   // push constant buffer addresses are GPU addresses, not dynamic-state offsets (iris init)
        struct GFX12_CS_DEBUG_MODE2 r = { .CONSTANT_BUFFERAddressOffsetDisable = true, .CONSTANT_BUFFERAddressOffsetDisableMask = true };
        uint32_t v; GFX12_CS_DEBUG_MODE2_pack(NULL, &v, &r);
        lri(GFX12_CS_DEBUG_MODE2_num, v);
    }
}
// the Metal draw: VB/VE/instancing packets are built by the kext between the parts
static void drawD(void) {
    prefixD();
    // the SBA from prefix() points at draw B's surface page: point it at ours
    // (patched below by the caller searching for it would be fragile: re-emit)
    EMIT(GFX12_PIPE_CONTROL, p) { p.CommandStreamerStallEnable = true; p.RenderTargetCacheFlushEnable = true; }
    EMIT(GFX12_STATE_BASE_ADDRESS, s) {
        uint32_t size = (1 << 20) - 1;
        s.GeneralStateBaseAddress = 0; s.GeneralStateBaseAddressModifyEnable = true;
        s.GeneralStateBufferSize = size; s.GeneralStateBufferSizeModifyEnable = true; s.GeneralStateMOCS = mocs;
        s.SurfaceStateBaseAddress = DRW_SURF; s.SurfaceStateBaseAddressModifyEnable = true; s.SurfaceStateMOCS = mocs;
        s.DynamicStateBaseAddress = DSB; s.DynamicStateBaseAddressModifyEnable = true;
        s.DynamicStateBufferSize = size; s.DynamicStateBufferSizeModifyEnable = true; s.DynamicStateMOCS = mocs;
        s.InstructionBaseAddress = 0; s.InstructionBaseAddressModifyEnable = true;
        s.InstructionBufferSize = size; s.InstructionBuffersizeModifyEnable = true; s.InstructionMOCS = mocs;
        s.IndirectObjectBaseAddress = 0; s.IndirectObjectBaseAddressModifyEnable = true;
        s.IndirectObjectBufferSize = size; s.IndirectObjectBufferSizeModifyEnable = true; s.IndirectObjectMOCS = mocs;
        s.StatelessDataPortAccessMOCS = mocs; s.BindlessSamplerStateMOCS = mocs; s.BindlessSurfaceStateMOCS = mocs;
    }
    EMIT(GFX12_PIPE_CONTROL, p) { p.CommandStreamerStallEnable = true; p.StateCacheInvalidationEnable = true;
                                  p.TextureCacheInvalidationEnable = true; p.ConstantCacheInvalidationEnable = true;
                                  p.InstructionCacheInvalidateEnable = true; }
    atD[D_RECT] = n;
    EMIT(GFX12_3DSTATE_DRAWING_RECTANGLE, r) { }
    atD[D_VB] = n;                                        // <- 3DSTATE_VERTEX_BUFFERS, 3DSTATE_VERTEX_ELEMENTS
    EMIT(GFX12_3DSTATE_VF_STATISTICS, v) { v.StatisticsEnable = false; }
    atD[D_SGVS] = n;
    EMIT(GFX12_3DSTATE_VF_SGVS, s) { }
    EMIT(GFX12_3DSTATE_VF_SGVS_2, s) { }
    atD[D_INST] = n;                                      // <- one 3DSTATE_VF_INSTANCING per element
    atD[D_VF] = n;
    EMIT(GFX12_3DSTATE_VF, v) { }
    commonPipeline2(DYN_BLEND);
    atD[D_PSB] = atPSB; atD[D_WMDS] = atWMDS;
    EMIT(GFX12_PIPE_CONTROL, p) { p.CommandStreamerStallEnable = true; p.DepthStallEnable = true; }
    atD[D_VS] = n;
    EMIT(GFX12_3DSTATE_VS, v) { v.Enable = true; v.SIMD8DispatchEnable = true; v.MaximumNumberofThreads = 546 - 1; }
    EMIT(GFX12_3DSTATE_HS, h) { }
    EMIT(GFX12_3DSTATE_TE, t) { }
    EMIT(GFX12_3DSTATE_DS, d) { }
    EMIT(GFX12_3DSTATE_STREAMOUT, s) { }
    EMIT(GFX12_3DSTATE_GS, g) { }
    atD[D_CLIP] = n;
    EMIT(GFX12_3DSTATE_CLIP, c) {                         // provoking vertex: first (Metal)
        c.EarlyCullEnable = true; c.APIMode = APIMODE_D3D; c.GuardbandClipTestEnable = true;
        c.ClipEnable = true; c.ClipMode = CLIPMODE_NORMAL;
        c.MinimumPointWidth = 0.125f; c.MaximumPointWidth = 255.875f; c.ForceZeroRTAIndexEnable = true;
    }
    atD[D_SF] = n;
    EMIT(GFX12_3DSTATE_SF, s) { s.DerefBlockSize = BlockDerefSize32; s.ViewportTransformEnable = true;
                                s.AALineDistanceMode = AALINEDISTANCE_TRUE; s.LineWidth = 1.0f; }
    atD[D_RASTER] = n;
    EMIT(GFX12_3DSTATE_RASTER, r) { r.ScissorRectangleEnable = true; }   // Metal: always a scissor (default = render area);
                                                                          // z clip tests, depth bias: patched (v0.34.0)
    atD[D_SBE] = n;
    EMIT(GFX12_3DSTATE_SBE, s) {
        s.AttributeSwizzleEnable = true; s.VertexURBEntryReadOffset = 1;
        s.ForceVertexURBEntryReadLength = true; s.ForceVertexURBEntryReadOffset = true;
        for (int i = 0; i < 32; i++) s.AttributeActiveComponentFormat[i] = ACF_XYZW;
    }
    atD[D_SWIZ] = n;
    EMIT(GFX12_3DSTATE_SBE_SWIZ, s) { }
    atD[D_WM] = n;
    EMIT(GFX12_3DSTATE_WM, w) { w.LineAntialiasingRegionWidth = _10pixels; w.LineEndCapAntialiasingRegionWidth = _05pixels;
                                w.PointRasterizationRule = RASTRULE_UPPER_RIGHT; }
    atD[D_PS] = n;
    EMIT(GFX12_3DSTATE_PS, p) { p._8PixelDispatchEnable = true; p.BindingTableEntryCount = 16; p.MaximumNumberofThreadsPerPSD = 63; }
    atD[D_PSX] = n;
    EMIT(GFX12_3DSTATE_PS_EXTRA, x) { x.PixelShaderValid = true; }
    EMIT(GFX12_3DSTATE_VIEWPORT_STATE_POINTERS_SF_CLIP, v) { v.SFClipViewportPointer = DRW_DYN - DSB + DYN_SFVP; }
    EMIT(GFX12_3DSTATE_VIEWPORT_STATE_POINTERS_CC, v) { v.CCViewportPointer = DRW_DYN - DSB + DYN_CCVP; }
    EMIT(GFX12_3DSTATE_SCISSOR_STATE_POINTERS, v) { v.ScissorRectPointer = DRW_DYN - DSB + DYN_SCISSOR; }
    EMIT(GFX12_3DSTATE_PRIMITIVE_REPLICATION, p) { }
    atD[D_CVS] = n;
    EMIT(GFX12_3DSTATE_CONSTANT_VS, c) { c.MOCS = mocs; }
    atD[D_CPS] = n;
    EMIT(GFX12_3DSTATE_CONSTANT_PS, c) { c.MOCS = mocs; }
    EMIT(GFX12_3DSTATE_BINDING_TABLE_POINTERS_VS, b) { b.PointertoVSBindingTable = SRF_BTVS; }
    EMIT(GFX12_3DSTATE_BINDING_TABLE_POINTERS_HS, b) { }
    EMIT(GFX12_3DSTATE_BINDING_TABLE_POINTERS_DS, b) { }
    EMIT(GFX12_3DSTATE_BINDING_TABLE_POINTERS_GS, b) { }
    EMIT(GFX12_3DSTATE_BINDING_TABLE_POINTERS_PS, b) { b.PointertoPSBindingTable = SRF_BTPS; }
    atD[D_SAMPP] = n;
    EMIT(GFX12_3DSTATE_SAMPLER_STATE_POINTERS_PS, s) { s.PointertoPSSamplerState = DRW_DYN - DSB + DYN_SAMP; }
    EMIT(GFX12_PIPE_CONTROL, p) { p.DepthStallEnable = true; p.DepthCacheFlushEnable = true; p.CommandStreamerStallEnable = true; }
    atD[D_DB] = n;                                        // surface type, format, address, size: patched (v0.34.0)
    EMIT(GFX12_3DSTATE_DEPTH_BUFFER, d) { d.MOCS = mocs; }
    atD[D_SB] = n;
    EMIT(GFX12_3DSTATE_STENCIL_BUFFER, s) { s.MOCS = mocs; }
    EMIT(GFX12_3DSTATE_HIER_DEPTH_BUFFER, h) { h.MOCS = mocs; }
    EMIT(GFX12_3DSTATE_CLEAR_PARAMS, c) { c.DepthClearValueValid = true; }
    atD[D_TOPO] = n;
    EMIT(GFX12_3DSTATE_VF_TOPOLOGY, t) { }
    atD[D_IB] = n;                                        // indexed draws: format, address, size patched
    EMIT(GFX12_3DSTATE_INDEX_BUFFER, ib) { ib.MOCS = mocs; }
    atD[D_PRIM] = n;
    EMIT(GFX12_3DPRIMITIVE, p) { p.VertexAccessType = SEQUENTIAL; }
    finish();
    EMIT(GFX12_PIPE_CONTROL, p) { p.CommandStreamerStallEnable = true; p.DepthStallEnable = true; }
    EMIT(GFX12_3DSTATE_VS, v) { }
    atD[D_END] = n;
}
// a clear of one render target: blorp's RECTLIST, VS off, PS = render_clear.asm
// (colour from push constants), binding table entry 0 = that render target
static int atC[8];
enum { C_RECT, C_BT, C_CPS, C_END, C_WMDS, C_DB, C_SB };
static void drawClr(void) {
    prefixD();
    EMIT(GFX12_PIPE_CONTROL, p) { p.CommandStreamerStallEnable = true; p.RenderTargetCacheFlushEnable = true; }
    EMIT(GFX12_STATE_BASE_ADDRESS, s) {
        uint32_t size = (1 << 20) - 1;
        s.GeneralStateBaseAddress = 0; s.GeneralStateBaseAddressModifyEnable = true;
        s.GeneralStateBufferSize = size; s.GeneralStateBufferSizeModifyEnable = true; s.GeneralStateMOCS = mocs;
        s.SurfaceStateBaseAddress = DRW_SURF; s.SurfaceStateBaseAddressModifyEnable = true; s.SurfaceStateMOCS = mocs;
        s.DynamicStateBaseAddress = DSB; s.DynamicStateBaseAddressModifyEnable = true;
        s.DynamicStateBufferSize = size; s.DynamicStateBufferSizeModifyEnable = true; s.DynamicStateMOCS = mocs;
        s.InstructionBaseAddress = 0; s.InstructionBaseAddressModifyEnable = true;
        s.InstructionBufferSize = size; s.InstructionBuffersizeModifyEnable = true; s.InstructionMOCS = mocs;
        s.IndirectObjectBaseAddress = 0; s.IndirectObjectBaseAddressModifyEnable = true;
        s.IndirectObjectBufferSize = size; s.IndirectObjectBufferSizeModifyEnable = true; s.IndirectObjectMOCS = mocs;
        s.StatelessDataPortAccessMOCS = mocs; s.BindlessSamplerStateMOCS = mocs; s.BindlessSurfaceStateMOCS = mocs;
    }
    EMIT(GFX12_PIPE_CONTROL, p) { p.CommandStreamerStallEnable = true; p.StateCacheInvalidationEnable = true;
                                  p.TextureCacheInvalidationEnable = true; p.ConstantCacheInvalidationEnable = true;
                                  p.InstructionCacheInvalidateEnable = true; }
    atC[C_RECT] = n;
    EMIT(GFX12_3DSTATE_DRAWING_RECTANGLE, r) { }
    struct GFX12_VERTEX_BUFFER_STATE vb[2] = {
        { .VertexBufferIndex = 0, .AddressModifyEnable = true, .MOCS = mocs, .L3BypassDisable = true,
          .BufferStartingAddress = DRW_CLRVTX, .BufferSize = 3 * 12, .BufferPitch = 12 },
        { .VertexBufferIndex = 1, .AddressModifyEnable = true, .MOCS = mocs, .L3BypassDisable = true,
          .BufferStartingAddress = DRW_CLRVTX + 0x40, .BufferSize = 16, .BufferPitch = 0 } };
    vertexBuffers(2, vb);
    struct GFX12_VERTEX_ELEMENT_STATE ve[2] = {
        { .VertexBufferIndex = 1, .Valid = true, .SourceElementFormat = 0x000,
          .Component0Control = VFCOMP_STORE_SRC, .Component1Control = VFCOMP_STORE_0,
          .Component2Control = VFCOMP_STORE_0, .Component3Control = VFCOMP_STORE_0 },
        { .VertexBufferIndex = 0, .Valid = true, .SourceElementFormat = 64 /* R32G32B32_FLOAT (v0.34.0: z = depth clear) */,
          .Component0Control = VFCOMP_STORE_SRC, .Component1Control = VFCOMP_STORE_SRC,
          .Component2Control = VFCOMP_STORE_SRC, .Component3Control = VFCOMP_STORE_1_FP } };
    vertexElements(2, ve);
    EMIT(GFX12_3DSTATE_VF_SGVS, s) { }
    EMIT(GFX12_3DSTATE_VF_SGVS_2, s) { }
    for (int i = 0; i < 2; i++) EMIT(GFX12_3DSTATE_VF_INSTANCING, v) { v.VertexElementIndex = i; v.InstancingEnable = false; }
    EMIT(GFX12_3DSTATE_VF, v) { }
    commonPipeline2(DYN_BLENDCLR);
    atC[C_WMDS] = atWMDS;
    EMIT(GFX12_PIPE_CONTROL, p) { p.CommandStreamerStallEnable = true; p.DepthStallEnable = true; }
    EMIT(GFX12_3DSTATE_VS, v) { }
    EMIT(GFX12_3DSTATE_HS, h) { }
    EMIT(GFX12_3DSTATE_TE, t) { }
    EMIT(GFX12_3DSTATE_DS, d) { }
    EMIT(GFX12_3DSTATE_STREAMOUT, s) { }
    EMIT(GFX12_3DSTATE_GS, g) { }
    EMIT(GFX12_3DSTATE_CLIP, c) { c.PerspectiveDivideDisable = true; }
    EMIT(GFX12_3DSTATE_SF, s) { s.DerefBlockSize = BlockDerefSize32; }
    EMIT(GFX12_3DSTATE_RASTER, r) { r.CullMode = CULLMODE_NONE; }
    EMIT(GFX12_3DSTATE_SBE, s) {
        s.VertexURBEntryReadOffset = 1; s.VertexURBEntryReadLength = 1;
        s.ForceVertexURBEntryReadLength = true; s.ForceVertexURBEntryReadOffset = true;
        for (int i = 0; i < 32; i++) s.AttributeActiveComponentFormat[i] = ACF_XYZW;
    }
    EMIT(GFX12_3DSTATE_SBE_SWIZ, s) { }
    EMIT(GFX12_3DSTATE_WM, w) { }
    EMIT(GFX12_3DSTATE_PS, p) {
        p.KernelStartPointer0 = DRW_CODE; p._8PixelDispatchEnable = true; p.PushConstantEnable = true;
        p.BindingTableEntryCount = 1; p.MaximumNumberofThreadsPerPSD = 63;
        p.DispatchGRFStartRegisterForConstantSetupData0 = 2;
    }
    EMIT(GFX12_3DSTATE_PS_EXTRA, x) { x.PixelShaderValid = true; }
    EMIT(GFX12_3DSTATE_VIEWPORT_STATE_POINTERS_CC, v) { v.CCViewportPointer = DRW_DYN - DSB + DYN_CCVP; }
    EMIT(GFX12_3DSTATE_PRIMITIVE_REPLICATION, p) { }
    atC[C_CPS] = n;
    EMIT(GFX12_3DSTATE_CONSTANT_PS, c) { c.MOCS = mocs; c.ConstantBody.ReadLength[0] = 1; }
    EMIT(GFX12_3DSTATE_BINDING_TABLE_POINTERS_VS, b) { }
    EMIT(GFX12_3DSTATE_BINDING_TABLE_POINTERS_HS, b) { }
    EMIT(GFX12_3DSTATE_BINDING_TABLE_POINTERS_DS, b) { }
    EMIT(GFX12_3DSTATE_BINDING_TABLE_POINTERS_GS, b) { }
    atC[C_BT] = n;
    EMIT(GFX12_3DSTATE_BINDING_TABLE_POINTERS_PS, b) { }
    EMIT(GFX12_PIPE_CONTROL, p) { p.DepthStallEnable = true; p.DepthCacheFlushEnable = true; p.CommandStreamerStallEnable = true; }
    atC[C_DB] = n;                                        // a depth / stencil clear: patched (v0.34.0)
    EMIT(GFX12_3DSTATE_DEPTH_BUFFER, d) { d.MOCS = mocs; }
    atC[C_SB] = n;
    EMIT(GFX12_3DSTATE_STENCIL_BUFFER, s) { s.MOCS = mocs; }
    EMIT(GFX12_3DSTATE_HIER_DEPTH_BUFFER, h) { h.MOCS = mocs; }
    EMIT(GFX12_3DSTATE_CLEAR_PARAMS, c) { c.DepthClearValueValid = true; }
    EMIT(GFX12_3DSTATE_VF_TOPOLOGY, t) { t.PrimitiveTopologyType = _3DPRIM_RECTLIST; }
    EMIT(GFX12_3DPRIMITIVE, p) { p.VertexAccessType = SEQUENTIAL; p.PrimitiveTopologyType = _3DPRIM_RECTLIST;
                                 p.VertexCountPerInstance = 3; p.InstanceCount = 1; }
    finish();
    atC[C_END] = n;
}

static void dump(const char *name, const uint32_t *d, int cnt, int per) {
    printf("static const UInt32 %s[%d] = {\n", name, cnt);
    for (int i = 0; i < cnt; i++) printf("%s0x%08x,%s", i % per ? " " : "    ", d[i], (i % per == per - 1 || i == cnt - 1) ? "\n" : "");
    printf("};\n");
}

static int load(const char *path, uint32_t off) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return -1; }
    int k = (int)fread(&sh[off / 4], 4, (0x400 / 4), f); fclose(f);
    return k;
}

int main(int argc, char **argv) {
    if (argc < 5) { fprintf(stderr, "usage: render render_ps.bin render_vs.bin render_ps2.bin render_clear.bin\n"); return 1; }
    if (load(argv[1], PSA_OFF) <= 0 || load(argv[2], VSB_OFF) <= 0 || load(argv[3], PSB_OFF) <= 0) return 1;
    {
        FILE *f = fopen(argv[4], "rb"); if (!f) return 1;
        uint32_t c[64]; int cn = (int)fread(c, 4, 64, f); fclose(f);
        dump("kClearPS", c, cn, 4);
    }

    printf("// generated by tools/shadergen/render.c with Mesa genxml gen120_pack.h (milestones 5a, 5b)\n");
    printf("#define RENDER_PS_VA    0x%08xu\n#define RENDER_DYN_VA   0x%08xu\n#define RENDER_SURF_VA  0x%08xu\n"
           "#define RENDER_RT_VA    0x%08xu\n#define RENDER_RT_W     %d\n#define RENDER_RT_H     %d\n#define RENDER_RT_PITCH %d\n",
           SH_VA, DYN_VA, SURF_VA, RT_VA, RT_W, RT_H, RT_PITCH);
    n = 0; drawA(); dump("kRenderCmds", out, n, 6);
    n = 0; drawB(0); dump("kRender2Cmds", out, n, 6);
    // draw C (5b): draw B as a template for shaders from our compiler (irisxe-metaltest):
    // the shader-dependent fields are 0; R3_x = dword index, shift of field x
    n = 0; drawB(1); dump("kRender3Cmds", out, n, 6);
    printf("// R3_<field>: dword in kRender3Cmds, bit shift (value << shift is or'ed in)\n");
    FIELDPOS(GFX12_3DSTATE_VS, atVS, "VS_KSP", f_->KernelStartPointer = 64);           // value = address >> 6
    FIELDPOS(GFX12_3DSTATE_VS, atVS, "VS_GRF", f_->DispatchGRFStartRegisterForURBData = 1);
    FIELDPOS(GFX12_3DSTATE_VS, atVS, "VS_READLEN", f_->VertexURBEntryReadLength = 1);
    FIELDPOS(GFX12_3DSTATE_CLIP, atCLIP, "CLIP_NONPERSP", f_->NonPerspectiveBarycentricEnable = 1);
    FIELDPOS(GFX12_3DSTATE_SBE, atSBE, "SBE_NUMATTR", f_->NumberofSFOutputAttributes = 1);
    FIELDPOS(GFX12_3DSTATE_SBE, atSBE, "SBE_READLEN", f_->VertexURBEntryReadLength = 1);
    FIELDPOS(GFX12_3DSTATE_SBE, atSBE, "SBE_CONSTINTERP", f_->ConstantInterpolationEnable = 1);
    FIELDPOS(GFX12_3DSTATE_SBE_SWIZ, atSWIZ, "SWIZ_SRC0", f_->Attribute[0].SourceAttribute = 1);
    FIELDPOS(GFX12_3DSTATE_SBE_SWIZ, atSWIZ, "SWIZ_SRC1", f_->Attribute[1].SourceAttribute = 1);
    FIELDPOS(GFX12_3DSTATE_WM, atWM, "WM_BARY", f_->BarycentricInterpolationMode = 1);
    FIELDPOS(GFX12_3DSTATE_WM, atWM, "WM_FORCE", f_->ForceThreadDispatchEnable = 1);
    FIELDPOS(GFX12_3DSTATE_PS, atPS, "PS_KSP", f_->KernelStartPointer0 = 64);
    FIELDPOS(GFX12_3DSTATE_PS, atPS, "PS_GRF", f_->DispatchGRFStartRegisterForConstantSetupData0 = 1);
    FIELDPOS(GFX12_3DSTATE_PS_EXTRA, atPSX, "PSX_ATTR", f_->AttributeEnable = 1);
    FIELDPOS(GFX12_3DSTATE_PS_EXTRA, atPSX, "PSX_KILL", f_->PixelShaderKillsPixel = 1);

    // ---- 5c: Metal draw templates
    printf("\n// ---- milestone 5c: Metal draws (IrisXeGPU.cpp mtlRunDraw3D) ----\n");
    printf("#define DRW_VA 0x%08xu\n#define DRW_DYN_OFF 0x1000\n#define DRW_SURF_OFF 0x2000\n", DRW_VA);
    printf("#define DYN_BLEND 0x%x\n#define DYN_CC 0x%x\n#define DYN_CCVP 0x%x\n#define DYN_SFVP 0x%x\n#define DYN_SAMP 0x%x\n"
           "#define DYN_BORDER 0x%x\n#define DYN_PUSHVS 0x%x\n#define DYN_PUSHPS 0x%x\n#define DYN_CLEAR 0x%x\n",
           DYN_BLEND, DYN_CC, DYN_CCVP, DYN_SFVP, DYN_SAMP, DYN_BORDER, DYN_PUSHVS, DYN_PUSHPS, DYN_CLEAR);
    printf("#define DYN_SCISSOR 0x%x\n#define DYN_BLENDCLR 0x%x\n", DYN_SCISSOR, DYN_BLENDCLR);
    printf("#define SRF_BTNULL 0x180   // v0.34.0: one entry -> SRF_SSNULL (depth / stencil clears write no colour)\n#define SRF_SSNULL 0x600\n");
    printf("#define SRF_BTPS 0x%x\n#define SRF_BTVS 0x%x\n#define SRF_BTCLR 0x%x\n#define SRF_SS 0x%x\n#define DRW_URB_SLOTS %d\n",
           SRF_BTPS, SRF_BTVS, SRF_BTCLR, SRF_SS, DRW_URB_SIZE * 4);
    n = 0; drawD();
    printf("// draw D: parts [0, D_VB) [D_VB, D_INST) [D_INST, end); the kext puts 3DSTATE_VERTEX_BUFFERS /\n"
           "// _ELEMENTS at D_VB and one 3DSTATE_VF_INSTANCING per element at D_INST\n");
    printf("#define D_AT_VB %d\n#define D_AT_INST %d\n", atD[D_VB], atD[D_INST]);
    dump("kDrawCmds", out, n, 6);
    FIELDPOSP("RD", GFX12_3DSTATE_DRAWING_RECTANGLE, atD[D_RECT], "RECT_XMAX", f_->ClippedDrawingRectangleXMax = 1);
    FIELDPOSP("RD", GFX12_3DSTATE_DRAWING_RECTANGLE, atD[D_RECT], "RECT_YMAX", f_->ClippedDrawingRectangleYMax = 1);
    FIELDPOSP("RD", GFX12_3DSTATE_VF_SGVS, atD[D_SGVS], "SGVS_VID_EN", f_->VertexIDEnable = 1);
    FIELDPOSP("RD", GFX12_3DSTATE_VF_SGVS, atD[D_SGVS], "SGVS_VID_COMP", f_->VertexIDComponentNumber = 1);
    FIELDPOSP("RD", GFX12_3DSTATE_VF_SGVS, atD[D_SGVS], "SGVS_VID_ELEM", f_->VertexIDElementOffset = 1);
    FIELDPOSP("RD", GFX12_3DSTATE_VF_SGVS, atD[D_SGVS], "SGVS_IID_EN", f_->InstanceIDEnable = 1);
    FIELDPOSP("RD", GFX12_3DSTATE_VF_SGVS, atD[D_SGVS], "SGVS_IID_COMP", f_->InstanceIDComponentNumber = 1);
    FIELDPOSP("RD", GFX12_3DSTATE_VF_SGVS, atD[D_SGVS], "SGVS_IID_ELEM", f_->InstanceIDElementOffset = 1);
    FIELDPOSP("RD", GFX12_3DSTATE_VS, atD[D_VS], "VS_KSP", f_->KernelStartPointer = 64);
    FIELDPOSP("RD", GFX12_3DSTATE_VS, atD[D_VS], "VS_GRF", f_->DispatchGRFStartRegisterForURBData = 1);
    FIELDPOSP("RD", GFX12_3DSTATE_VS, atD[D_VS], "VS_READLEN", f_->VertexURBEntryReadLength = 1);
    FIELDPOSP("RD", GFX12_3DSTATE_VS, atD[D_VS], "VS_BTCOUNT", f_->BindingTableEntryCount = 1);
    FIELDPOSP("RD", GFX12_3DSTATE_CLIP, atD[D_CLIP], "CLIP_NONPERSP", f_->NonPerspectiveBarycentricEnable = 1);
    FIELDPOSP("RD", GFX12_3DSTATE_CLIP, atD[D_CLIP], "CLIP_XYTEST", f_->ViewportXYClipTestEnable = 1);
    FIELDPOSP("RD", GFX12_3DSTATE_RASTER, atD[D_RASTER], "RASTER_CULL", f_->CullMode = 1);
    FIELDPOSP("RD", GFX12_3DSTATE_RASTER, atD[D_RASTER], "RASTER_FRONTCCW", f_->FrontWinding = 1);
    FIELDPOSP("RD", GFX12_3DSTATE_RASTER, atD[D_RASTER], "RASTER_FILLF", f_->FrontFaceFillMode = 1);
    FIELDPOSP("RD", GFX12_3DSTATE_RASTER, atD[D_RASTER], "RASTER_FILLB", f_->BackFaceFillMode = 1);
    FIELDPOSP("RD", GFX12_3DSTATE_SBE, atD[D_SBE], "SBE_NUMATTR", f_->NumberofSFOutputAttributes = 1);
    FIELDPOSP("RD", GFX12_3DSTATE_SBE, atD[D_SBE], "SBE_READLEN", f_->VertexURBEntryReadLength = 1);
    FIELDPOSP("RD", GFX12_3DSTATE_SBE, atD[D_SBE], "SBE_CONSTINTERP", f_->ConstantInterpolationEnable = 1);
    FIELDPOSP("RD", GFX12_3DSTATE_SBE_SWIZ, atD[D_SWIZ], "SWIZ_SRC0", f_->Attribute[0].SourceAttribute = 1);
    FIELDPOSP("RD", GFX12_3DSTATE_WM, atD[D_WM], "WM_BARY", f_->BarycentricInterpolationMode = 1);
    FIELDPOSP("RD", GFX12_3DSTATE_WM, atD[D_WM], "WM_FORCE", f_->ForceThreadDispatchEnable = 1);
    FIELDPOSP("RD", GFX12_3DSTATE_PS, atD[D_PS], "PS_KSP", f_->KernelStartPointer0 = 64);
    FIELDPOSP("RD", GFX12_3DSTATE_PS, atD[D_PS], "PS_GRF", f_->DispatchGRFStartRegisterForConstantSetupData0 = 1);
    FIELDPOSP("RD", GFX12_3DSTATE_PS, atD[D_PS], "PS_PUSH", f_->PushConstantEnable = 1);
    FIELDPOSP("RD", GFX12_3DSTATE_PS, atD[D_PS], "PS_SAMPCOUNT", f_->SamplerCount = 1);
    FIELDPOSP("RD", GFX12_3DSTATE_PS_EXTRA, atD[D_PSX], "PSX_ATTR", f_->AttributeEnable = 1);
    FIELDPOSP("RD", GFX12_3DSTATE_PS_EXTRA, atD[D_PSX], "PSX_KILL", f_->PixelShaderKillsPixel = 1);
    FIELDPOSB("RD", GFX12_3DSTATE_CONSTANT_VS, atD[D_CVS], "CVS_LEN", f_->MOCS = mocs, f_->ConstantBody.ReadLength[0] = 1);
    FIELDPOSB("RD", GFX12_3DSTATE_CONSTANT_VS, atD[D_CVS], "CVS_ADDR", f_->MOCS = mocs, f_->ConstantBody.Buffer[0] = 32);
    FIELDPOSB("RD", GFX12_3DSTATE_CONSTANT_PS, atD[D_CPS], "CPS_LEN", f_->MOCS = mocs, f_->ConstantBody.ReadLength[0] = 1);
    FIELDPOSB("RD", GFX12_3DSTATE_CONSTANT_PS, atD[D_CPS], "CPS_ADDR", f_->MOCS = mocs, f_->ConstantBody.Buffer[0] = 32);
    FIELDPOSP("RD", GFX12_3DSTATE_VF_TOPOLOGY, atD[D_TOPO], "TOPO", f_->PrimitiveTopologyType = 1);
    FIELDPOSP("RD", GFX12_3DPRIMITIVE, atD[D_PRIM], "PRIM_TOPO", f_->PrimitiveTopologyType = 1);
    FIELDPOSP("RD", GFX12_3DPRIMITIVE, atD[D_PRIM], "PRIM_COUNT", f_->VertexCountPerInstance = 1);
    FIELDPOSP("RD", GFX12_3DPRIMITIVE, atD[D_PRIM], "PRIM_START", f_->StartVertexLocation = 1);
    FIELDPOSP("RD", GFX12_3DPRIMITIVE, atD[D_PRIM], "PRIM_INST", f_->InstanceCount = 1);
    FIELDPOSP("RD", GFX12_3DPRIMITIVE, atD[D_PRIM], "PRIM_BASEINST", f_->StartInstanceLocation = 1);
    // v0.33.0: indexed draws, blending
    FIELDPOSP("RD", GFX12_3DPRIMITIVE, atD[D_PRIM], "PRIM_ACCESS", f_->VertexAccessType = 1);
    FIELDPOSP("RD", GFX12_3DPRIMITIVE, atD[D_PRIM], "PRIM_BASEVTX", f_->BaseVertexLocation = 1);
    FIELDPOSP("RD", GFX12_3DSTATE_VF, atD[D_VF], "VF_CUTEN", f_->IndexedDrawCutIndexEnable = 1);
    FIELDPOSP("RD", GFX12_3DSTATE_VF, atD[D_VF], "VF_CUT", f_->CutIndex = 1);
    FIELDPOSB("RD", GFX12_3DSTATE_INDEX_BUFFER, atD[D_IB], "IB_FMT", f_->MOCS = mocs, f_->IndexFormat = 1);
    FIELDPOSB("RD", GFX12_3DSTATE_INDEX_BUFFER, atD[D_IB], "IB_ADDR", f_->MOCS = mocs, f_->BufferStartingAddress = 1);
    FIELDPOSB("RD", GFX12_3DSTATE_INDEX_BUFFER, atD[D_IB], "IB_SIZE", f_->MOCS = mocs, f_->BufferSize = 1);
    FIELDPOSP("RD", GFX12_3DSTATE_PS_BLEND, atD[D_PSB], "PSB_ENABLE", f_->ColorBufferBlendEnable = 1);
    FIELDPOSP("RD", GFX12_3DSTATE_PS_BLEND, atD[D_PSB], "PSB_SRC", f_->SourceBlendFactor = 1);
    FIELDPOSP("RD", GFX12_3DSTATE_PS_BLEND, atD[D_PSB], "PSB_DST", f_->DestinationBlendFactor = 1);
    FIELDPOSP("RD", GFX12_3DSTATE_PS_BLEND, atD[D_PSB], "PSB_SRCA", f_->SourceAlphaBlendFactor = 1);
    FIELDPOSP("RD", GFX12_3DSTATE_PS_BLEND, atD[D_PSB], "PSB_DSTA", f_->DestinationAlphaBlendFactor = 1);
    FIELDPOSP("RD", GFX12_3DSTATE_PS_BLEND, atD[D_PSB], "PSB_INDEP", f_->IndependentAlphaBlendEnable = 1);
    FIELDPOSE("BLS", GFX12_BLEND_STATE, "INDEP", f_->IndependentAlphaBlendEnable = 1);
    FIELDPOSE("BLE", GFX12_BLEND_STATE_ENTRY, "ENABLE", f_->ColorBufferBlendEnable = 1);
    FIELDPOSE("BLE", GFX12_BLEND_STATE_ENTRY, "SRC", f_->SourceBlendFactor = 1);
    FIELDPOSE("BLE", GFX12_BLEND_STATE_ENTRY, "DST", f_->DestinationBlendFactor = 1);
    FIELDPOSE("BLE", GFX12_BLEND_STATE_ENTRY, "FUNC", f_->ColorBlendFunction = 1);
    FIELDPOSE("BLE", GFX12_BLEND_STATE_ENTRY, "SRCA", f_->SourceAlphaBlendFactor = 1);
    FIELDPOSE("BLE", GFX12_BLEND_STATE_ENTRY, "DSTA", f_->DestinationAlphaBlendFactor = 1);
    FIELDPOSE("BLE", GFX12_BLEND_STATE_ENTRY, "FUNCA", f_->AlphaBlendFunction = 1);
    FIELDPOSE("BLE", GFX12_BLEND_STATE_ENTRY, "WD_R", f_->WriteDisableRed = 1);
    FIELDPOSE("BLE", GFX12_BLEND_STATE_ENTRY, "WD_G", f_->WriteDisableGreen = 1);
    FIELDPOSE("BLE", GFX12_BLEND_STATE_ENTRY, "WD_B", f_->WriteDisableBlue = 1);
    FIELDPOSE("BLE", GFX12_BLEND_STATE_ENTRY, "WD_A", f_->WriteDisableAlpha = 1);
    FIELDPOSE("SCR", GFX12_SCISSOR_RECT, "XMIN", f_->ScissorRectangleXMin = 1);
    FIELDPOSE("SCR", GFX12_SCISSOR_RECT, "YMIN", f_->ScissorRectangleYMin = 1);
    FIELDPOSE("SCR", GFX12_SCISSOR_RECT, "XMAX", f_->ScissorRectangleXMax = 1);
    FIELDPOSE("SCR", GFX12_SCISSOR_RECT, "YMAX", f_->ScissorRectangleYMax = 1);
    FIELDPOSE("CCS", GFX12_COLOR_CALC_STATE, "BLEND_R", f_->BlendConstantColorRed = 1.0f);
    FIELDPOSE("CCS", GFX12_COLOR_CALC_STATE, "BLEND_G", f_->BlendConstantColorGreen = 1.0f);
    FIELDPOSE("CCS", GFX12_COLOR_CALC_STATE, "BLEND_B", f_->BlendConstantColorBlue = 1.0f);
    FIELDPOSE("CCS", GFX12_COLOR_CALC_STATE, "BLEND_A", f_->BlendConstantColorAlpha = 1.0f);
    printf("#define BLS_LENGTH %d   // BLEND_STATE dwords before entry 0 (2 per entry)\n", GFX12_BLEND_STATE_length);
    // v0.34.0: depth / stencil (draw "RD", clear "RC")
#define DSPOS(PFX, AT, ATDB, ATSB) \
    FIELDPOSB(PFX, GFX12_3DSTATE_DEPTH_BUFFER, ATDB, "DB_TYPE", f_->MOCS = mocs, f_->SurfaceType = 1); \
    FIELDPOSB(PFX, GFX12_3DSTATE_DEPTH_BUFFER, ATDB, "DB_FMT", f_->MOCS = mocs, f_->SurfaceFormat = 1); \
    FIELDPOSB(PFX, GFX12_3DSTATE_DEPTH_BUFFER, ATDB, "DB_WRITE", f_->MOCS = mocs, f_->DepthWriteEnable = 1); \
    FIELDPOSB(PFX, GFX12_3DSTATE_DEPTH_BUFFER, ATDB, "DB_PITCH", f_->MOCS = mocs, f_->SurfacePitch = 1); \
    FIELDPOSB(PFX, GFX12_3DSTATE_DEPTH_BUFFER, ATDB, "DB_ADDR", f_->MOCS = mocs, f_->SurfaceBaseAddress = 1); \
    FIELDPOSB(PFX, GFX12_3DSTATE_DEPTH_BUFFER, ATDB, "DB_W", f_->MOCS = mocs, f_->Width = 1); \
    FIELDPOSB(PFX, GFX12_3DSTATE_DEPTH_BUFFER, ATDB, "DB_H", f_->MOCS = mocs, f_->Height = 1); \
    FIELDPOSB(PFX, GFX12_3DSTATE_DEPTH_BUFFER, ATDB, "DB_QPITCH", f_->MOCS = mocs, f_->SurfaceQPitch = 1); \
    FIELDPOSB(PFX, GFX12_3DSTATE_STENCIL_BUFFER, ATSB, "SB_TYPE", f_->MOCS = mocs, f_->SurfaceType = 1); \
    FIELDPOSB(PFX, GFX12_3DSTATE_STENCIL_BUFFER, ATSB, "SB_WRITE", f_->MOCS = mocs, f_->StencilWriteEnable = 1); \
    FIELDPOSB(PFX, GFX12_3DSTATE_STENCIL_BUFFER, ATSB, "SB_PITCH", f_->MOCS = mocs, f_->SurfacePitch = 1); \
    FIELDPOSB(PFX, GFX12_3DSTATE_STENCIL_BUFFER, ATSB, "SB_ADDR", f_->MOCS = mocs, f_->SurfaceBaseAddress = 1); \
    FIELDPOSB(PFX, GFX12_3DSTATE_STENCIL_BUFFER, ATSB, "SB_W", f_->MOCS = mocs, f_->Width = 1); \
    FIELDPOSB(PFX, GFX12_3DSTATE_STENCIL_BUFFER, ATSB, "SB_H", f_->MOCS = mocs, f_->Height = 1); \
    FIELDPOSB(PFX, GFX12_3DSTATE_STENCIL_BUFFER, ATSB, "SB_QPITCH", f_->MOCS = mocs, f_->SurfaceQPitch = 1); \
    FIELDPOSP(PFX, GFX12_3DSTATE_WM_DEPTH_STENCIL, AT, "DS_DTEST", f_->DepthTestEnable = 1); \
    FIELDPOSP(PFX, GFX12_3DSTATE_WM_DEPTH_STENCIL, AT, "DS_DWRITE", f_->DepthBufferWriteEnable = 1); \
    FIELDPOSP(PFX, GFX12_3DSTATE_WM_DEPTH_STENCIL, AT, "DS_DFUNC", f_->DepthTestFunction = 1); \
    FIELDPOSP(PFX, GFX12_3DSTATE_WM_DEPTH_STENCIL, AT, "DS_STEST", f_->StencilTestEnable = 1); \
    FIELDPOSP(PFX, GFX12_3DSTATE_WM_DEPTH_STENCIL, AT, "DS_SWRITE", f_->StencilBufferWriteEnable = 1); \
    FIELDPOSP(PFX, GFX12_3DSTATE_WM_DEPTH_STENCIL, AT, "DS_2SIDED", f_->DoubleSidedStencilEnable = 1); \
    FIELDPOSP(PFX, GFX12_3DSTATE_WM_DEPTH_STENCIL, AT, "DS_SFUNC", f_->StencilTestFunction = 1); \
    FIELDPOSP(PFX, GFX12_3DSTATE_WM_DEPTH_STENCIL, AT, "DS_SFAIL", f_->StencilFailOp = 1); \
    FIELDPOSP(PFX, GFX12_3DSTATE_WM_DEPTH_STENCIL, AT, "DS_SZFAIL", f_->StencilPassDepthFailOp = 1); \
    FIELDPOSP(PFX, GFX12_3DSTATE_WM_DEPTH_STENCIL, AT, "DS_SPASS", f_->StencilPassDepthPassOp = 1); \
    FIELDPOSP(PFX, GFX12_3DSTATE_WM_DEPTH_STENCIL, AT, "DS_BFUNC", f_->BackfaceStencilTestFunction = 1); \
    FIELDPOSP(PFX, GFX12_3DSTATE_WM_DEPTH_STENCIL, AT, "DS_BFAIL", f_->BackfaceStencilFailOp = 1); \
    FIELDPOSP(PFX, GFX12_3DSTATE_WM_DEPTH_STENCIL, AT, "DS_BZFAIL", f_->BackfaceStencilPassDepthFailOp = 1); \
    FIELDPOSP(PFX, GFX12_3DSTATE_WM_DEPTH_STENCIL, AT, "DS_BPASS", f_->BackfaceStencilPassDepthPassOp = 1); \
    FIELDPOSP(PFX, GFX12_3DSTATE_WM_DEPTH_STENCIL, AT, "DS_SMASK", f_->StencilTestMask = 1); \
    FIELDPOSP(PFX, GFX12_3DSTATE_WM_DEPTH_STENCIL, AT, "DS_SWMASK", f_->StencilWriteMask = 1); \
    FIELDPOSP(PFX, GFX12_3DSTATE_WM_DEPTH_STENCIL, AT, "DS_BMASK", f_->BackfaceStencilTestMask = 1); \
    FIELDPOSP(PFX, GFX12_3DSTATE_WM_DEPTH_STENCIL, AT, "DS_BWMASK", f_->BackfaceStencilWriteMask = 1); \
    FIELDPOSP(PFX, GFX12_3DSTATE_WM_DEPTH_STENCIL, AT, "DS_SREF", f_->StencilReferenceValue = 1); \
    FIELDPOSP(PFX, GFX12_3DSTATE_WM_DEPTH_STENCIL, AT, "DS_BREF", f_->BackfaceStencilReferenceValue = 1);
    DSPOS("RD", atD[D_WMDS], atD[D_DB], atD[D_SB])
    FIELDPOSP("RD", GFX12_3DSTATE_RASTER, atD[D_RASTER], "RASTER_ZNEAR", f_->ViewportZNearClipTestEnable = 1);
    FIELDPOSP("RD", GFX12_3DSTATE_RASTER, atD[D_RASTER], "RASTER_ZFAR", f_->ViewportZFarClipTestEnable = 1);
    FIELDPOSP("RD", GFX12_3DSTATE_RASTER, atD[D_RASTER], "RASTER_DOSOLID", f_->GlobalDepthOffsetEnableSolid = 1);
    FIELDPOSP("RD", GFX12_3DSTATE_RASTER, atD[D_RASTER], "RASTER_DOWIRE", f_->GlobalDepthOffsetEnableWireframe = 1);
    FIELDPOSP("RD", GFX12_3DSTATE_RASTER, atD[D_RASTER], "RASTER_DOPOINT", f_->GlobalDepthOffsetEnablePoint = 1);
    FIELDPOSP("RD", GFX12_3DSTATE_RASTER, atD[D_RASTER], "RASTER_DOCONST", f_->GlobalDepthOffsetConstant = 1.0f);  // whole dword
    FIELDPOSP("RD", GFX12_3DSTATE_RASTER, atD[D_RASTER], "RASTER_DOSCALE", f_->GlobalDepthOffsetScale = 1.0f);
    FIELDPOSP("RD", GFX12_3DSTATE_RASTER, atD[D_RASTER], "RASTER_DOCLAMP", f_->GlobalDepthOffsetClamp = 1.0f);
    FIELDPOSP("RD", GFX12_3DSTATE_PS_EXTRA, atD[D_PSX], "PSX_CDEPTH", f_->PixelShaderComputedDepthMode = 1);
    FIELDPOSP("RD", GFX12_3DSTATE_PS_EXTRA, atD[D_PSX], "PSX_SRCDEPTH", f_->PixelShaderUsesSourceDepth = 1);   // v0.35.0
    printf("#define GEN_D32_FLOAT %d\n#define GEN_D16_UNORM %d\n#define GEN_SURFTYPE_2D %d\n#define GEN_SURFTYPE_NULL %d\n",
           D32_FLOAT, D16_UNORM, SURFTYPE_2D, SURFTYPE_NULL);
    {   // MTLCompareFunction 0..7, MTLStencilOperation 0..7 -> Gen
        static const int cf[8] = { COMPAREFUNCTION_NEVER, COMPAREFUNCTION_LESS, COMPAREFUNCTION_EQUAL, COMPAREFUNCTION_LEQUAL,
            COMPAREFUNCTION_GREATER, COMPAREFUNCTION_NOTEQUAL, COMPAREFUNCTION_GEQUAL, COMPAREFUNCTION_ALWAYS };
        static const int so[8] = { STENCILOP_KEEP, STENCILOP_ZERO, STENCILOP_REPLACE, STENCILOP_INCRSAT, STENCILOP_DECRSAT,
            STENCILOP_INVERT, STENCILOP_INCR, STENCILOP_DECR };
        printf("static const UInt8 kGenCompare[8] = {"); for (int i = 0; i < 8; i++) printf(" %d,", cf[i]); printf(" };\n");
        printf("static const UInt8 kGenStencilOp[8] = {"); for (int i = 0; i < 8; i++) printf(" %d,", so[i]); printf(" };\n");
        printf("#define GEN_COMPARE_ALWAYS %d\n#define GEN_STENCILOP_REPLACE %d\n", COMPAREFUNCTION_ALWAYS, STENCILOP_REPLACE);
    }
    printf("#define GEN_INDEX_WORD %d\n#define GEN_INDEX_DWORD %d\n#define GEN_RANDOM %d\n", INDEX_WORD, INDEX_DWORD, RANDOM);
    {   // Metal enums -> Gen: MTLBlendFactor 0..14 (no dual source), MTLBlendOperation 0..4
        static const int bf[15] = { BLENDFACTOR_ZERO, BLENDFACTOR_ONE, BLENDFACTOR_SRC_COLOR, BLENDFACTOR_INV_SRC_COLOR,
            BLENDFACTOR_SRC_ALPHA, BLENDFACTOR_INV_SRC_ALPHA, BLENDFACTOR_DST_COLOR, BLENDFACTOR_INV_DST_COLOR,
            BLENDFACTOR_DST_ALPHA, BLENDFACTOR_INV_DST_ALPHA, BLENDFACTOR_SRC_ALPHA_SATURATE, BLENDFACTOR_CONST_COLOR,
            BLENDFACTOR_INV_CONST_COLOR, BLENDFACTOR_CONST_ALPHA, BLENDFACTOR_INV_CONST_ALPHA };
        static const int bo[5] = { BLENDFUNCTION_ADD, BLENDFUNCTION_SUBTRACT, BLENDFUNCTION_REVERSE_SUBTRACT, BLENDFUNCTION_MIN, BLENDFUNCTION_MAX };
        printf("static const UInt8 kGenBlendFactor[15] = {"); for (int i = 0; i < 15; i++) printf(" %d,", bf[i]); printf(" };\n");
        printf("static const UInt8 kGenBlendOp[5] = {"); for (int i = 0; i < 5; i++) printf(" %d,", bo[i]); printf(" };\n");
        printf("#define GEN_BLENDFACTOR_ONE %d\n", BLENDFACTOR_ONE);
    }
    // packets the kext builds: headers and field positions inside one entry
    {
        struct GFX12_3DSTATE_VERTEX_BUFFERS h = { GFX12_3DSTATE_VERTEX_BUFFERS_header }; uint32_t w;
        h.DWordLength = 0; GFX12_3DSTATE_VERTEX_BUFFERS_pack(NULL, &w, &h);
        printf("#define VB_HEADER 0x%08xu   // + (1 + 4 n) - 2 in bits 7:0\n", w);
        struct GFX12_3DSTATE_VERTEX_ELEMENTS e = { GFX12_3DSTATE_VERTEX_ELEMENTS_header };
        e.DWordLength = 0; GFX12_3DSTATE_VERTEX_ELEMENTS_pack(NULL, &w, &e);
        printf("#define VE_HEADER 0x%08xu   // + (1 + 2 n) - 2\n", w);
        uint32_t t[4] = { 0 };
        struct GFX12_VERTEX_BUFFER_STATE v = { .AddressModifyEnable = true, .MOCS = mocs, .L3BypassDisable = true };
        GFX12_VERTEX_BUFFER_STATE_pack(NULL, t, &v);
        printf("#define VBS_DW0 0x%08xu   // | index, pitch; DW1-2 address, DW3 size\n", t[0]);
        FIELDPOSS("VBS", GFX12_VERTEX_BUFFER_STATE, "INDEX", f_->VertexBufferIndex = 1);
        FIELDPOSS("VBS", GFX12_VERTEX_BUFFER_STATE, "PITCH", f_->BufferPitch = 1);
        FIELDPOSS("VBS", GFX12_VERTEX_BUFFER_STATE, "ADDR", f_->BufferStartingAddress = 1);
        FIELDPOSS("VBS", GFX12_VERTEX_BUFFER_STATE, "SIZE", f_->BufferSize = 1);
        FIELDPOSE("VES", GFX12_VERTEX_ELEMENT_STATE, "OFFSET", f_->SourceElementOffset = 1);
        FIELDPOSE("VES", GFX12_VERTEX_ELEMENT_STATE, "FORMAT", f_->SourceElementFormat = 1);
        FIELDPOSE("VES", GFX12_VERTEX_ELEMENT_STATE, "VALID", f_->Valid = 1);
        FIELDPOSE("VES", GFX12_VERTEX_ELEMENT_STATE, "VB", f_->VertexBufferIndex = 1);
        FIELDPOSE("VES", GFX12_VERTEX_ELEMENT_STATE, "C0", f_->Component0Control = 1);
        FIELDPOSE("VES", GFX12_VERTEX_ELEMENT_STATE, "C1", f_->Component1Control = 1);
        FIELDPOSE("VES", GFX12_VERTEX_ELEMENT_STATE, "C2", f_->Component2Control = 1);
        FIELDPOSE("VES", GFX12_VERTEX_ELEMENT_STATE, "C3", f_->Component3Control = 1);
        struct GFX12_3DSTATE_VF_INSTANCING vi = { GFX12_3DSTATE_VF_INSTANCING_header }; uint32_t vt[3];
        GFX12_3DSTATE_VF_INSTANCING_pack(NULL, vt, &vi);
        printf("#define VFI_DW0 0x%08xu   // 3 dwords\n", vt[0]);
        FIELDPOSP("VFI", GFX12_3DSTATE_VF_INSTANCING, 0, "INDEX", f_->VertexElementIndex = 1);
        FIELDPOSP("VFI", GFX12_3DSTATE_VF_INSTANCING, 0, "ENABLE", f_->InstancingEnable = 1);
        FIELDPOSP("VFI", GFX12_3DSTATE_VF_INSTANCING, 0, "STEP", f_->InstanceDataStepRate = 1);
        printf("#define VFCOMP_NOSTORE_ 0\n#define VFCOMP_SRC_ 1\n#define VFCOMP_0_ 2\n#define VFCOMP_1FP_ 3\n#define VFCOMP_1INT_ 4\n");
    }
    n = 0; drawClr();
    printf("#define C_AT_END %d\n", atC[C_END]);
    dump("kClearCmds", out, n, 6);
    FIELDPOSP("RC", GFX12_3DSTATE_DRAWING_RECTANGLE, atC[C_RECT], "RECT_XMAX", f_->ClippedDrawingRectangleXMax = 1);
    FIELDPOSP("RC", GFX12_3DSTATE_DRAWING_RECTANGLE, atC[C_RECT], "RECT_YMAX", f_->ClippedDrawingRectangleYMax = 1);
    FIELDPOSB("RC", GFX12_3DSTATE_CONSTANT_PS, atC[C_CPS], "CPS_ADDR", f_->MOCS = mocs, f_->ConstantBody.Buffer[0] = 32);
    FIELDPOSP("RC", GFX12_3DSTATE_BINDING_TABLE_POINTERS_PS, atC[C_BT], "BT", f_->PointertoPSBindingTable = 32);
    DSPOS("RC", atC[C_WMDS], atC[C_DB], atC[C_SB])
    {   // dynamic-state blobs: blend (8 entries, all channels), CC, CC viewport; SF_CLIP_VIEWPORT field dwords
        uint32_t d[64] = { 0 };
        struct GFX12_BLEND_STATE b = { };
        GFX12_BLEND_STATE_pack(NULL, d, &b);
        for (int k = 0; k < 8; k++) {
            struct GFX12_BLEND_STATE_ENTRY e = { .PreBlendColorClampEnable = true, .PostBlendColorClampEnable = true,
                                                 .ColorClampRange = COLORCLAMP_RTFORMAT };
            GFX12_BLEND_STATE_ENTRY_pack(NULL, &d[GFX12_BLEND_STATE_length + 2 * k], &e);
        }
        dump("kDrawBlend", d, GFX12_BLEND_STATE_length + 16, 6);
        memset(d, 0, sizeof(d));
        struct GFX12_CC_VIEWPORT vp = { .MinimumDepth = 0.0f, .MaximumDepth = 1.0f };
        GFX12_CC_VIEWPORT_pack(NULL, d, &vp);
        dump("kDrawCCVP", d, GFX12_CC_VIEWPORT_length, 6);
#define FDW(NAME, F) do { struct GFX12_SF_CLIP_VIEWPORT a_ = { 0 }, b_ = { 0 }; b_.F = 1.0f; uint32_t pa_[16] = { 0 }, pb_[16] = { 0 }; \
        GFX12_SF_CLIP_VIEWPORT_pack(NULL, pa_, &a_); GFX12_SF_CLIP_VIEWPORT_pack(NULL, pb_, &b_); \
        int i_; for (i_ = 0; i_ < 16 && pa_[i_] == pb_[i_]; i_++) ; printf("#define SFVP_%-6s %d\n", NAME, i_); } while (0)
        FDW("M00", ViewportMatrixElementm00); FDW("M11", ViewportMatrixElementm11); FDW("M22", ViewportMatrixElementm22);
        FDW("M30", ViewportMatrixElementm30); FDW("M31", ViewportMatrixElementm31); FDW("M32", ViewportMatrixElementm32);
        FDW("GXMIN", XMinClipGuardband); FDW("GXMAX", XMaxClipGuardband); FDW("GYMIN", YMinClipGuardband); FDW("GYMAX", YMaxClipGuardband);
        FDW("VXMIN", XMinViewPort); FDW("VXMAX", XMaxViewPort); FDW("VYMIN", YMinViewPort); FDW("VYMAX", YMaxViewPort);
    }

    // dynamic state page (0x20004000)
    {
        struct GFX12_BLEND_STATE b = { };
        GFX12_BLEND_STATE_pack(NULL, &dyn[BLEND_OFF / 4], &b);
        struct GFX12_BLEND_STATE_ENTRY e = { .PreBlendColorClampEnable = true, .PostBlendColorClampEnable = true,
                                             .ColorClampRange = COLORCLAMP_RTFORMAT };
        GFX12_BLEND_STATE_ENTRY_pack(NULL, &dyn[BLEND_OFF / 4 + GFX12_BLEND_STATE_length], &e);
        struct GFX12_COLOR_CALC_STATE cc = { };
        GFX12_COLOR_CALC_STATE_pack(NULL, &dyn[CC_OFF / 4], &cc);
        struct GFX12_CC_VIEWPORT vp = { .MinimumDepth = 0.0f, .MaximumDepth = 1.0f };
        GFX12_CC_VIEWPORT_pack(NULL, &dyn[CCVP_OFF / 4], &vp);
        // Metal viewport (0, 0, 64, 64, znear 0, zfar 1): x = (x+1) w/2, y = (1-y) h/2, z = z
        // guardband: Mesa intel_calculate_guardband_size
        float m00 = RT_W / 2.0f, m11 = -RT_H / 2.0f, m30 = RT_W / 2.0f, m31 = RT_H / 2.0f;
        float gx0, gx1, gy0, gy1;
        intel_calculate_guardband_size(0, RT_W - 1, 0, RT_H - 1, m00, m11, m30, m31, &gx0, &gx1, &gy0, &gy1);
        struct GFX12_SF_CLIP_VIEWPORT sv = {
            .ViewportMatrixElementm00 = m00, .ViewportMatrixElementm11 = m11, .ViewportMatrixElementm22 = 1.0f,
            .ViewportMatrixElementm30 = m30, .ViewportMatrixElementm31 = m31, .ViewportMatrixElementm32 = 0.0f,
            .XMinClipGuardband = gx0, .XMaxClipGuardband = gx1, .YMinClipGuardband = gy0, .YMaxClipGuardband = gy1,
            .XMinViewPort = 0, .XMaxViewPort = RT_W - 1, .YMinViewPort = 0, .YMaxViewPort = RT_H - 1 };
        GFX12_SF_CLIP_VIEWPORT_pack(NULL, &dyn[SFVP_OFF / 4], &sv);
    }
    // surface page (0x20005000)
    {
        surf[BT_OFF / 4] = SS_OFF;                     // BT entry 0 -> RT surface state (offset from surface base)
        struct GFX12_RENDER_SURFACE_STATE ss = {
            .SurfaceType = SURFTYPE_2D, .SurfaceFormat = 0x0C7 /* R8G8B8A8_UNORM */,
            .TileMode = LINEAR, .SurfaceHorizontalAlignment = HALIGN_4, .SurfaceVerticalAlignment = VALIGN_4,
            .Width = RT_W - 1, .Height = RT_H - 1, .SurfacePitch = RT_PITCH - 1, .Depth = 0,
            .MOCS = mocs, .ShaderChannelSelectRed = SCS_RED, .ShaderChannelSelectGreen = SCS_GREEN,
            .ShaderChannelSelectBlue = SCS_BLUE, .ShaderChannelSelectAlpha = SCS_ALPHA,
            .SurfaceBaseAddress = RT_VA };
        GFX12_RENDER_SURFACE_STATE_pack(NULL, &surf[SS_OFF / 4], &ss);
        // draw A, screen space: RECTLIST v0 = bottom right, v1 = bottom left, v2 = top left
        const float v[6][3] = { { 28, 28, 0 }, { 4, 28, 0 }, { 4, 4, 0 },
                                { 36, 36, 0 }, { 60, 36, 0 }, { 36, 60, 0 } };
        memcpy(&surf[VB0_OFF / 4], v, sizeof(v));
        // VB1: 16 zero bytes
        memcpy(&surf[VBB_OFF / 4], kVertsB, sizeof(kVertsB));
    }
    int dn = 0, sn = 0, hn = 0;
    for (int i = 0; i < 1024; i++) { if (dyn[i]) dn = i + 1; if (surf[i]) sn = i + 1; if (sh[i]) hn = i + 1; }
    dump("kRenderDyn", dyn, dn, 6);
    dump("kRenderSurf", surf, sn, 6);
    dump("kRenderShaders", sh, hn, 4);

    // draw B reference (the kext has no floating point): expected RGBA8 per pixel,
    // perspective-correct colour at the pixel centre; 0 = outside; 0xFFFFFFFF =
    // too close to an edge to say (fill rule)
    {
        static uint32_t ex[RT_W * RT_H];
        double sx[3], sy[3], w[3];
        for (int i = 0; i < 3; i++) {
            w[i] = kVertsB[i][3];
            sx[i] = (kVertsB[i][0] / w[i] + 1.0) * RT_W / 2.0;
            sy[i] = (1.0 - kVertsB[i][1] / w[i]) * RT_H / 2.0;
        }
        double area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (sy[1] - sy[0]);
        int inside = 0;
        for (int y = 0; y < RT_H; y++)
            for (int x = 0; x < RT_W; x++) {
                double px = x + 0.5, py = y + 0.5, l[3], dmin = 1e9;
                for (int i = 0; i < 3; i++) {
                    int a = (i + 1) % 3, b = (i + 2) % 3;
                    double e = (sx[b] - sx[a]) * (py - sy[a]) - (sy[b] - sy[a]) * (px - sx[a]);
                    l[i] = e / area;
                    double len = sqrt((sx[b] - sx[a]) * (sx[b] - sx[a]) + (sy[b] - sy[a]) * (sy[b] - sy[a]));
                    double d = l[i] * fabs(area) / len;        // signed distance to the opposite edge, pixels
                    if (d < dmin) dmin = d;
                }
                uint32_t v;
                if (dmin > 0.05) {
                    double q[3], qs = 0, c[4] = { 0, 0, 0, 0 };
                    for (int i = 0; i < 3; i++) { q[i] = l[i] / w[i]; qs += q[i]; }
                    for (int i = 0; i < 3; i++) for (int k = 0; k < 4; k++) c[k] += q[i] / qs * kVertsB[i][4 + k];
                    v = 0;
                    for (int k = 0; k < 4; k++) { int b8 = (int)floor(c[k] * 255.0 + 0.5); v |= (uint32_t)b8 << (8 * k); }
                    inside++;
                } else if (dmin < -0.05) v = 0;
                else v = 0xFFFFFFFFu;
                ex[y * RT_W + x] = v;
            }
        printf("#define RENDER2_INSIDE %d\n", inside);
        dump("kRender2Expect", ex, RT_W * RT_H, 8);
    }
    return 0;
}
