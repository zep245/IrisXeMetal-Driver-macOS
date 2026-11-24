// IrisXeGPU - GPU engines, step 4: a permanent GPU core (boot-arg -irisxegpu)
//
//   * one blitter (BCS0) context that stays set up, with a 64 KB ring buffer;
//     jobs can be submitted at any time, one after another
//   * real interrupts: an MSI handler; every job ends with MI_USER_INTERRUPT
//     and the driver learns it finished from the interrupt (a 50 ms poll is
//     only a fallback, counted separately)
//   * RC6: the GPU core is allowed to power down when idle (sequence from
//     Linux i915 gt/intel_rc6.c gen11_rc6_enable, no GuC)
//   * forcewake is only held while a job runs, so the GPU can really sleep
//   * survives system sleep: everything is set up again on wake
//
// Demo (v0.68.7: only with boot-arg -irisxedemo): 12 jobs paint colour bars on the boot screen (each job starts
// when the previous one's interrupt arrives); afterwards a small "heartbeat"
// job runs every 10 s. Status in IrisXe-GPU.
//
// Step 5 (v0.14.0): the render engine (RCS0) as a second engine. Its context
// image is the gen12 render layout (i915 gen12_rcs_offsets, 14 pages); each
// heartbeat also runs a render job: a PIPE_CONTROL (CS stall + post-sync
// write through the GGTT) - the render pipeline's own completion path.

#include "IrisXeProbe.hpp"
#include "IrisXeSpaceABI.h"
#include "TGLRegs.h"
#include <IOKit/IOFilterInterruptEventSource.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/pwr_mgt/RootDomain.h>
#include <kern/clock.h>
#include <libkern/libkern.h>
#include <string.h>
#include "IrisXeCompute.inc"
#include "IrisXeDrawTest.h"     // milestone 5b: XeDrawTestCfg
#include "IrisXeDrawABI.h"      // milestone 5c: XeDrawDesc
#include "IrisXeRender.inc"     // milestone 5a: 3D draw commands, pixel shader, state (tools/shadergen/render.c)
// v0.63.2: fill-rate test shaders (constant colour, SIMD8 / SIMD16), tools: our encoder (src/compiler/xe_enc.h)
static const UInt32 kTestPS8[] = {   // 5 instructions
    0x80030061, 0x70054aa0, 0x00000000, 0x3e800000,
    0x80030061, 0x71054aa0, 0x00000000, 0x3f000000,
    0x80030061, 0x72054aa0, 0x00000000, 0x3f400000,
    0x80030061, 0x73054aa0, 0x00000000, 0x3f800000,
    0x00030132, 0x00000004, 0x58007024, 0x00c40000,
};
static const UInt32 kTestPS16[] = {   // 9 instructions
    0x80030061, 0x70054aa0, 0x00000000, 0x3e800000,
    0x80030061, 0x71054aa0, 0x00000000, 0x3e800000,
    0x80030061, 0x72054aa0, 0x00000000, 0x3f000000,
    0x80030061, 0x73054aa0, 0x00000000, 0x3f000000,
    0x80030061, 0x74054aa0, 0x00000000, 0x3f400000,
    0x80030061, 0x75054aa0, 0x00000000, 0x3f400000,
    0x80030061, 0x76054aa0, 0x00000000, 0x3f800000,
    0x80030061, 0x77054aa0, 0x00000000, 0x3f800000,
    0x00040132, 0x00000004, 0x50007044, 0x00c40000,
};
#define TESTPS8_VA   (RENDER_DYN_VA + 0x800)        // render page 18 (320 bytes used)
#define TESTPS16_VA  (RENDER_DYN_VA + 0xA00)
#define SLOT_VA(i)        (DRW_VA + 0x30000u + (UInt32)(i) * 0x4000u)     // GPU 0x20040000 + 16 KB each
#define TPL_SBA_SURF      89                                       // STATE_BASE_ADDRESS (2nd): surface base
#define TPL_SBA_DYN       91                                       //                          dynamic base
#define TPL_CLR_VB0       119                                      // clear template: vertex buffer addresses
#define TPL_CLR_VB1       123
#define RELOC_CLEAR(c) do { (c)[TPL_SBA_SURF] += delta; (c)[TPL_SBA_DYN] += delta; (c)[TPL_CLR_VB0] += delta; (c)[TPL_CLR_VB1] += delta; } while (0)
static bool mtlBatchTrimOK();                 // v0.56.8 (below)
static bool sBatchTrim = true;                // v0.56.8: boot-arg -irisxebatchfull: re-send everything (v0.56.7)
#define RELOC_DRAW(t)  do { (t)[TPL_SBA_SURF] += delta; (t)[TPL_SBA_DYN] += delta; } while (0)

#define LOG(fmt, ...) IOLog("IrisXeGPU: " fmt "\n", ##__VA_ARGS__)
#define RD(off)     (*(volatile UInt32 *)(gpuMMIO + (off)))
#define WR(off, v)  (*(volatile UInt32 *)(gpuMMIO + (off)) = (v))

// engine
#define BCS                 0x22000
#define R_HWS_PGA           0x80
#define R_HWSTAM            0x98
#define R_MI_MODE           0x9C
#define R_MAX_IDLE          0x54
#define R_MODE_GEN7         0x29C
#define R_HEAD              0x34
#define R_TAIL              0x30
#define R_ESR               0xB8
#define R_EL_SQ             0x510
#define R_EL_CONTROL        0x550
#define MASKED_ON(b)        (((b) << 16) | (b))
#define MASKED_OFF(b)       ((b) << 16)

// commands
#define MI_INSTR(op, f)     (((op) << 23) | (f))
#define MI_NOOP             0
#define MI_LRI(n)           MI_INSTR(0x22, 2 * (n) - 1)
#define MI_LRI_POSTED       (1u << 12)
#define MI_LRI_CS_MMIO      (1u << 19)
#define MI_BB_END           MI_INSTR(0x0A, 0)
#define MI_SDI(len)         MI_INSTR(0x20, (len))
#define MI_USE_GGTT         (1u << 22)
#define MI_FLUSH_DW_4       (MI_INSTR(0x26, 1) + 1)
#define MI_USER_INTERRUPT   MI_INSTR(0x02, 0)
#define XY_COLOR_BLT_CMD    ((2u << 29) | (0x50u << 22))
#define BLT_WRITE_RGBA      ((1u << 20) | (2u << 20))
#define BLT_DEPTH_32        (3u << 24)
#define BLT_ROP_COLOR_COPY  (0xF0u << 16)

// context image (i915 gen12_xcs_offsets)
static const UInt16 kLRI1[] = { 0x244, 0x034, 0x030, 0x038, 0x03c, 0x168, 0x140,
                                0x110, 0x1c0, 0x1c4, 0x1c8, 0x180, 0x2b4 };
static const UInt16 kLRI2[] = { 0x3a8, 0x28c, 0x288, 0x284, 0x280, 0x27c, 0x278, 0x274, 0x270 };
#define CTX_CONTEXT_CONTROL 0x03
#define CTX_RING_HEAD       0x05
#define CTX_RING_TAIL       0x07
#define CTX_RING_START      0x09
#define CTX_RING_CTL        0x0B
#define CTX_PDP0_UDW        0x31
#define CTX_PDP0_LDW        0x33
#define CTX_INHIBIT_SYN     (1u << 3)
#define CTX_RESTORE_INHIBIT (1u << 0)
#define GFX_OP_PIPE_CONTROL(len) ((0x3u << 29) | (0x3u << 27) | (0x2u << 24) | ((len) - 2))
#define PC_GLOBAL_GTT       (1u << 24)
#define PC_CS_STALL         (1u << 20)
#define PC_QW_WRITE         (1u << 14)
#define PC_MAGIC            0x5EC0DE00u

// i915 gen12_rcs_offsets, encoded the same way as the table there
#define NOP(x)  (0x80 | (x))
#define LRI(c, f) (((f) << 6) | (c))
#define REG(x)  ((x) >> 2)
#define REG16(x) (((x) >> 9) | 0x80), (((x) >> 2) & 0x7f)
static const UInt8 kRcsOffsets[] = {
    NOP(1), LRI(13, 1),
    REG16(0x244), REG(0x034), REG(0x030), REG(0x038), REG(0x03c), REG(0x168), REG(0x140),
    REG(0x110), REG(0x1c0), REG(0x1c4), REG(0x1c8), REG(0x180), REG16(0x2b4),
    NOP(5), LRI(9, 1),
    REG16(0x3a8), REG16(0x28c), REG16(0x288), REG16(0x284), REG16(0x280), REG16(0x27c),
    REG16(0x278), REG16(0x274), REG16(0x270),
    LRI(3, 1), REG(0x1b0), REG16(0x5a8), REG16(0x5ac),
    NOP(6), LRI(1, 0), REG(0x0c8), NOP(3 + 9 + 1),
    LRI(51, 1),
    REG16(0x588), REG16(0x588), REG16(0x588), REG16(0x588), REG16(0x588), REG16(0x588),
    REG(0x028), REG(0x09c), REG(0x0c0), REG(0x178), REG(0x17c), REG16(0x358), REG(0x170),
    REG(0x150), REG(0x154), REG(0x158), REG16(0x41c),
    REG16(0x600), REG16(0x604), REG16(0x608), REG16(0x60c), REG16(0x610), REG16(0x614),
    REG16(0x618), REG16(0x61c), REG16(0x620), REG16(0x624), REG16(0x628), REG16(0x62c),
    REG16(0x630), REG16(0x634), REG16(0x638), REG16(0x63c), REG16(0x640), REG16(0x644),
    REG16(0x648), REG16(0x64c), REG16(0x650), REG16(0x654), REG16(0x658), REG16(0x65c),
    REG16(0x660), REG16(0x664), REG16(0x668), REG16(0x66c), REG16(0x670), REG16(0x674),
    REG16(0x678), REG16(0x67c), REG(0x068), REG(0x084), NOP(1),
    0
};
#define CTX_MI_MODE_RCS     0x60   // i915 lrc_ring_mi_mode() for gen12

// i915 set_offsets(): write LRI headers and register addresses; returns the
// dword index right after the last block (where MI_BATCH_BUFFER_END goes)
static UInt32 setOffsets(volatile UInt32 *regs, const UInt8 *data, UInt32 base) {
    UInt32 i = 0;
    while (*data) {
        if (*data & 0x80) { i += *data++ & 0x7F; continue; }
        UInt32 count = *data & 0x3F, flags = *data >> 6;
        data++;
        regs[i] = MI_LRI(count) | MI_LRI_CS_MMIO | ((flags & 1) ? MI_LRI_POSTED : 0);
        i++;
        do {
            UInt32 off = 0; UInt8 v;
            do { v = *data++; off = (off << 7) | (v & 0x7F); } while (v & 0x80);
            regs[i] = base + (off << 2);
            i += 2;
        } while (--count);
    }
    return i;
}

// memory layout (pages)
#define G_BYTES             (128 * 1024)
#define G_PAGES             (G_BYTES / 4096)
#define G_HWSP              0
#define G_LRC               1      // 4 pages
#define G_PML4              5
#define G_PDP               6
#define G_PD                7
#define G_PT                8      // 8 pages -> 16 MB
#define G_RING              16     // 16 pages
#define G_RING_BYTES        (16 * 4096)
#define G_GGTT_ADDR         0xF0600000u
// render engine block (own allocation)
#define R_BYTES             (192 * 1024)   // v0.68.38: + 16 pages for the bigger render ring (was 128 KB)
#define R_PAGES             (R_BYTES / 4096)
#define R_HWSP              0
#define R_LRC               1      // 1 ppHWSP + 14 state pages + spare
// v0.68.38: the render ring is 64 KB (16 pages at 32..47; was 16 KB at 24..27). A job may use at most half the
// ring (v0.68.34), and with 16 KB that was ~2 draws per job in Maps (the first draw of a job carries the whole
// pipeline setup): 47 GPU jobs per command buffer, each ~130 us of GPU setup + flushes. Now a job is only limited
// by the draw buffer (sDrawBuf, 8192 dwords) and the state slots (6 draws).
#define R_RING              32     // 16 pages
#define R_RING_BYTES        (16 * 4096)
#define R_GGTT_ADDR         0xF0700000u
#define RCS                 0x2000
#define RCS0_RSVD_MASK      0x190090
// compute test (step 6): GPU address 0x20000000 -> render block pages
#define R_CPT_PT            20     // page table for GPU VA 0x20000000
#define R_CPT_KERNEL        21     // shader at +0, interface descriptor at +0x800
#define R_CPT_DATA          22     // shader output (GPU VA 0x20001000)
#define R_CPT_STATE         23     // v0.30.0: binding table, surface states, sampler states (GPU VA 0x20002000)
#define CPT_GPU_VA          0x20000000ULL
// milestone 5a: first 3D draw, mapped right after the compute pages
#define R_RND_PS            17     // pixel shader          (GPU VA 0x20003000)
#define R_RND_DYN           18     // blend / CC / viewport (GPU VA 0x20004000)
#define R_RND_SURF          19     // BT, RT surface, vertices (GPU VA 0x20005000)
#define R_RND_RT            28     // 4 pages: 64x64 RGBA8 render target (GPU VA 0x20006000)
#define DRW_PAGES           4      // milestone 5c: draw state (own allocation, GPU VA 0x20010000)
static UInt32 sDrawBuf[8192];                  // v0.68.38: 8192 (was 4096) - bigger jobs, fewer per frame
static UInt32 sAsyncResets;                                          // v0.63.0: gpuResets when the async job started      // the last draw job (mtlJobLock held); v0.42.4: also dumped on a hang
// v0.43.1: Gen12 / Tiger Lake hardware workarounds, as Linux i915 sets them (gt/intel_workarounds.c:
// gen12_ctx_workarounds_init, rcs_engine_wa_init, general_render_compute_wa_init, gen12_gt_workarounds_init).
// We never had any. Written by MMIO at start-up, and the render ones again at the start of every draw job
// (MI_LOAD_REGISTER_IMM), because our draw jobs don't restore a saved context (v0.40.9).
#define MASKBIT(b)  ((1u << ((b) + 16)) | (1u << (b)))
static const UInt32 kWaRegs[][2] = {
    { 0x7304, MASKBIT(9) },                          // COMMON_SLICE_CHICKEN3: DISABLE_CPS_AWARE_COLOR_PIPE (Wa_1409142259 ...)
    { 0x2580, (0x6u << 16) | 0x2u },                 // CS_CHICKEN1: GPGPU preemption = thread group (WaDisableGPGPUMidThreadPreemption)
    { 0x6604, (4u << 16) | (224u << 24) },           // FF_MODE2: TDS timer 128, GS timer 224 (Wa_16011163337, Wa_1604555607)
    { 0x7018, MASKBIT(13) },                         // HIZ_CHICKEN: HZ_DEPTH_TEST_LE_GE_OPT_DISABLE (Wa_1806527549)
    { 0x7300, MASKBIT(6) },                          // COMMON_SLICE_CHICKEN4: DISABLE_TDC_LOAD_BALANCING_CALC (Wa_1606376872)
    { 0x20ec, MASKBIT(1) },                          // CS_DEBUG_MODE1: FF_DOP_CLOCK_GATE_DISABLE (Wa_1606700617, Wa_22010271021)
    { 0xe4f4, MASKBIT(14) | MASKBIT(8) },            // ROW_CHICKEN2: DISABLE_EARLY_READ (Wa_1606931601), PUSH_CONST_DEREF_HOLD_DIS (Wa_1409804808)
    { 0xe18c, MASKBIT(15) | MASKBIT(0) },            // SAMPLER_MODE: ENABLE_SMALLPL (Wa_1406941453), INDIRECT_STATE_BASE_ADDR_OVERRIDE
    { 0xe48c, MASKBIT(9) },                          // ROW_CHICKEN4: DISABLE_TDL_PUSH (Wa_14010229206)
    { 0x2050, MASKBIT(7) | MASKBIT(12) },            // RCS PSMI_CTL: no power down at wait-for-event / semaphore idle (Wa_1607297627)
    { 0x20e0, MASKBIT(14) },                         // FF_SLICE_CS_CHICKEN1: FFSC_PERCTX_PREEMPT_CTRL
};
static UInt32 sFFThreadMode;                         // GEN7_FF_THREAD_MODE (0x20a0) | FF_TESSELATION_DOP_GATE_DISABLE (Wa_1407928979)
static bool sLastJobIsDraw;
#define GGTT_BASE           0x800000
#define GFX_FLSH_CNTL       0x101008
#define FB_GPU_VA           0x30000000ULL        // v0.49.2: was 0x10000000, now above the Metal range
#define MTL_PT_PAGES        256          // Metal buffers: GPU VA 0 .. 512 MB (v0.49.2: was 256 MB; Maps uses 0x1040d000+)
static_assert(MTL_PT_PAGES <= ((0x20000000ULL >> 21) & 511), "Metal range below the draw / compute pages at 0x20000000");
static_assert(((FB_GPU_VA >> 21) & 511) >= MTL_PT_PAGES && (FB_GPU_VA >> 30) == 0, "framebuffer pages outside the Metal range");
#define HWSP_SEQ            0x40

// interrupts (Linux xe regs/xe_irq_regs.h)
#define GFX_MSTR_IRQ        0x190010
#define MASTER_IRQ          (1u << 31)
#define GU_MISC_IRQ         (1u << 29)
#define DISPLAY_IRQ         (1u << 16)
#define GT_INTR_DW(b)       (0x190018 + (b) * 4)
#define INTR_IDENTITY(b)    (0x190060 + (b) * 4)
#define IIR_SELECTOR(b)     (0x190070 + (b) * 4)
#define RENDER_COPY_ENABLE  0x190030
#define BCS_RSVD_MASK       0x1900A0
#define DISPLAY_INT_CTL     0x44200
#define GU_MISC_IIR         0x444F8

// RC6 (i915 gt/intel_gt_regs.h)
#define RC_CONTROL          0xA090
#define RC6_WAKE_RATE_LIMIT 0xA09C
#define MEDIA_WAKE_RATE     0xA0A0
#define RC_EVAL_INTERVAL    0xA0A8
#define RC_IDLE_HYST        0xA0AC
#define RC_SLEEP            0xA0B0
#define RC6_THRESHOLD       0xA0B8
#define MEDIA_PG_IDLE_HYST  0xA0C4
#define RENDER_PG_IDLE_HYST 0xA0C8
#define PG_ENABLE           0xA210
#define GUC_MAX_IDLE_COUNT  0xC3E4
#define RC6_RESIDENCY       0x138108

#define HEARTBEAT_MS        10000
#define POLL_MS             50
#define JOB_TIMEOUT_MS      1000
#define DEMO_JOBS           12

static const UInt32 kFWReq[] = { 0xA278, 0xA188 }, kFWAck[] = { 0x0D84, 0x130044 };

// ---------------------------------------------------------------------------

bool IrisXeProbe::gpuForcewake(bool on) {
    for (UInt32 k = 0; k < 2; k++) WR(kFWReq[k], on ? MASKED_ON(1u) : MASKED_OFF(1u));
    if (!on) return true;
    for (UInt32 k = 0; k < 2; k++) {
        UInt32 t = 0;
        while (!(RD(kFWAck[k]) & 1) && t < 50000) { IODelay(10); t += 10; }
        if (!(RD(kFWAck[k]) & 1)) return false;
    }
    return true;
}

bool IrisXeProbe::gpuStart(IOPCIDevice *pci) {
    gpuMap = pci->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
    if (!gpuMap) { gpuStatus("no BAR0"); return false; }
    gpuMMIO = (volatile UInt8 *)gpuMap->getVirtualAddress();

    UInt32 planeCtl = RD(TGL_PLANE_CTL_1_A);
    fbSurf   = RD(TGL_PLANE_SURF_1_A) & ~0xFFFu;
    fbStride = (RD(TGL_PLANE_STRIDE_1_A) & 0x7FF) * 64;
    fbHeight = (RD(TGL_PIPESRC_A) & 0xFFFF) + 1;
    fbOK = (planeCtl & TGL_PLANE_CTL_ENABLE) && !(planeCtl & TGL_PLANE_CTL_TILING_MASK) && fbStride;

    gpuMem = IOBufferMemoryDescriptor::withOptions(
        kIODirectionInOut | kIOMemoryPhysicallyContiguous | kIOMapInhibitCache, G_BYTES, 4096);
    if (!gpuMem) { gpuStatus("no memory"); return false; }
    gpuVA = (volatile UInt32 *)gpuMem->getBytesNoCopy();
    IOByteCount seg = 0;
    gpuPhys = gpuMem->getPhysicalSegment(0, &seg, kIOMemoryMapperNone);
    if (!gpuVA || !gpuPhys || seg < G_BYTES) { gpuStatus("memory not contiguous"); return false; }
    for (UInt32 i = 0; i < G_BYTES / 4; i++) gpuVA[i] = 0;

    // v0.57.9 (Step C): the render ring + context and the draw state pages are written by the CPU for every draw
    // (~450 command dwords + state): write-combining memory (writes go out in bursts; the mfence before every submit
    // drains them) instead of uncached (every 4-byte write a separate trip to RAM). Boot-arg -irisxeuc: uncached.
    const IOOptionBits drawCache = PE_parse_boot_argn("-irisxeuc", nullptr, 0) ? kIOMapInhibitCache : kIOMapWriteCombineCache;
    gpuDrawWC = drawCache == kIOMapWriteCombineCache;
    rcsMem = IOBufferMemoryDescriptor::withOptions(
        kIODirectionInOut | kIOMemoryPhysicallyContiguous | drawCache, R_BYTES, 4096);
    if (rcsMem) {
        rcsVA = (volatile UInt32 *)rcsMem->getBytesNoCopy();
        seg = 0;
        rcsPhys = rcsMem->getPhysicalSegment(0, &seg, kIOMemoryMapperNone);
        if (!rcsVA || !rcsPhys || seg < R_BYTES) { rcsVA = nullptr; OSSafeReleaseNULL(rcsMem); }
        else for (UInt32 i = 0; i < R_BYTES / 4; i++) rcsVA[i] = 0;
    }
    // milestone 5c: draw state pages (GPU VA DRW_VA = 0x20010000, IrisXeRender.inc)
    drwMem = IOBufferMemoryDescriptor::withOptions(
        kIODirectionInOut | kIOMemoryPhysicallyContiguous | drawCache, DRW_PAGES * 4096, 4096);
    if (drwMem) {
        drwVA = (volatile UInt32 *)drwMem->getBytesNoCopy();
        seg = 0;
        drwPhys = drwMem->getPhysicalSegment(0, &seg, kIOMemoryMapperNone);
        if (!drwVA || !drwPhys || seg < DRW_PAGES * 4096) { drwVA = nullptr; OSSafeReleaseNULL(drwMem); }
        else for (UInt32 i = 0; i < DRW_PAGES * 1024; i++) drwVA[i] = 0;
    }
    // v0.49.0: batching: a copy of the draw state pages per draw of a job (GPU VA SLOT_VA(i))
    for (UInt32 k = 0; k < XE_BATCH_SLOTS && drwVA; k++) {
        slotMem[k] = IOBufferMemoryDescriptor::withOptions(
            kIODirectionInOut | kIOMemoryPhysicallyContiguous | drawCache, DRW_PAGES * 4096, 4096);
        if (!slotMem[k]) break;
        slotVA[k] = (volatile UInt32 *)slotMem[k]->getBytesNoCopy();
        seg = 0;
        slotPhys[k] = slotMem[k]->getPhysicalSegment(0, &seg, kIOMemoryMapperNone);
        if (!slotVA[k] || !slotPhys[k] || seg < DRW_PAGES * 4096) { slotVA[k] = nullptr; OSSafeReleaseNULL(slotMem[k]); break; }
        for (UInt32 i = 0; i < DRW_PAGES * 1024; i++) slotVA[k][i] = 0;
    }
    mtlBatchOK = drwVA && slotVA[XE_BATCH_SLOTS - 1] && mtlBatchTemplatesOK() && !PE_parse_boot_argn("-irisxenobatch", nullptr, 0);
    if (drwVA && !PE_parse_boot_argn("-irisxenoshadow", nullptr, 0))      // v0.60.1
        for (UInt32 k = 0; k <= XE_BATCH_SLOTS; k++) mtlShadow[k] = (UInt32 *)IOMallocAligned(DRW_PAGES * 4096, 64);
    sBatchTrim = mtlBatchTrimOK() && !PE_parse_boot_argn("-irisxebatchfull", nullptr, 0);   // v0.56.8
    rcsOn = rcsVA && !PE_parse_boot_argn("-irisxenorcs", nullptr, 0);
    cptWanted = rcsOn && PE_parse_boot_argn("-irisxecompute", nullptr, 0);

    // Metal buffer page tables (milestone 2c): 128 PTs = GPU VA 0 .. 256 MB
    mtlPTMem = IOBufferMemoryDescriptor::withOptions(
        kIODirectionInOut | kIOMemoryPhysicallyContiguous | kIOMapInhibitCache, MTL_PT_PAGES * 4096, 4096);
    if (mtlPTMem) {
        mtlPT = (volatile UInt32 *)mtlPTMem->getBytesNoCopy();
        seg = 0;
        mtlPTPhys = mtlPTMem->getPhysicalSegment(0, &seg, kIOMemoryMapperNone);
        if (!mtlPT || !mtlPTPhys || seg < MTL_PT_PAGES * 4096) { mtlPT = nullptr; OSSafeReleaseNULL(mtlPTMem); }
        else for (UInt32 i = 0; i < MTL_PT_PAGES * 1024; i++) mtlPT[i] = 0;
    }
    mtlLock = IOLockAlloc();
    mtlJobLock = IOLockAlloc();
    gpuDoneLock = IOLockAlloc();                           // v0.45.0

    gpuBuildTables();

    gpuWL = IOWorkLoop::workLoop();
    if (!gpuWL) { gpuStatus("no workloop"); return false; }
    gpuTimer = IOTimerEventSource::timerEventSource(this, &IrisXeProbe::gpuTimerFired);
    if (!gpuTimer || gpuWL->addEventSource(gpuTimer) != kIOReturnSuccess) { gpuStatus("no timer"); return false; }

    // interrupt: prefer MSI
    int idx = -1, type = 0;
    for (int i = 0; pci->getInterruptType(i, &type) == kIOReturnSuccess; i++) {
        if (type & kIOInterruptTypePCIMessaged) { idx = i; gpuIrqMSI = true; break; }
        if (idx < 0) idx = i;
    }
    if (idx >= 0) {
        gpuIRQ = IOFilterInterruptEventSource::filterInterruptEventSource(
            this, &IrisXeProbe::gpuIrqHandler, &IrisXeProbe::gpuIrqFilter, pci, idx);
        if (gpuIRQ && gpuWL->addEventSource(gpuIRQ) == kIOReturnSuccess) gpuIRQ->enable();
        else OSSafeReleaseNULL(gpuIRQ);
    }
    gpuIrqIndex = idx;

    // power management: re-initialise after system sleep
    static IOPMPowerState states[2] = {
        { kIOPMPowerStateVersion1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
        { kIOPMPowerStateVersion1, kIOPMPowerOn | kIOPMDeviceUsable, kIOPMPowerOn, kIOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0 },
    };
    PMinit();
    pci->joinPMtree(this);
    registerPowerDriver(this, states, 2);

    gpuWL->runAction(OSMemberFunctionCast(IOWorkLoop::Action, this, &IrisXeProbe::gpuBringUpGated), this);
    return true;
}

// GGTT entries + GPU page tables (framebuffer at FB_GPU_VA) + context image
void IrisXeProbe::gpuBuildTables() {
    auto pa  = [&](UInt32 pg) { return gpuPhys + (UInt64)pg * 4096; };
    auto pgv = [&](UInt32 pg) { return gpuVA + pg * 1024; };
    auto p64 = [&](volatile UInt32 *t, UInt32 i, UInt64 v) { t[i * 2] = (UInt32)v; t[i * 2 + 1] = (UInt32)(v >> 32); };

    p64(pgv(G_PML4), 0, pa(G_PDP) | 3);
    p64(pgv(G_PDP), (UInt32)((FB_GPU_VA >> 30) & 511), pa(G_PD) | 3);
    for (UInt32 h = 1; h <= MTL_HI_PDS; h++) p64(pgv(G_PDP), h, 0);   // v0.58.7: 1..4 GB = the current space's PDs
    UInt32 pd0 = (UInt32)((FB_GPU_VA >> 21) & 511);
    for (UInt32 t = 0; t < 8; t++) p64(pgv(G_PD), pd0 + t, pa(G_PT + t) | 3);
    if (fbOK) {
        UInt32 pages = (fbStride * fbHeight + 4095) / 4096;
        if (pages > 4096) pages = 4096;
        UInt32 g = fbSurf >> 12;
        for (UInt32 p = 0; p < pages; p++) {
            UInt64 e = ((UInt64)RD(GGTT_BASE + (g + p) * 8 + 4) << 32) | RD(GGTT_BASE + (g + p) * 8);
            p64(pgv(G_PT + p / 512), p % 512, (e & 0x0000007FFFFFF000ULL) | (e & 0x98) | 3);
        }
    }

    if (mtlPT)                       // Metal buffers: PD entries 0..127 (GPU VA 0 .. 256 MB)
        for (UInt32 t = 0; t < MTL_PT_PAGES; t++) p64(pgv(G_PD), t, (mtlPTPhys + (UInt64)t * 4096) | 3);
    mtlCur = 0;                      // (also after wake: the PD points at the shared space again)

    volatile UInt32 *regs = pgv(G_LRC + 1);
    for (UInt32 i = 0; i < 1024; i++) regs[i] = 0;
    UInt32 i = 1;
    regs[i++] = MI_LRI(13) | MI_LRI_POSTED | MI_LRI_CS_MMIO;
    for (UInt16 off : kLRI1) { regs[i] = BCS + off; i += 2; }
    i += 5;
    regs[i++] = MI_LRI(9) | MI_LRI_POSTED | MI_LRI_CS_MMIO;
    for (UInt16 off : kLRI2) { regs[i] = BCS + off; i += 2; }
    regs[i] = MI_BB_END | 1;
    regs[CTX_CONTEXT_CONTROL] = MASKED_ON(CTX_INHIBIT_SYN) | MASKED_OFF(CTX_RESTORE_INHIBIT) | CTX_RESTORE_INHIBIT;
    regs[CTX_RING_START] = G_GGTT_ADDR + G_RING * 4096;
    regs[CTX_RING_CTL]   = ((G_RING_BYTES - 4096) & 0x1FF000) | 1;
    regs[CTX_PDP0_UDW]   = (UInt32)(pa(G_PML4) >> 32);
    regs[CTX_PDP0_LDW]   = (UInt32)pa(G_PML4);
    gpuRingTail = 0;
    gpuCtxFresh = true;

    if (rcsOn) {                      // render engine context (gen12 render layout)
        volatile UInt32 *r = rcsVA + (R_LRC + 1) * 1024;
        for (UInt32 k = 0; k < 16 * 1024; k++) r[k] = 0;
        UInt32 end = setOffsets(r, kRcsOffsets, RCS);
        r[end] = MI_BB_END | 1;
        r[CTX_CONTEXT_CONTROL] = MASKED_ON(CTX_INHIBIT_SYN) | MASKED_OFF(CTX_RESTORE_INHIBIT) | CTX_RESTORE_INHIBIT;
        r[CTX_RING_START] = R_GGTT_ADDR + R_RING * 4096;
        r[CTX_RING_CTL]   = ((R_RING_BYTES - 4096) & 0x1FF000) | 1;
        r[CTX_PDP0_UDW]   = (UInt32)(pa(G_PML4) >> 32);       // same GPU address space
        r[CTX_PDP0_LDW]   = (UInt32)pa(G_PML4);
        r[CTX_MI_MODE_RCS + 1] = MASKED_OFF(1u << 8);         // STOP_RING off
        rcsLayoutEnd = end;

        // compute test: map shader + output pages at GPU VA 0x20000000
        auto rpa = [&](UInt32 pg) { return rcsPhys + (UInt64)pg * 4096; };
        volatile UInt32 *pt = rcsVA + R_CPT_PT * 1024;
        for (UInt32 k = 0; k < 1024; k++) pt[k] = 0;
        p64(pt, 0, rpa(R_CPT_KERNEL) | 3);
        p64(pt, 1, rpa(R_CPT_DATA) | 3);
        p64(pt, 2, rpa(R_CPT_STATE) | 3);
        // milestone 5a: draw test pages (IrisXeRender.inc)
        static_assert(RENDER_PS_VA == CPT_GPU_VA + 0x3000 && RENDER_DYN_VA == CPT_GPU_VA + 0x4000 &&
                      RENDER_SURF_VA == CPT_GPU_VA + 0x5000 && RENDER_RT_VA == CPT_GPU_VA + 0x6000, "render layout");
        static_assert(RENDER_RT_PITCH * RENDER_RT_H <= 4 * 4096, "render target size");
        static_assert(R_RND_RT + 4 <= R_RING && R_RING + R_RING_BYTES / 4096 <= R_PAGES, "render pages");
        static_assert(R_GGTT_ADDR + R_BYTES <= 0xF0800000u, "render block below the video block (V_GGTT_ADDR)");
        p64(pt, 3, rpa(R_RND_PS) | 3);
        p64(pt, 4, rpa(R_RND_DYN) | 3);
        p64(pt, 5, rpa(R_RND_SURF) | 3);
        for (UInt32 k = 0; k < 4; k++) p64(pt, 6 + k, rpa(R_RND_RT + k) | 3);
        // milestone 5c: draw state pages
        static_assert(DRW_VA == CPT_GPU_VA + 0x10000, "draw pages");
        if (drwVA) {
            for (UInt32 k = 0; k < DRW_PAGES; k++) p64(pt, 16 + k, (drwPhys + (UInt64)k * 4096) | 3);
            for (UInt32 k = 0; k < sizeof(kClearPS) / 4; k++) drwVA[k] = kClearPS[k];
            volatile UInt32 *dy = drwVA + DRW_DYN_OFF / 4;
            for (UInt32 k = 0; k < sizeof(kDrawBlend) / 4; k++) dy[DYN_BLEND / 4 + k] = dy[DYN_BLENDCLR / 4 + k] = kDrawBlend[k];
            for (UInt32 k = 0; k < sizeof(kDrawCCVP) / 4; k++) dy[DYN_CCVP / 4 + k] = kDrawCCVP[k];
        }
        static_assert(SLOT_VA(XE_BATCH_SLOTS) <= CPT_GPU_VA + 0x200000, "batch slots in the 0x20000000 page table");
        for (UInt32 i = 0; i < XE_BATCH_SLOTS; i++) {          // v0.49.0: batch slots, laid out as the draw pages
            if (!slotVA[i]) continue;
            UInt32 pte = (SLOT_VA(i) - (UInt32)CPT_GPU_VA) >> 12;
            for (UInt32 k = 0; k < DRW_PAGES; k++) p64(pt, pte + k, (slotPhys[i] + (UInt64)k * 4096) | 3);
            volatile UInt32 *dy = slotVA[i] + DRW_DYN_OFF / 4;
            for (UInt32 k = 0; k < sizeof(kDrawBlend) / 4; k++) dy[DYN_BLEND / 4 + k] = dy[DYN_BLENDCLR / 4 + k] = kDrawBlend[k];
            for (UInt32 k = 0; k < sizeof(kDrawCCVP) / 4; k++) dy[DYN_CCVP / 4 + k] = kDrawCCVP[k];
        }
        {
            volatile UInt32 *q = rcsVA + R_RND_PS * 1024;
            for (UInt32 k = 0; k < 3 * 1024; k++) q[k] = 0;            // pages 17..19
            for (UInt32 k = 0; k < sizeof(kRenderShaders) / 4; k++) q[k] = kRenderShaders[k];   // PS A, VS B, PS B
            q = rcsVA + R_RND_DYN * 1024;
            for (UInt32 k = 0; k < sizeof(kRenderDyn) / 4; k++) q[k] = kRenderDyn[k];
            for (UInt32 k = 0; k < sizeof(kTestPS8) / 4; k++) q[0x800 / 4 + k] = kTestPS8[k];      // v0.63.2
            for (UInt32 k = 0; k < sizeof(kTestPS16) / 4; k++) q[0xA00 / 4 + k] = kTestPS16[k];
            q = rcsVA + R_RND_SURF * 1024;
            for (UInt32 k = 0; k < sizeof(kRenderSurf) / 4; k++) q[k] = kRenderSurf[k];
        }
        p64(pgv(G_PD), (UInt32)((CPT_GPU_VA >> 21) & 511), rpa(R_CPT_PT) | 3);
        volatile UInt32 *kp = rcsVA + R_CPT_KERNEL * 1024;
        for (UInt32 k = 0; k < 1024; k++) kp[k] = 0;
        for (UInt32 k = 0; k < sizeof(kComputeShader) / 4; k++) kp[k] = kComputeShader[k];
        for (UInt32 k = 0; k < 8; k++) kp[0x800 / 4 + k] = kComputeIDD[k];
        volatile UInt32 *dp = rcsVA + R_CPT_DATA * 1024;
        for (UInt32 k = 0; k < 1024; k++) dp[k] = 0xABABABAB;
        rcsRingTail = 0;
        rcsCtxFresh = true;
    }
}

void IrisXeProbe::gpuWriteGGTT() {
    UInt32 idx = G_GGTT_ADDR >> 12;
    for (UInt32 i = 0; i < G_PAGES; i++) {
        UInt64 pte = (gpuPhys + (UInt64)i * 4096) | 1;
        WR(GGTT_BASE + (idx + i) * 8,     (UInt32)pte);
        WR(GGTT_BASE + (idx + i) * 8 + 4, (UInt32)(pte >> 32));
    }
    if (rcsOn) {
        UInt32 ridx = R_GGTT_ADDR >> 12;
        for (UInt32 i = 0; i < R_PAGES; i++) {
            UInt64 pte = (rcsPhys + (UInt64)i * 4096) | 1;
            WR(GGTT_BASE + (ridx + i) * 8,     (UInt32)pte);
            WR(GGTT_BASE + (ridx + i) * 8 + 4, (UInt32)(pte >> 32));
        }
    }
    (void)RD(GGTT_BASE + idx * 8);
    WR(GFX_FLSH_CNTL, 1);
    (void)RD(GFX_FLSH_CNTL);
}

// (re)initialise the hardware: GGTT, engine, interrupts, RC6. On workloop.
IOReturn IrisXeProbe::gpuBringUpGated() {
    gpuWriteGGTT();
    scanGen++;                                             // v0.59.0: scanout slots must be written again
    if (!gpuForcewake(true)) { gpuForcewake(false); gpuStatus("forcewake failed"); return kIOReturnError; }

    // engine into execlist mode, status page
    WR(BCS + R_HWSTAM, 0xFFFFFFFF);
    WR(BCS + R_MODE_GEN7, MASKED_ON(1u << 3));          // GEN11_GFX_DISABLE_LEGACY_MODE
    WR(BCS + R_MI_MODE, MASKED_OFF(1u << 8));           // clear STOP_RING
    WR(BCS + R_HWS_PGA, G_GGTT_ADDR + G_HWSP * 4096);
    (void)RD(BCS + R_HWS_PGA);
    if (rcsOn) {
        WR(RCS + R_HWSTAM, 0xFFFFFFFF);
        WR(RCS + R_MODE_GEN7, MASKED_ON(1u << 3));
        WR(RCS + R_MI_MODE, MASKED_OFF(1u << 8));
        WR(RCS + R_HWS_PGA, R_GGTT_ADDR + R_HWSP * 4096);
        (void)RD(RCS + R_HWS_PGA);
        // v0.43.1: hardware workarounds (i915)
        for (const auto &w : kWaRegs) WR(w[0], w[1]);
        sFFThreadMode = RD(0x20a0) | (1u << 19);
        WR(0x20a0, sFFThreadMode);
        WR(0x9550, RD(0x9550) | (1u << 9));              // DFR_RATIO_EN_AND_CHICKEN: DFR_DISABLE (Wa_14011059788)
        WR(0x9424, RD(0x9424) & ~(1u << 1));             // MISCCPCTL: DOP clock gating render off (Wa_14015795083)
        waApplied++;
        gpuMocsSetup();                                  // v0.49.1
        gpuRingFreqSetup();                              // v0.57.4
        gpuPatSetup();                                   // v0.57.5
        gpuHashSetup();                                  // v0.57.8
    }

    // interrupts: only GT user interrupts; keep display/other sources off
    if (gpuIRQ) {
        gpuDisplayIntCtl = RD(DISPLAY_INT_CTL);
        if (gpuDisplayIntCtl & (1u << 31)) WR(DISPLAY_INT_CTL, 0);
        WR(GT_INTR_DW(0), RD(GT_INTR_DW(0)));               // drop stale bits
        WR(RENDER_COPY_ENABLE, (1u << 16) | 1u);             // RCS + BCS user interrupt
        WR(BCS_RSVD_MASK, ~(1u << 16));                      // unmask BCS0
        if (rcsOn) WR(RCS0_RSVD_MASK, ~(1u << 16));          // unmask RCS0
        if (!gpuIrqStorm) WR(GFX_MSTR_IRQ, MASTER_IRQ);
    }

    // RC6 + power gating (no GuC)
    if (!PE_parse_boot_argn("-irisxenorc6", nullptr, 0)) {
        WR(RC6_WAKE_RATE_LIMIT, 54u << 16 | 85);
        WR(MEDIA_WAKE_RATE, 150);
        WR(RC_EVAL_INTERVAL, 125000);
        WR(RC_IDLE_HYST, 25);
        static const UInt32 engines[] = { 0x2000, 0x22000, 0x1C0000, 0x1C8000, 0x1D0000 };
        for (UInt32 e : engines) WR(e + R_MAX_IDLE, 10);
        WR(GUC_MAX_IDLE_COUNT, 0xA);
        WR(RC_SLEEP, 0);
        WR(RC6_THRESHOLD, 50000);
        WR(MEDIA_PG_IDLE_HYST, 60);
        WR(RENDER_PG_IDLE_HYST, 60);
        UInt32 pg = (1u << 0) | (1u << 1) | (1u << 2) |          // render, media, sampler
                    (1u << 3) | (1u << 4) | (1u << 7) | (1u << 8);   // VCS0 + VCS2 HCP/MFX
        WR(PG_ENABLE, pg);
        WR(RC_CONTROL, (1u << 31) | (1u << 18) | (1u << 27));    // HW enable | RC6 | EI mode 1
        gpuRC6On = true;
    }
    gpuForcewake(false);

    gpuUp = true;
    gpuBusy = false;
    gpuFreqReport();                                       // v0.45.1
    gpuLastRC6 = RD(RC6_RESIDENCY);
    clock_get_uptime(&gpuLastRC6Time);
    // v0.68.7: the boot colour bars (the first GPU test, v0.13) are off by default; boot-arg -irisxedemo shows them
    static bool demoChecked;
    if (!demoChecked) {
        demoChecked = true;
        if (PE_parse_boot_argn("-irisxedemo", nullptr, 0)) gpuDemoLeft = DEMO_JOBS;
    }
    if (gpuDemoLeft) gpuSubmitDemo();
    else gpuTimer->setTimeoutMS(HEARTBEAT_MS);
    gpuStatus(nullptr);
    return kIOReturnSuccess;
}

// ---- v0.45.1: GPU clock (i915 gt/intel_rps.c, gen6_rps_get_freq_caps / intel_rps_read_actual_frequency) ----
// We never set the GPU clock. Units: 16.67 MHz (50 MHz / GEN9_FREQ_SCALER 3).
#define GEN6_RP_STATE_CAP   0x145998       // MCHBAR mirror: RP0 7:0, RP1 15:8, RPn 23:16 (50 MHz units)
#define GEN6_RPNSWREQ       0xA008         // requested frequency, bits 31:23 (gen9+)
#define GEN6_RPSTAT1        0xA01C
#define GEN12_RPSTAT1       0x1381B4       // actual frequency (CAGF), bits 19:11 (gen12)
#define RCS_TIMESTAMP       (RCS + 0x358)
static inline UInt32 freqMHz(UInt32 u) { return (u * 50 + 1) / 3; }
IOReturn IrisXeProbe::gpuFreqSetGated() {
    if (gpuMMIO && gpuForcewake(true)) WR(GEN6_RPNSWREQ, gpuFreqReq << 23);   // 0: lowest, the hardware picks
    gpuForcewake(false);
    IODelay(20000);                                        // 20 ms for the PCU to switch
    gpuFreqReport();
    return kIOReturnSuccess;
}
// v0.49.1: the GPU's cache settings (MOCS table, i915 gt/intel_mocs.c). We never programmed it, and every
// command we send (templates, surface states, STATE_BASE_ADDRESS) uses MOCS index 2: on Tiger Lake an index
// nobody set up means uncached (i915: "for TGL all the unspecified MOCS indexes are mapped to L3 UC", and
// LE_0_PAGETABLE means UC on Gen12). Maps: ~30 million pixels a second, a full-screen clear 198 ms.
// Index 2 here = write-back in L3 and LLC (i915's index 4 / Mesa's choice for buffers and images);
// entries 2..59 the same, 60..63 as i915 (hardware special cases). Entries 0 and 1 are left alone.
// Boot-arg -irisxenomocs: don't touch the table.
#define GEN12_GLOBAL_MOCS(i)  (0x4000 + (i) * 4)
#define GEN9_LNCFCMOCS(i)     (0xB020 + (i) * 4)          // L3 control, 2 entries per register
#define LE_WB_LLC_LRU3        (3u | (1u << 2) | (3u << 4))   // LE_3_WB | LE_TC_1_LLC | LE_LRUM(3)
#define LE_UC_LLC             (1u | (1u << 2))               // LE_1_UC | LE_TC_1_LLC
#define L3_WB                 (3u << 4)
#define L3_UC                 (1u << 4)
void IrisXeProbe::gpuMocsSetup() {
    if (!gpuMOCSBefore[0]) {                               // what the hardware had (first time only)
        gpuMOCSBefore[0] = RD(GEN12_GLOBAL_MOCS(2)) | 0x80000000u;
        gpuMOCSBefore[1] = RD(GEN9_LNCFCMOCS(1));
    }
    if (PE_parse_boot_argn("-irisxenomocs", nullptr, 0)) { gpuMOCSSet = false; return; }
    UInt16 l3[64];
    for (UInt32 i = 0; i < 64; i++) {
        UInt32 ctl = LE_WB_LLC_LRU3; UInt16 l = L3_WB;
        if (i == 60 || i >= 62) l = L3_UC;                  // CCS special case / HW reserved
        if (i == 61) { ctl = LE_UC_LLC; l = L3_WB; }        // displayable
        l3[i] = l;
        if (i >= 2) WR(GEN12_GLOBAL_MOCS(i), ctl);
    }
    for (UInt32 i = 1; i < 32; i++) {                       // register i = entries 2i, 2i+1
        UInt32 v = RD(GEN9_LNCFCMOCS(i));
        UInt32 lo = i == 0 ? (v & 0xFFFF) : l3[2 * i];
        WR(GEN9_LNCFCMOCS(i), lo | ((UInt32)l3[2 * i + 1] << 16));
    }
    gpuMOCSSet = true;
}

// v0.57.4: the ring / LLC clock. The GPU reaches memory through the CPU's ring and last-level cache, and the ring
// runs at the CPU cores' clock unless the driver tells the power controller (PCODE) which ring clock each GPU clock
// needs. i915 does this for every chip with an LLC (intel_llc.c gen6_update_ring_freq; gen9+: ring ratio = GPU ratio,
// no IA floor). We never did: with the CPU idle the ring stays slow, and our measurements showed the GPU moving only
// 4-7 GB/s (full-screen draws 4-9 ms). Boot-arg -irisxenoringfreq: don't write the table.
#define GEN6_PCODE_MAILBOX              0x138124
#define GEN6_PCODE_DATA                 0x138128
#define GEN6_PCODE_DATA1                0x13812C
#define GEN6_PCODE_READY                (1u << 31)
#define GEN6_PCODE_WRITE_MIN_FREQ_TABLE 0x8
#define GEN6_PCODE_READ_MIN_FREQ_TABLE  0x9
static bool pcodeWait(volatile UInt8 *mmio) {
    for (int i = 0; i < 500; i++) {                        // up to ~5 ms
        if (!(*(volatile UInt32 *)(mmio + GEN6_PCODE_MAILBOX) & GEN6_PCODE_READY)) return true;
        IODelay(10);
    }
    return false;
}
void IrisXeProbe::gpuRingFreqSetup() {
    if (PE_parse_boot_argn("-irisxenoringfreq", nullptr, 0)) { snprintf(gpuRingMsg, sizeof(gpuRingMsg), "not set (-irisxenoringfreq)"); return; }
    volatile UInt8 *m = (volatile UInt8 *)gpuMMIO;
    UInt32 caps = RD(GEN6_RP_STATE_CAP);
    UInt32 maxF = caps & 0xFF, minF = (caps >> 16) & 0xFF;  // 50 MHz units (i915: rps max/min_freq / GEN9_FREQ_SCALER)
    if (!m || !maxF || minF > maxF || maxF > 0x40) { snprintf(gpuRingMsg, sizeof(gpuRingMsg), "not set (clock limits 0x%x look wrong)", caps); return; }
    UInt32 ok = 0, fail = 0, before = 0xFFFFFFFF, after = 0xFFFFFFFF;
    // what PCODE had for the top GPU clock (read back)
    if (pcodeWait(m)) {
        WR(GEN6_PCODE_DATA, maxF); WR(GEN6_PCODE_DATA1, 0);
        WR(GEN6_PCODE_MAILBOX, GEN6_PCODE_READY | GEN6_PCODE_READ_MIN_FREQ_TABLE);
        if (pcodeWait(m)) before = RD(GEN6_PCODE_DATA);
    }
    for (UInt32 f = maxF; f >= minF && f; f--) {
        if (!pcodeWait(m)) { fail++; continue; }
        WR(GEN6_PCODE_DATA, (0u << 8) | (f << 16) | f);   // IA ratio 0, ring ratio = GPU ratio, GPU ratio
        WR(GEN6_PCODE_DATA1, 0);
        WR(GEN6_PCODE_MAILBOX, GEN6_PCODE_READY | GEN6_PCODE_WRITE_MIN_FREQ_TABLE);
        if (pcodeWait(m) && !(RD(GEN6_PCODE_MAILBOX) & 0xFF)) ok++; else fail++;
    }
    if (pcodeWait(m)) {
        WR(GEN6_PCODE_DATA, maxF); WR(GEN6_PCODE_DATA1, 0);
        WR(GEN6_PCODE_MAILBOX, GEN6_PCODE_READY | GEN6_PCODE_READ_MIN_FREQ_TABLE);
        if (pcodeWait(m)) after = RD(GEN6_PCODE_DATA);
    }
    snprintf(gpuRingMsg, sizeof(gpuRingMsg), "set: GPU clocks %u-%u MHz, %u entries written, %u failed | entry for the top clock: before 0x%x, after 0x%x",
             minF * 50, maxF * 50, ok, fail, before, after);
}

// v0.57.5: the page-table caching table (PAT). Every GPU page-table entry we write uses PAT index 0, meant as
// write-back. The 8 entries are registers (GEN12_PAT_INDEX 0x4800 + 4 i) that i915 programs on Tiger Lake
// (gt/intel_gtt.c tgl_setup_private_ppat: 0 WB, 1 WC, 2 WT, 3 UC, 4-7 WB); we never did, so index 0 is whatever
// the chip starts with - if that is uncached, every GPU memory access bypasses the caches (our measurements: the
// GPU reads and writes memory at 4-7 GB/s). Boot-arg -irisxenopat: leave the table alone (it is still reported).
#define GEN12_PAT_INDEX(i) (0x4800 + (i) * 4)
void IrisXeProbe::gpuPatSetup() {
    UInt32 before[8];
    for (int i = 0; i < 8; i++) before[i] = RD(GEN12_PAT_INDEX(i));
    bool set = !PE_parse_boot_argn("-irisxenopat", nullptr, 0);
    static const UInt32 kPat[8] = { 3, 1, 2, 0, 3, 3, 3, 3 };   // GEN8_PPAT_WB 3, WC 1, WT 2, UC 0
    if (set) for (int i = 0; i < 8; i++) WR(GEN12_PAT_INDEX(i), kPat[i]);
    UInt32 after0 = RD(GEN12_PAT_INDEX(0));
    snprintf(gpuPatMsg, sizeof(gpuPatMsg), "%s | before: %x %x %x %x %x %x %x %x (0 UC, 1 WC, 2 WT, 3 WB) | index 0 now %x",
             set ? "set: 0 WB, 1 WC, 2 WT, 3 UC, 4-7 WB" : "not set (-irisxenopat)",
             before[0], before[1], before[2], before[3], before[4], before[5], before[6], before[7], after0);
}

// v0.57.8: pixel pipe balancing. Gen12 has 3 pixel pipes of 2 dual subslices (DSS) each. This chip has 5 DSS on,
// so one pipe has only 1: with the hardware's default spreading (every pipe gets the same share of the screen) that
// pipe gets as many pixels as a full one, at half the speed, and the others wait for it. Mesa programs a table that
// gives the short pipe less (genX_init_state.c, GFX_VERx10 == 120; tables from intel_pixel_hash.h). Emitted at the
// start of every draw job. Boot-arg -irisxenohash: not used; property IrisXe-HashCfg (0 off / 1 on) switches it.
static void pixelHash3way(unsigned n, unsigned m, unsigned period, unsigned index, bool flip, UInt32 *p) {
    for (unsigned i = 0; i < n; i++)
        for (unsigned j = 0; j < m; j++) {
            const unsigned k = (i + j) % period;
            p[j + m * i] = (k == index ? 2 : (k & 1) ^ (flip ? 1 : 0));
        }
}
void IrisXeProbe::gpuHashSetup() {
    gpuHashDSS = RD(0x913C);                               // GEN12_GT_GEOMETRY_DSS_ENABLE
    unsigned of[3] = {};
    for (int p = 0; p < 3; p++) { gpuHashPipes[p] = (UInt32)__builtin_popcount((gpuHashDSS >> (2 * p)) & 3); of[gpuHashPipes[p]]++; }
    gpuHashOK = false;
    if (of[2] == 3 || of[0] == 2) { gpuHashPublish(); return; }     // balanced, or one pipe: nothing to do
    UInt32 two[8 * 16] = {}, three[8 * 16] = {};
    bool twoSet = true, threeSet = true;
    if (of[2] == 2 && of[0] == 1) pixelHash3way(8, 16, 2, 2, false, two);
    else if (of[2] == 1 && of[1] == 1 && of[0] == 1) pixelHash3way(8, 16, 3, 3, false, two);
    else twoSet = false;
    if (of[2] == 2 && of[1] == 1) pixelHash3way(8, 16, 5, 4, false, three);
    else if (of[2] == 2 && of[0] == 1) pixelHash3way(8, 16, 2, 2, false, three);
    else if (of[2] == 1 && of[1] == 1 && of[0] == 1) pixelHash3way(8, 16, 3, 3, false, three);
    else threeSet = false;
    if (!threeSet) { gpuHashPublish(); return; }           // Mesa: "Illegal fusing"
    for (int i = 0; i < 14; i++) gpuHashDW[i] = 0;
    gpuHashDW[0] = (3u << 29) | (3u << 27) | (1u << 24) | (0x1Fu << 16) | 12;   // 3DSTATE_SUBSLICE_HASH_TABLE, 14 dwords
    gpuHashDW[1] = 2;                                      // Slice Hash Control[0] = TABLE_0, single table
    for (unsigned b = 0; twoSet && b < 128; b++)           // Two Way Table Entry: bits 64.., 1 bit each
        if (two[b] & 1) gpuHashDW[2 + b / 32] |= 1u << (b % 32);
    for (unsigned b = 0; b < 128; b++)                     // Three Way Table Entry: bits 192.., 2 bits each
        gpuHashDW[6 + (2 * b) / 32] |= (three[b] & 3) << ((2 * b) % 32);
    gpuHashOK = true;
    gpuHashOn = !PE_parse_boot_argn("-irisxenohash", nullptr, 0);
    gpuHashPublish();
}
void IrisXeProbe::gpuHashPublish() {
    char t[200];
    snprintf(t, sizeof(t), "%s | DSS 0x%x: pixel pipes have %u %u %u dual subslices",
             !gpuHashOK ? "not needed (pipes balanced)" : gpuHashOn ? "on: balancing table in every draw job" : "off (table ready)",
             gpuHashDSS, gpuHashPipes[0], gpuHashPipes[1], gpuHashPipes[2]);
    setProperty("IrisXe-Hash", t);
}

void IrisXeProbe::gpuFreqReport() {
    if (!gpuMMIO || !gpuForcewake(true)) { gpuForcewake(false); return; }
    UInt32 caps = RD(GEN6_RP_STATE_CAP);
    UInt32 rp0 = (caps & 0xFF) * 3, rp1 = ((caps >> 8) & 0xFF) * 3, rpn = ((caps >> 16) & 0xFF) * 3;
    gpuFreqRP0 = rp0; gpuFreqRPn = rpn;
    // v0.46.0: run at the highest clock by default (it was 100-200 MHz: 31 ms a draw in Maps, 0.75 ms at 1300 MHz).
    // RC6 still powers the GPU down when it is idle. Boot-arg -irisxenoboost keeps the hardware default.
    static bool boostChecked;
    if (!boostChecked) {
        boostChecked = true;
        if (!PE_parse_boot_argn("-irisxenoboost", nullptr, 0) && rp0) gpuFreqReq = rp0;
    }
    if (gpuFreqReq) WR(GEN6_RPNSWREQ, gpuFreqReq << 23);       // (again after a wake / reset)
    // the CS timestamp clock, measured (ticks per microsecond x 1000)
    UInt32 t0 = RD(RCS_TIMESTAMP);
    uint64_t c0; clock_get_uptime(&c0);
    IODelay(2000);
    UInt32 t1 = RD(RCS_TIMESTAMP);
    uint64_t c1; clock_get_uptime(&c1);
    UInt64 ns; absolutetime_to_nanoseconds(c1 - c0, &ns);
    if (ns) gpuTsPerMs = (UInt32)((UInt64)(t1 - t0) * 1000000ULL / ns);
    UInt32 now12 = (RD(GEN12_RPSTAT1) >> 11) & 0x1FF, now9 = (RD(GEN6_RPSTAT1) >> 23) & 0x1FF;
    UInt32 req = (RD(GEN6_RPNSWREQ) >> 23) & 0x1FF;
    gpuForcewake(false);
    char r[200];
    snprintf(r, sizeof(r), "now %u MHz (other reg %u MHz), requested %u MHz, range %u-%u MHz (RP1 %u), timestamps %u.%03u MHz",
             freqMHz(now12), freqMHz(now9), freqMHz(req), freqMHz(rpn), freqMHz(rp0), freqMHz(rp1),
             gpuTsPerMs / 1000, gpuTsPerMs % 1000);
    setProperty("IrisXe-GPUFreq", r);
    char mo[160];                                           // v0.49.1
    snprintf(mo, sizeof(mo), "%s | index 2 before: control 0x%x, L3 0x%x (entries 2-3)",
             gpuMOCSSet ? "set: L3 + LLC write-back" : "not set (-irisxenomocs)", gpuMOCSBefore[0] & 0x7FFFFFFF, gpuMOCSBefore[1]);
    setProperty("IrisXe-MOCS", mo);
    if (gpuRingMsg[0]) setProperty("IrisXe-RingFreq", gpuRingMsg);   // v0.57.4
    if (gpuPatMsg[0]) setProperty("IrisXe-PAT", gpuPatMsg);           // v0.57.5
}

// append a job to the ring and hand the context to the engine. On workloop.
bool IrisXeProbe::gpuSubmit(const UInt32 *cmds, UInt32 count, bool render) {
    if (render) sLastJobIsDraw = (cmds == sDrawBuf);                 // v0.42.4: hang dump
    {                                                                // v0.43.0: job history
        uint64_t now; clock_get_uptime(&now);
        UInt64 ns; absolutetime_to_nanoseconds(now, &ns);
        JobRec &j = jobHist[jobHistN++ & 31];
        j.usec = (UInt32)(ns / 1000); j.engine = render ? (cmds == sDrawBuf ? 2 : 1) : 0;
        j.space = mtlCur; j.dwords = count; j.first = count ? cmds[0] : 0;
        j.seq = render ? rcsSeqSubmitted + 1 : gpuSeqSubmitted + 1;
    }
    if (!gpuUp || gpuBusy || gpuHung) return false;
    if (render && !rcsOn) return false;
    volatile UInt32 *base = render ? rcsVA : gpuVA;
    UInt32 ggttBase = render ? R_GGTT_ADDR : G_GGTT_ADDR;
    UInt32 ringPg = render ? R_RING : G_RING, lrcPg = render ? R_LRC : G_LRC, hwspPg = render ? R_HWSP : G_HWSP;
    UInt32 &tailRef = render ? rcsRingTail : gpuRingTail;
    bool &freshRef = render ? rcsCtxFresh : gpuCtxFresh;
    UInt32 &seqRef = render ? rcsSeqSubmitted : gpuSeqSubmitted;
    UInt32 engBase = render ? RCS : BCS;
    volatile UInt32 *ring = base + ringPg * 1024;
    const UInt32 ringDW = (render ? R_RING_BYTES : G_RING_BYTES) / 4;
    UInt32 need = count + 6;                               // + seqno store + interrupt + pad
    // v0.68.33: never more than half the ring (see mtlRunDraw3D): after a wrap the job would end past the GPU's
    // stopping point and only its middle would run (a hang). Refused instead - the caller reports a failed job.
    if (need >= ringDW / 2) { gpuRingTooBig++; return false; }
    UInt32 t = tailRef / 4;
    if (t + need >= ringDW) {                              // wrap: pad the end with NOOPs
        while (t < ringDW) ring[t++] = MI_NOOP;
        t = 0;
    }
    jobRingStart = t * 4; jobCount = count;           // v0.32.2: where a hang happened (IrisXe-Hang)
    for (UInt32 i = 0; i < count; i++) ring[t++] = cmds[i];
    seqRef++;
    ring[t++] = MI_SDI(2) | MI_USE_GGTT;
    ring[t++] = ggttBase + hwspPg * 4096 + HWSP_SEQ * 4;
    ring[t++] = 0;
    ring[t++] = seqRef;
    ring[t++] = MI_USER_INTERRUPT;
    if (t & 1) ring[t++] = MI_NOOP;
    if (t >= ringDW) t = 0;
    tailRef = t * 4;

    volatile UInt32 *regs = base + (lrcPg + 1) * 1024;
    regs[CTX_RING_TAIL] = tailRef;
    // v0.40.9: render jobs never restore the saved engine state. Every job sets up the whole pipeline itself
    // (PIPELINE_SELECT, L3, STATE_BASE_ADDRESS, all 3D state), and the first job after start-up - which runs
    // with restore inhibited - always worked, while the next one, restoring what a clear left behind, hung at its
    // very first command (irisxe-mtkdemo, v0.40.2-v0.40.8). Only the ring registers are loaded then.
    bool restore = !freshRef && !render;
    if (!freshRef)
        regs[CTX_CONTEXT_CONTROL] = MASKED_ON(CTX_INHIBIT_SYN) |
                                    (render ? MASKED_ON(CTX_RESTORE_INHIBIT) : MASKED_OFF(CTX_RESTORE_INHIBIT));
    __asm__ volatile("mfence" ::: "memory");

    if (!gpuForcewake(true)) { gpuForcewake(false); gpuStatus("forcewake failed"); return false; }
    UInt32 lo = (ggttBase + lrcPg * 4096) | (3u << 3) | (1u << 8) | 1u;
    if (restore) lo |= (1u << 2);                          // CTX_DESC_FORCE_RESTORE (blitter only, v0.40.9)
    UInt32 hi = render ? (4u << 5) : ((3u << 5) | (3u << 29));   // sw ctx id; class render=0 / copy=3
    WR(engBase + R_EL_SQ + 8,  0);
    WR(engBase + R_EL_SQ + 12, 0);
    WR(engBase + R_EL_SQ + 0,  lo);
    WR(engBase + R_EL_SQ + 4,  hi);
    WR(engBase + R_EL_CONTROL, 1);
    freshRef = false;
    gpuBusyRender = render;
    gpuBusy = true;
    gpuBusyMS = 0;
    clock_get_uptime(&gpuJobStart);
    gpuTimer->setTimeoutMS(POLL_MS);                       // fallback / watchdog
    return true;
}

// v0.45.0: Metal jobs wait for the job-end interrupt instead of polling every millisecond. IOSleep(1) made every
// draw cost at least ~1-2 ms (Maps: ~5 ms between draws, ~100 draws a frame = the lag). Returns ms since t0.
// seq 0: wait for the end of any job (the GPU was busy). At most 1 ms per call, so timeouts still work.
UInt32 IrisXeProbe::gpuWait(bool render, UInt32 seq, uint64_t t0) {
    if (gpuDoneLock) {
        IOLockLock(gpuDoneLock);
        bool done = seq && (SInt32)((render ? rcsSeqDone : gpuSeqDone) - seq) >= 0;
        if (!done) {
            uint64_t dl; clock_interval_to_deadline(1, 1000000, &dl);          // 1 ms
            AbsoluteTime adl; memcpy(&adl, &dl, sizeof(adl));
            IOLockSleepDeadline(gpuDoneLock, &gpuDoneEvent, adl, 0 /* THREAD_UNINT */);
        }
        IOLockUnlock(gpuDoneLock);
    } else IOSleep(1);
    uint64_t now; clock_get_uptime(&now);
    UInt64 ns; absolutetime_to_nanoseconds(now - t0, &ns);
    return (UInt32)(ns / 1000000);
}
void IrisXeProbe::gpuWakeWaiters() {
    if (!gpuDoneLock) return;
    IOLockLock(gpuDoneLock);
    IOLockWakeup(gpuDoneLock, &gpuDoneEvent, false);
    IOLockUnlock(gpuDoneLock);
}

void IrisXeProbe::gpuSubmitDemo() {
    if (!fbOK) { gpuDemoLeft = 0; gpuTimer->setTimeoutMS(HEARTBEAT_MS); return; }
    static const UInt32 colors[DEMO_JOBS] = {
        0xFFE53935, 0xFFFB8C00, 0xFFFDD835, 0xFF7CB342, 0xFF43A047, 0xFF00ACC1,
        0xFF1E88E5, 0xFF3949AB, 0xFF8E24AA, 0xFFD81B60, 0xFFFFFFFF, 0xFF26A69A };
    UInt32 k = DEMO_JOBS - gpuDemoLeft;
    UInt32 x1 = 96 + k * 28, y1 = 260, x2 = x1 + 24, y2 = y1 + 120;
    UInt32 c[] = {
        XY_COLOR_BLT_CMD | BLT_WRITE_RGBA | (7 - 2),
        BLT_DEPTH_32 | BLT_ROP_COLOR_COPY | fbStride,
        (y1 << 16) | x1, (y2 << 16) | x2,
        (UInt32)FB_GPU_VA, (UInt32)(FB_GPU_VA >> 32),
        colors[k], MI_NOOP,
        MI_FLUSH_DW_4, 0, 0, 0,
    };
    if (gpuSubmit(c, sizeof(c) / 4, false)) gpuDemoLeft--;
}

// heartbeats alternate: blitter no-op job / render PIPE_CONTROL job
void IrisXeProbe::gpuSubmitHeartbeat() {
    gpuHeartbeats++;
    // v0.43.0: no render heartbeat jobs between Metal jobs (only while no app uses the GPU)
    if (rcsOn && (gpuHeartbeats & 1) && !mtlSpaceStat[3]) { rcsSubmitPipeControl(); return; }
    UInt32 c[] = { MI_NOOP, MI_NOOP };
    gpuSubmit(c, 2, false);
}

// render job: CS stall + post-sync quad-word write into the render status page
void IrisXeProbe::rcsSubmitPipeControl() {
    UInt32 seq = rcsSeqSubmitted + 1;
    UInt32 addr = R_GGTT_ADDR + R_HWSP * 4096 + 0x48 * 4;
    UInt32 c[] = {
        GFX_OP_PIPE_CONTROL(6),
        PC_CS_STALL | PC_QW_WRITE | PC_GLOBAL_GTT,
        addr, 0,
        PC_MAGIC | (seq & 0xFF), seq,
    };
    if (gpuSubmit(c, sizeof(c) / 4, true)) rcsPCSent++;
}

// job finished? On workloop (interrupt handler or timer).
void IrisXeProbe::gpuCheckDone(bool viaIRQ) {
    if (!gpuBusy) return;
    bool render = gpuBusyRender;
    UInt32 seq = render ? rcsVA[R_HWSP * 1024 + HWSP_SEQ] : gpuVA[G_HWSP * 1024 + HWSP_SEQ];
    if (seq != (render ? rcsSeqSubmitted : gpuSeqSubmitted)) return;
    gpuBusy = false;
    if (render) {
        rcsSeqDone = seq;
        clock_get_uptime(&rcsDoneAt);                       // v0.60.1: IrisXe-DrawPhases (wake-up delay)
        volatile UInt32 *h = rcsVA + R_HWSP * 1024;
        if (h[0x49] == seq && h[0x48] == (PC_MAGIC | (seq & 0xFF))) rcsPCOK++;
        if (cptPending) {                                   // compute job finished: check output
            cptPending = false;
            volatile UInt32 *dp = rcsVA + R_CPT_DATA * 1024;
            UInt32 good = 0;
            for (UInt32 k = 0; k < 8; k++) if (dp[k] == 0xC0DE0000u + k) good++;
            cptGood = good;
            snprintf(cptResult, sizeof(cptResult), "%s (%u/8 lanes) %08x %08x .. %08x",
                     good == 8 ? "SHADER RAN" : "wrong output", good, dp[0], dp[1], dp[7]);
        }
    } else {
        gpuSeqDone = seq;
    }
    gpuWakeWaiters();                                      // v0.45.0
    if (viaIRQ) gpuDoneIRQ++; else gpuDonePoll++;
    uint64_t now; clock_get_uptime(&now);
    UInt64 ns; absolutetime_to_nanoseconds(now - gpuJobStart, &ns);
    gpuLastJobUS = (UInt32)(ns / 1000);
    gpuForcewake(false);                                   // let the GPU sleep
    if (gpuDemoLeft) gpuSubmitDemo();
    else if (rcsOn && !rcsFirstDone) {                     // first render job right after the demo
        rcsFirstDone = true;
        rcsSubmitPipeControl();
    } else if (rcsOn && cptWanted && !cptSent) {           // then the compute shader test
        cptSent = true;
        gpuSubmit(kComputeCmds, sizeof(kComputeCmds) / 4, true);
        cptPending = true;
    } else gpuTimer->setTimeoutMS(HEARTBEAT_MS);
    gpuStatus(nullptr);
}

bool IrisXeProbe::gpuIrqFilter(OSObject *owner, IOFilterInterruptEventSource *) {
    IrisXeProbe *self = (IrisXeProbe *)owner;
    volatile UInt8 *gpuMMIO = self->gpuMMIO;
    if (!gpuMMIO) return false;
    WR(GFX_MSTR_IRQ, 0);
    UInt32 m = RD(GFX_MSTR_IRQ);
    if (!m) { if (!self->gpuIrqStorm) WR(GFX_MSTR_IRQ, MASTER_IRQ); return false; }
    bool user = false;
    for (UInt32 b = 0; b < 2; b++) {
        if (!(m & (1u << b))) continue;
        UInt32 dw = RD(GT_INTR_DW(b));
        for (UInt32 bit = 0; bit < 32; bit++) {
            if (!(dw & (1u << bit))) continue;
            WR(IIR_SELECTOR(b), 1u << bit);
            UInt32 id = 0;
            for (UInt32 t = 0; t < 100; t++) { id = RD(INTR_IDENTITY(b)); if (id & (1u << 31)) break; IODelay(1); }
            WR(INTR_IDENTITY(b), id);
            UInt32 cls = (id >> 16) & 7;
            if ((cls == 3 || cls == 0) && (id & 1)) user = true;   // copy or render user interrupt
        }
        WR(GT_INTR_DW(b), dw);
    }
    if (m & GU_MISC_IRQ) WR(GU_MISC_IIR, RD(GU_MISC_IIR));
    if (m & DISPLAY_IRQ) self->gpuIrqDisplay++;
    self->gpuIrqTotal++;
    if (user) self->gpuIrqUser++;
    if (self->gpuIrqTotal > 200000 && self->gpuIrqUser * 4 < self->gpuIrqTotal)
        self->gpuIrqStorm = true;                          // safety: stop if flooded
    if (!self->gpuIrqStorm) WR(GFX_MSTR_IRQ, MASTER_IRQ);
    return user;
}

void IrisXeProbe::gpuIrqHandler(OSObject *owner, IOInterruptEventSource *, int) {
    IrisXeProbe *self = (IrisXeProbe *)owner;
    self->gpuCheckDone(true);
}

void IrisXeProbe::gpuTimerFired(OSObject *owner, IOTimerEventSource *sender) {
    IrisXeProbe *self = (IrisXeProbe *)owner;
    if (!self->gpuUp) return;
    if (self->gpuBusy) {
        self->gpuCheckDone(false);
        if (!self->gpuBusy) return;
        self->gpuBusyMS += POLL_MS;
        if (self->gpuBusyMS >= JOB_TIMEOUT_MS) {           // GPU did not finish: stop using it
            self->gpuHung = true;
            self->gpuBusy = false;
            volatile UInt8 *gpuMMIO = self->gpuMMIO;
            char s[96];
            UInt32 eb = self->gpuBusyRender ? RCS : BCS;
            UInt32 head = RD(eb + R_HEAD) & 0x1FFFFC;
            snprintf(s, sizeof(s), "HUNG on %s: head=%x tail=%x esr=%x ipehr=%x",
                     self->gpuBusyRender ? "render" : "blitter",
                     RD(eb + R_HEAD), RD(eb + R_TAIL), RD(eb + R_ESR), RD(eb + 0x68));
            {   // v0.32.2: which command of the job (dword index) the engine stopped at
                char h[480];
                // v0.40.5: + the GPU page fault registers (i915 GEN12_RING_FAULT_REG / GEN12_FAULT_TLB_DATA0/1):
                // bit 0 of fault = valid; the faulting address = (data1 & 0xF) << 44 | data0 << 12
                UInt32 flt = RD(0xcec4), fd0 = RD(0xceb8), fd1 = RD(0xcebc);
                UInt64 fva = ((UInt64)(fd1 & 0xF) << 44) | ((UInt64)fd0 << 12);
                MtlSpace *cs = self->mtlCur && self->mtlCur < IRISXE_MAX_SPACES ? self->mtlSpaces[self->mtlCur] : nullptr;
                snprintf(h, sizeof(h), "%s | job of %u dwords at ring 0x%x: head at dword %d, acthd %x, instdone %x"
                         " | fault reg %x (%s) at GPU 0x%llx (data1 %x) | space %u: %u page tables, %u pages mapped"
                         " | last draw: vs 0x%llx ps 0x%llx, %u vertices x %u, index type %u, %u RT, %u textures, %u VBs",
                         s, self->jobCount, self->jobRingStart, (int)(head - self->jobRingStart) / 4,
                         RD(eb + 0x74), RD(eb + 0x6C), flt, (flt & 1) ? "PAGE FAULT" : "no fault", fva, fd1,
                         self->mtlCur, cs ? cs->tables : 0, cs ? cs->mapped : 0,
                         self->lastVS, self->lastPS, self->lastVerts, self->lastInst, self->lastIdx, self->lastRT,
                         self->lastTex, self->lastVB);
                self->setProperty("IrisXe-Hang", h);
                {   // v0.43.0: the last 16 jobs before the hang, oldest first
                    char hist[1200]; size_t o = 0;
                    UInt32 n = self->jobHistN < 16 ? self->jobHistN : 16;
                    for (UInt32 i = self->jobHistN - n; i != self->jobHistN && o + 80 < sizeof(hist); i++) {
                        const JobRec &j = self->jobHist[i & 31];
                        o += snprintf(hist + o, sizeof(hist) - o, "%s%u.%03u ms %s space %u %u dw [%08x] seq %u",
                                      o ? " | " : "", j.usec / 1000 % 100000, j.usec % 1000,
                                      j.engine == 2 ? "DRAW" : j.engine == 1 ? "render" : "blit", j.space, j.dwords, j.first, j.seq);
                    }
                    self->setProperty("IrisXe-HangHistory", hist);
                    char m[96];
                    snprintf(m, sizeof(m), "memory map changes that waited for a running job: %u", self->mapsDuringJob);
                    self->setProperty("IrisXe-HangMaps", m);
                }
                // v0.42.4: the hung draw job and the draw state pages, for decoding offline
                if (sLastJobIsDraw && self->jobCount && self->jobCount <= 4096) {
                    OSData *j = OSData::withBytes(sDrawBuf, self->jobCount * 4);
                    if (j) { self->setProperty("IrisXe-HangJob", j); j->release(); }
                    if (self->drwVA) {
                        OSData *st = OSData::withBytes((const void *)self->drwVA, DRW_PAGES * 4096);
                        if (st) { self->setProperty("IrisXe-HangState", st); st->release(); }
                    }
                }
            }
            self->gpuForcewake(false);
            if (self->cptPending) {
                volatile UInt32 *dp = self->rcsVA + R_CPT_DATA * 1024;
                snprintf(self->cptResult, sizeof(self->cptResult), "GPU hung during shader; out %08x %08x", dp[0], dp[1]);
            }
            self->gpuStatus(s);
            // v0.44.0: reset the GPU instead of leaving it dead until a reboot (-irisxenoreset: don't)
            if (!PE_parse_boot_argn("-irisxenoreset", nullptr, 0)) self->gpuResetGated();
            if (!self->gpuHung) { sender->setTimeoutMS(HEARTBEAT_MS); }
            return;
        }
        sender->setTimeoutMS(POLL_MS);
        return;
    }
    self->gpuSubmitHeartbeat();
    self->gpuStatus(nullptr);
}

void IrisXeProbe::gpuStatus(const char *err) {
    // RC6 share of the time since the last report (counter unit 1.28 us)
    UInt32 rc6 = gpuMMIO ? RD(RC6_RESIDENCY) : 0;
    uint64_t now; clock_get_uptime(&now);
    UInt64 ns = 0; absolutetime_to_nanoseconds(now - gpuLastRC6Time, &ns);
    UInt32 pct = 0;
    if (ns > 1000000) {
        UInt64 rcNs = (UInt64)(rc6 - gpuLastRC6) * 1280;
        pct = (UInt32)(rcNs * 100 / ns);
        if (pct > 100) pct = 100;
        gpuLastRC6 = rc6; gpuLastRC6Time = now;
        gpuRC6Pct = pct;
    }
    char s[400];
    char r[96];
    if (rcsOn) snprintf(r, sizeof(r), "render: jobs %u done, pipe_control %u/%u ok", rcsSeqDone, rcsPCOK, rcsPCSent);
    if (cptResult[0]) setProperty("IrisXe-Compute", cptResult);
    else if (cptWanted) setProperty("IrisXe-Compute", cptSent ? "running" : "waiting");
    else snprintf(r, sizeof(r), "render: off");
    snprintf(s, sizeof(s),
             "%s | blitter jobs %u | %s | done via irq %u, poll %u, last %u us | irq total %u user %u display %u%s | "
             "RC6 %s %u%% | heartbeats %u | %s irq idx %d | wakes %u | workarounds %s",
             err ? err : (gpuHung ? "HUNG" : (gpuUp ? "running" : "down")),
             gpuSeqDone, r, gpuDoneIRQ, gpuDonePoll, gpuLastJobUS,
             gpuIrqTotal, gpuIrqUser, gpuIrqDisplay, gpuIrqStorm ? " STORM-STOPPED" : "",
             gpuRC6On ? "on" : "off", gpuRC6Pct, gpuHeartbeats,
             gpuIrqMSI ? "MSI" : "legacy", gpuIrqIndex, gpuWakes, waApplied ? "on" : "off");
    setProperty("IrisXe-GPU", s);
    char m[480];
    snprintf(m, sizeof(m), "page tables %s | pages mapped %u | Metal jobs %u ok, %u failed | "
             "process spaces: live %u, created %u, freed %u, switches %u, kept (GPU busy) %u | compute jobs %u ok, %u failed (with barriers %u, split last group %u, kernels in GPU memory %u, with textures %u) | draws %u, clears %u, failed %u, cs_debug_mode2 %x",
             mtlPT ? "ready (GPU VA 0-256 MB)" : "NONE", mtlMapped, mtlJobs, mtlJobFails,
             mtlSpaceStat[3], mtlSpaceStat[0], mtlSpaceStat[1], mtlSpaceStat[2], mtlSpaceStat[4],
             mtlComputeJobs, mtlComputeFails, mtlComputeBarrier, mtlComputeSplit, mtlComputeBig, mtlComputeTex,
             mtlDraws, mtlClears, mtlDrawFails, mtlDbgMode2 & 0x7FFFFFFFu);
    setProperty("IrisXe-Metal", m);
    if (!getProperty("IrisXe-Draw")) setProperty("IrisXe-Draw", "ready (milestone 5a: run irisxe-metaltest)");
    if (!getProperty("IrisXe-DrawC")) setProperty("IrisXe-DrawC", "ready (milestone 5b: run irisxe-metaltest)");
}

// ---- v0.44.0: GPU reset after a hang (as i915 does on Gen12: request "ready to reset" from each engine,
// then a full GT reset with GDRST, then the same start-up as after a system wake). Display is not reset.
// Jobs that were waiting see gpuResets change and fail with -9.
bool IrisXeProbe::gpuResetGated() {
    gpuForcewake(true);
    static const UInt32 engines[] = { RCS, BCS };
    for (UInt32 e : engines) {
        WR(e + 0xd0, MASKED_ON(1u << 0));                      // RING_RESET_CTL: REQUEST_RESET
        for (UInt32 t = 0; t < 70 && !(RD(e + 0xd0) & (1u << 1)); t++) IODelay(10);   // READY_TO_RESET (may never come)
    }
    WR(0x941c, 1u << 0);                                       // GEN6_GDRST: GRDOM_FULL
    UInt32 t = 0;
    for (; t < 50000 && (RD(0x941c) & 1u); t++) IODelay(10);  // up to 500 ms
    bool ok = !(RD(0x941c) & 1u);
    for (UInt32 e : engines) WR(e + 0xd0, MASKED_OFF(1u << 0));
    gpuForcewake(false);
    gpuResets++;
    if (!ok) { gpuResetFails++; gpuStatus("HUNG (GPU reset failed)"); return false; }
    // everything after a reset is like after a wake: fresh rings, contexts, page tables, registers
    gpuBusy = false;
    gpuBusyMS = 0;
    gpuBuildTables();
    rcsSeqDone = rcsSeqSubmitted; gpuSeqDone = gpuSeqSubmitted;  // nothing is running any more
    gpuVA[G_HWSP * 1024 + HWSP_SEQ] = gpuSeqSubmitted;
    if (rcsOn) rcsVA[R_HWSP * 1024 + HWSP_SEQ] = rcsSeqSubmitted;
    mtlCur = 0xFFFF;                                            // the next job points the GPU at its space again
    mtlLastDone = true;
    gpuHung = false;
    gpuUp = false;
    gpuBringUpGated();
    char r[96];
    snprintf(r, sizeof(r), "%u resets (%u failed), last took %u us | jobs too big for the ring: %u", gpuResets, gpuResetFails, t * 10, gpuRingTooBig);
    setProperty("IrisXe-Resets", r);
    gpuWakeWaiters();                                           // v0.45.0
    gpuStatus(nullptr);
    return gpuUp;
}

// system sleep / wake
IOReturn IrisXeProbe::setPowerState(unsigned long state, IOService *) {
    if (!gpuWL) return IOPMAckImplied;
    if (state == 0) {
        gpuWL->runAction(OSMemberFunctionCast(IOWorkLoop::Action, this, &IrisXeProbe::gpuSleepGated), this);
    } else if (gpuMMIO && !gpuUp && gpuWasUp) {
        gpuWakes++;
        gpuWL->runAction(OSMemberFunctionCast(IOWorkLoop::Action, this, &IrisXeProbe::gpuBringUpGated), this);
    }
    return IOPMAckImplied;
}

IOReturn IrisXeProbe::gpuSleepGated() {
    if (!gpuUp) return kIOReturnSuccess;
    gpuTimer->cancelTimeout();
    // let a running job finish (max 100 ms)
    for (UInt32 t = 0; gpuBusy && t < 100; t++) { IODelay(1000); gpuCheckDone(false); }
    if (gpuBusy) { gpuBusy = false; gpuForcewake(false); }
    WR(GFX_MSTR_IRQ, 0);
    gpuUp = false;
    gpuWasUp = true;
    // after wake the engine has lost its state: start the context fresh again
    gpuBuildTables();
    gpuVA[G_HWSP * 1024 + HWSP_SEQ] = gpuSeqSubmitted;
    if (rcsOn) rcsVA[R_HWSP * 1024 + HWSP_SEQ] = rcsSeqSubmitted;
    gpuStatus("sleeping");
    return kIOReturnSuccess;
}

// ============================================================================
// Metal (milestone 2c): page tables for Metal buffers + blitter jobs.
// IrisXeAccel (another kext) calls these through callPlatformFunction, so the
// two kexts need no link dependency (IrisXeCore lives in OpenCore, IrisXeAccel
// in /Library/Extensions).
//
// v0.23.0: each Metal process gets its own GPU address space (mtlSpaceOp).
// each process its own range starting at 4 KB, so two Metal programs at the
// same time would overlap) and GPU addresses below 256 MB.
// ============================================================================

#define MTL_VA_LIMIT ((UInt64)MTL_PT_PAGES * 512 * 4096)
static_assert(MTL_VA_LIMIT == IRISXE_LO_END, "low Metal range");
static_assert((IRISXE_HI_VA >> 21) >= MTL_PT_PAGES && ((IRISXE_HI_END - 1) >> 21) < MTL_SPACE_PTS &&
              (IRISXE_HI_VA >> 30) == 1 && ((IRISXE_HI_END - 1) >> 30) == MTL_HI_PDS, "high Metal range = PDP entries 1..3");

// v0.58.7: a GPU range a Metal job or map may use: 0..512 MB, or 1 GB .. 4 GB - 64 KB
bool IrisXeProbe::mtlVAOK(UInt64 va, UInt64 len) const {
    if (va + len < va) return false;
    return va + len <= MTL_VA_LIMIT || (va >= IRISXE_HI_VA && va + len <= IRISXE_HI_END);
}

bool IrisXeProbe::mtlMapPages(UInt64 va, const UInt64 *pa, UInt32 count) {
    if (!mtlPT || !pa || (va & 0xFFF) || va + (UInt64)count * 4096 > MTL_VA_LIMIT) return false;
    IOLockLock(mtlLock);
    UInt64 first = va >> 12;
    for (UInt32 i = 0; i < count; i++) {
        UInt64 e = (pa[i] & 0x0000007FFFFFF000ULL) | 3;      // present + writable, PAT index 0 (WB)
        mtlPT[(first + i) * 2]     = (UInt32)e;
        mtlPT[(first + i) * 2 + 1] = (UInt32)(e >> 32);
    }
    mtlMapped += count;
    __asm__ volatile("mfence" ::: "memory");
    IOLockUnlock(mtlLock);
    return true;
}

void IrisXeProbe::mtlUnmapPages(UInt64 va, UInt32 count) {
    if (!mtlPT || (va & 0xFFF) || va + (UInt64)count * 4096 > MTL_VA_LIMIT) return;
    IOLockLock(mtlLock);
    UInt64 first = va >> 12;
    for (UInt32 i = 0; i < count; i++) { mtlPT[(first + i) * 2] = 0; mtlPT[(first + i) * 2 + 1] = 0; }
    mtlMapped = mtlMapped > count ? mtlMapped - count : 0;
    __asm__ volatile("mfence" ::: "memory");
    IOLockUnlock(mtlLock);
}

IOReturn IrisXeProbe::mtlTrySubmitGated(OSObject *owner, void *cmds, void *count, void *seqOut, void *) {
    IrisXeProbe *me = (IrisXeProbe *)owner;
    if (me->gpuBusy) return kIOReturnBusy;                 // heartbeat / demo job still running
    if (!me->gpuSubmit((const UInt32 *)cmds, (UInt32)(uintptr_t)count, false)) return kIOReturnError;
    *(UInt32 *)seqOut = me->gpuSeqSubmitted;
    return kIOReturnSuccess;
}

// run a blitter job (commands are wrapped with a TLB invalidate first) and
// wait until it finished. 0 = done, -1 not ready, -2 could not submit, -3 timeout
int IrisXeProbe::mtlRunBlit(const UInt32 *cmds, UInt32 count, UInt32 timeoutMS, UInt32 *seqOut) {
    if (!gpuUp || gpuHung || !gpuWL || !cmds || count > 1000) return -1;
    const UInt32 jobBytes = (count + 8) * 4;
    UInt32 *job = (UInt32 *)IOMalloc(jobBytes);           // kernel stacks are small
    if (!job) return -1;
    UInt32 n = 0;
    // MI_FLUSH_DW: invalidate the GPU's TLBs (new page-table entries), post-sync
    // store into the status page scratch (i915 does the same)
    job[n++] = MI_FLUSH_DW_4 | (1u << 18) | (1u << 21) | (1u << 14);
    job[n++] = (0x80 * 4) | (1u << 2);
    job[n++] = 0;
    job[n++] = 0;
    for (UInt32 i = 0; i < count; i++) job[n++] = cmds[i];
    job[n++] = MI_FLUSH_DW_4;                              // results out to memory
    job[n++] = 0; job[n++] = 0; job[n++] = 0;

    UInt32 seq = 0;
    UInt32 waited = 0;
    UInt32 resets0 = gpuResets;                            // v0.44.0
    uint64_t t0_; clock_get_uptime(&t0_);
    for (;;) {
        IOReturn r = gpuWL->runAction(&IrisXeProbe::mtlTrySubmitGated, this, job, (void *)(uintptr_t)n, &seq);
        if (r == kIOReturnSuccess) break;
        if (r != kIOReturnBusy || waited >= timeoutMS) { IOFree(job, jobBytes); mtlJobFails++; return -2; }
        waited = gpuWait(false, 0, t0_);          // v0.45.0: wake on the next job end
    }
    IOFree(job, jobBytes);                                 // copied into the ring already
    if (seqOut) *seqOut = seq;
    while ((SInt32)(gpuSeqDone - seq) < 0) {
        if (waited >= timeoutMS) { mtlJobFails++; return -3; }
        waited = gpuWait(false, seq, t0_);          // v0.45.0: woken by the job-end interrupt
    }
    if (gpuResets != resets0) { mtlJobFails++; return -9; }  // v0.44.0: it hung, the GPU was reset
    mtlJobs++;
    gpuStatus(nullptr);
    return 0;
}

// ---- milestone 2e: per-process GPU address spaces ---------------------------
// page table covering va in a space (space 0 = the shared table); with create,
// a missing table of a process space is allocated (and hooked into the PD if
// that space is the current one). Called with mtlLock held.
// v0.58.7: one page of GPU paging structure (4 KB, contiguous, uncached, zeroed)
// v0.68.36: every entry starts as `fill` (a 64-bit entry; 0 = not present)
static volatile UInt32 *ptPage(IOBufferMemoryDescriptor **mem, UInt64 *phys, UInt64 fill = 0) {
    IOBufferMemoryDescriptor *m = IOBufferMemoryDescriptor::withOptions(
        kIODirectionInOut | kIOMemoryPhysicallyContiguous | kIOMapInhibitCache, 4096, 4096);
    if (!m) return nullptr;
    IOByteCount seg = 0;
    volatile UInt32 *v = (volatile UInt32 *)m->getBytesNoCopy();
    UInt64 ph = m->getPhysicalSegment(0, &seg, kIOMemoryMapperNone);
    if (!v || !ph) { m->release(); return nullptr; }
    for (UInt32 i = 0; i < 512; i++) { v[i * 2] = (UInt32)fill; v[i * 2 + 1] = (UInt32)(fill >> 32); }
    *mem = m; *phys = ph;
    return v;
}

// v0.68.36: scratch page (as i915 does). In the process spaces every GPU address without memory points at one
// zeroed scratch page instead of "not present": a shader that reads or writes a little past its buffer (Terminal's
// text blit read a byte past the end now and then; Chromium shaders) then gets zeros instead of a GPU page fault -
// which on this GPU is a hang, 2 s of frozen screen and a GPU reset. Missing page tables / directories point at a
// scratch table / directory whose entries all point at the scratch page. Space 0 (the shared table) keeps
// "not present" (its checks read the present bit). Boot-arg -irisxenoscratch: off.
static IOBufferMemoryDescriptor *sScratchMem, *sScratchPTMem, *sScratchPDMem;
static UInt64 sScratchPTE, sScratchPTEntry, sScratchPDEntry;   // 0 = not made (or off)
static bool scratchReady() {
    static int state = -1;                                     // -1 not tried, 0 off / failed, 1 ready
    if (state >= 0) return state == 1;
    state = 0;
    if (PE_parse_boot_argn("-irisxenoscratch", nullptr, 0)) return false;
    UInt64 pg = 0, pt = 0, pd = 0;
    if (!ptPage(&sScratchMem, &pg, 0)) return false;
    if (!ptPage(&sScratchPTMem, &pt, pg | 3)) return false;
    if (!ptPage(&sScratchPDMem, &pd, pt | 3)) return false;
    sScratchPTE = pg | 3; sScratchPTEntry = pt | 3; sScratchPDEntry = pd | 3;
    state = 1;
    return true;
}

volatile UInt32 *IrisXeProbe::mtlTable(UInt32 space, UInt64 va, bool create) {
    UInt32 t = (UInt32)(va >> 21);
    bool hi = va >= IRISXE_HI_VA;
    if (hi ? (va >= IRISXE_HI_END || t >= MTL_SPACE_PTS) : t >= MTL_PT_PAGES) return nullptr;
    if (space == 0) return !hi && mtlPT ? mtlPT + t * 1024 : nullptr;     // the shared space: 0..512 MB only
    MtlSpace *sp = space < IRISXE_MAX_SPACES ? mtlSpaces[space] : nullptr;
    if (!sp) return nullptr;
    if (!sp->pt[t] && create) {
        // v0.58.7: 1..4 GB: the table goes into the space's own page directory (made first if needed);
        // the PDP entry points at that directory while the space is the current one
        UInt32 h = 0;
        if (hi) {
            h = (UInt32)(va >> 30) - 1;
            if (!sp->pd[h]) {
                sp->pd[h] = ptPage(&sp->pdMem[h], &sp->pdPhys[h], scratchReady() ? sScratchPTEntry : 0);
                if (!sp->pd[h]) return nullptr;
                if (mtlCur == space) {
                    volatile UInt32 *pdp = gpuVA + G_PDP * 1024;
                    UInt64 e = sp->pdPhys[h] | 3;
                    pdp[(h + 1) * 2] = (UInt32)e; pdp[(h + 1) * 2 + 1] = (UInt32)(e >> 32);
                }
            }
        }
        IOBufferMemoryDescriptor *m = nullptr; UInt64 ph = 0;
        volatile UInt32 *v = ptPage(&m, &ph, scratchReady() ? sScratchPTE : 0);
        if (!v) return nullptr;
        sp->ptMem[t] = m; sp->pt[t] = v; sp->ptPhys[t] = ph; sp->tables++;
        UInt64 e = ph | 3;
        if (hi) {
            volatile UInt32 *pd = sp->pd[h];
            UInt32 k = t & 511;
            pd[k * 2] = (UInt32)e; pd[k * 2 + 1] = (UInt32)(e >> 32);
        } else if (mtlCur == space) {
            volatile UInt32 *pd = gpuVA + G_PD * 1024;
            pd[t * 2] = (UInt32)e; pd[t * 2 + 1] = (UInt32)(e >> 32);
        }
        __asm__ volatile("mfence" ::: "memory");
    }
    return sp->pt[t];
}

// point PD entries 0..255 (GPU VA 0..512 MB) and, v0.58.7, PDP entries 1..3 (1..4 GB) at a space
// (missing tables / directories = not present).
// Called with mtlJobLock and mtlLock held, while no Metal job is on the GPU.
void IrisXeProbe::mtlPointPD(UInt32 space) {
    volatile UInt32 *pd = gpuVA + G_PD * 1024;
    volatile UInt32 *pdp = gpuVA + G_PDP * 1024;
    MtlSpace *sp = space && space < IRISXE_MAX_SPACES ? mtlSpaces[space] : nullptr;
    const bool scr = sp && scratchReady();                      // v0.68.36: process spaces: scratch, not "missing"
    for (UInt32 t = 0; t < MTL_PT_PAGES; t++) {
        UInt64 e = 0;
        if (space == 0) e = mtlPT ? ((mtlPTPhys + (UInt64)t * 4096) | 3) : 0;
        else if (sp && sp->pt[t]) e = sp->ptPhys[t] | 3;
        else if (scr) e = sScratchPTEntry;
        pd[t * 2] = (UInt32)e; pd[t * 2 + 1] = (UInt32)(e >> 32);
    }
    for (UInt32 h = 0; h < MTL_HI_PDS; h++) {
        UInt64 e = sp && sp->pd[h] ? (sp->pdPhys[h] | 3) : (scr ? sScratchPDEntry : 0);
        pdp[(h + 1) * 2] = (UInt32)e; pdp[(h + 1) * 2 + 1] = (UInt32)(e >> 32);
    }
    __asm__ volatile("mfence" ::: "memory");
    if (mtlCur != space) mtlSpaceStat[2]++;
    mtlCur = space;
}

// v0.58.7: IrisXe-GPUSpace: how much of each process's GPU address space holds memory
void IrisXeProbe::mtlSpacePublish() {
    if (!mtlLock) return;
    char t[900];
    int n = snprintf(t, sizeof(t), "per process: 0..512 MB + 1..4 GB | map failures %u", mtlMapFails);
    IOLockLock(mtlLock);
    for (UInt32 id = 1; id < IRISXE_MAX_SPACES && n > 0 && n < (int)sizeof(t) - 80; id++) {
        MtlSpace *sp = mtlSpaces[id];
        if (!sp) continue;
        n += snprintf(t + n, sizeof(t) - n, " | space %u: %u MB (%u MB above 1 GB, peak %u MB), %u tables",
                      id, sp->mapped / 256, sp->hiMapped / 256, sp->peakMapped / 256, sp->tables);
    }
    IOLockUnlock(mtlLock);
    setProperty("IrisXe-GPUSpace", t);
}

// point the GPU at a process's space before its job (mtlJobLock held).
// Refused (-4) while an earlier timed-out job may still run: it would touch
// another process's memory. -5 = no such space.
bool IrisXeProbe::mtlSwitchSpace(UInt32 space, int *err) {
    if (mtlCur == space) return true;
    UInt32 done = mtlLastRender ? rcsSeqDone : gpuSeqDone;
    if (!mtlLastDone && (SInt32)(done - mtlLastSeq) < 0) { *err = -4; return false; }
    IOLockLock(mtlLock);
    bool exists = space == 0 || (space < IRISXE_MAX_SPACES && mtlSpaces[space]);
    if (exists) mtlPointPD(space);
    IOLockUnlock(mtlLock);
    if (!exists) *err = -5;
    return exists;
}

IOReturn IrisXeProbe::mtlTrySubmitRenderGated(OSObject *owner, void *cmds, void *count, void *seqOut, void *) {
    IrisXeProbe *me = (IrisXeProbe *)owner;
    if (me->gpuBusy) return kIOReturnBusy;
    if (!me->gpuSubmit((const UInt32 *)cmds, (UInt32)(uintptr_t)count, true)) return kIOReturnError;
    *(UInt32 *)seqOut = me->rcsSeqSubmitted;
    return kIOReturnSuccess;
}

// ---- milestone 3a: a compute job on the render engine ------------------------
// Kernel, interface descriptor and constant (CURBE) data go into our own pages
// at GPU VA 0x20000000 (PD entry 256: outside the per-process Metal range, so
// every space sees them). Commands follow Mesa iris for GFX 12.0 (GPGPU_WALKER).
#define CPT_KERNEL_MAX  (0xF80 / 4)              // kernel dwords at 0x20000000 (v0.26: 3968 bytes)
#define CPT_IDD_OFF     0xF80                    // interface descriptor (end of the kernel page)
#define CPT_CURBE_VA    (CPT_GPU_VA + 0x1000)    // constant data page (R_CPT_DATA)
// milestone 4: the state page (R_CPT_STATE) is also the surface state base:
//   +0x000 binding table (entry k = texture slot k), +0x040 * (1 + k) surface states,
//   +0x400 sampler states (16 bytes each), +0x600 border color (black, transparent)
#define CPT_STATE_VA    (CPT_GPU_VA + 0x2000)
#define CPT_SAMPLER_OFF 0x400
#define CPT_BORDER_OFF  0x600
// v0.30.2: Metal jobs set the dynamic state base to CPT_GPU_VA. The CURBE and
// interface descriptor loads and the sampler state pointer are relative to it,
// and the border color pointer must be in its bottom 16 MB (only 24 bits are
// used, see Mesa iris_border_color.c): at 0x20002600 with base 0 the hardware
// read the border color from 0x2600 (the process's own memory).
#define CPT_DSB         CPT_GPU_VA
#define CPT_DYN(va)     ((UInt32)((va) - CPT_DSB))

// RENDER_SURFACE_STATE (Gen12, Mesa genxml gen120.xml), a linear 2D (array) surface
// v0.36.0: XeTexDesc.pad = mip levels (bits 7:0, 0 = 1) | the level a render target draws into, or for a
// sampled texture its base level (15:8); v0.37.0: bit 16 = Y-tiled (a depth texture: qpitch = rows allocated),
// bit 17 = only the base level (generateMipmaps reads level k - 1 that way).
// A texture with mips spans qpitch rows per layer (the whole mip chain, isl GFX4_2D layout)
#define TEX_TILEY (1u << 16)
#define TEX_ONELEVEL (1u << 17)
#define TEX_3D (1u << 19)                        // v0.51.8: a 3D texture: layers = depth slices (array memory layout), SURFTYPE_3D
#define TEX_CUBE (1u << 18)                      // v0.41.0: a cube (array): layers = 6 x cubes, sampled as SURFTYPE_CUBE
static inline UInt32 texLevels(const XeTexDesc &t) { return (t.pad & 0xFF) ? (t.pad & 0xFF) : 1; }
static inline UInt64 texSpan(const XeTexDesc &t) {
    return (UInt64)t.pitch * ((t.array || texLevels(t) > 1 || (t.pad & TEX_TILEY)) ? t.qpitch : t.height) * (t.layers ? t.layers : 1);
}
static inline bool texMipsOK(const XeTexDesc &t) {
    UInt32 l = texLevels(t);
    if (l > 15 || ((t.pad >> 8) & 0xFF) >= l || (t.pad >> 20)) return false;
    if ((t.pad & TEX_CUBE) && (!t.array || !t.layers || t.layers % 6 || t.width != t.height)) return false;
    if (l > 1 && (t.qpitch < t.height || (t.qpitch & 3))) return false;
    if ((t.pad & TEX_TILEY) && (l != 1 || t.array || (t.pitch & 127) || (t.qpitch & 31) || t.qpitch < t.height)) return false;
    return true;
}
// mipCount: RENDER_SURFACE_STATE MIP Count / LOD - for sampling the levels - 1, for a render target
// or a typed surface the level it uses
static void packSurfaceState(volatile UInt32 *s, const XeTexDesc &t, UInt32 mipCount = 0, UInt32 minLod = 0) {
    for (int i = 0; i < 16; i++) s[i] = 0;
    if (!t.va) { s[0] = 7u << 29; return; }                                 // SURFTYPE_NULL
    s[0] = (1u << 29) | ((t.array ? 1u : 0u) << 28) | ((t.format & 0x3FF) << 18) |
           (1u << 16) | (1u << 14);                                         // 2D, VALIGN 4, HALIGN 4, linear
    s[1] = (4u << 24) | ((t.array || texLevels(t) > 1) ? ((t.qpitch >> 2) & 0x7FFF) : 0);   // MOCS 4, QPitch
    s[2] = ((t.width - 1) & 0x3FFF) | (((t.height - 1) & 0x3FFF) << 16);
    s[3] = ((t.pitch - 1) & 0x3FFFF) | (((t.layers - 1) & 0x7FF) << 21);
    s[4] = ((t.layers - 1) & 0x7FF) << 7;                                   // render target view extent
    s[5] = (mipCount & 15) | ((minLod & 15) << 4);                          // v0.36.0: MIP count / LOD; v0.37.0: min LOD
    if (t.pad & TEX_TILEY) s[0] |= 3u << 12;                                // v0.37.0: Y-major tiled (depth textures)
    if (t.pad & TEX_CUBE) {                                                 // v0.41.0: isl: SURFTYPE_CUBE, all faces,
        UInt32 cubes = t.layers / 6;                                        // Depth = cubes - 1
        s[0] = (s[0] & ~(7u << 29)) | (3u << 29) | 0x3Fu;
        s[3] = (s[3] & 0x1FFFFF) | (((cubes - 1) & 0x7FF) << 21);
        s[4] = ((cubes - 1) & 0x7FF) << 7;
    }
    if (t.pad & TEX_3D) {                                                   // v0.51.8: SURFTYPE_3D, Depth = slices - 1, not an array
        s[0] = (s[0] & ~((7u << 29) | (1u << 28))) | (2u << 29);
        s[3] = (s[3] & 0x1FFFFF) | (((t.layers - 1) & 0x7FF) << 21);
        s[4] = 0;
    }
    s[7] = (4u << 25) | (5u << 22) | (6u << 19) | (7u << 16);              // channel selects R G B A
    s[8] = (UInt32)t.va; s[9] = (UInt32)(t.va >> 32);
}
// SAMPLER_STATE (Gen12)
static void packSamplerState(volatile UInt32 *p, const XeSampDesc &d, UInt32 borderVA) {
    p[0] = ((UInt32)(d.minFilter & 7) << 14) | ((UInt32)(d.magFilter & 7) << 17) |
           ((UInt32)(d.mipFilter & 3) << 20) | (2u << 27);                  // LOD pre-clamp: OpenGL
    p[1] = ((14u * 256u) << 8) | 1u;                                        // max LOD 14, min LOD 0; cube: OVERRIDE (v0.41.0, as anv)
    // v0.37.0: compare (sample_compare): pad[0] = MTLCompareFunction + 1. Metal gives 1 if ref <op> texel,
    // the hardware 0 if texel <op> ref: flip and negate (as Mesa's translate_shadow_func)
    if (d.pad[0]) {
        static const UInt8 shadow[8] = { 0 /*NEVER->ALWAYS*/, 4 /*LESS->LEQUAL*/, 6 /*EQUAL->NOTEQUAL*/, 2 /*LEQUAL->LESS*/,
                                         7 /*GREATER->GEQUAL*/, 3 /*NOTEQUAL->EQUAL*/, 5 /*GEQUAL->GREATER*/, 1 /*ALWAYS->NEVER*/ };
        p[1] |= (UInt32)shadow[(d.pad[0] - 1) & 7] << 1;
    }
    p[2] = borderVA & ~63u;
    UInt32 round = (d.minFilter || d.magFilter) ? (0x3Fu << 13) : 0;        // address rounding (as Mesa for linear)
    p[3] = (d.addrW & 7) | ((UInt32)(d.addrV & 7) << 3) | ((UInt32)(d.addrU & 7) << 6) |
           ((d.normalized ? 0u : 1u) << 10) | round;
}
#define MEDIA_CURBE_LOAD_CMD  0x70010002u
#define MEDIA_IDD_LOAD_CMD    0x70020002u
#define MEDIA_VFE_STATE_CMD   0x70000007u
#define GPGPU_WALKER_CMD      0x7105000du
#define MEDIA_STATE_FLUSH_CMD 0x70040000u
#define PC_TLB_INVALIDATE     (1u << 18)

int IrisXeProbe::mtlRunCompute(const XeSpaceArgs *a, UInt32 *seqOut) {
    if (!gpuUp || gpuHung || !gpuWL) return -1;
    if (!rcsOn || !rcsVA) return -6;
    UInt32 tpg = a->tpg, threads = (tpg + 7) / 8;
    // ABI 8: W x H x D thread groups (the walker runs them as W H D threads in a row)
    UInt32 tH = a->tpgHD[0] ? a->tpgHD[0] : 1, tD = a->tpgHD[1] ? a->tpgHD[1] : 1;
    if (!tpg || tpg % (tH * tD)) return -7;
    UInt32 tW = tpg / (tH * tD);
    UInt32 gridY = a->gridYZ[0] ? a->gridYZ[0] : a->groups[1] * tH, gridZ = a->gridYZ[1] ? a->gridYZ[1] : a->groups[2] * tD;
    if (gridY > a->groups[1] * tH || gridZ > a->groups[2] * tD) return -7;
    // ABI 4: a big kernel stays in the process's GPU memory (va); small ones are copied here
    bool inMem = a->va != 0;
    if (!a->count || !tpg || threads > 64 || !a->groups[0] || !a->groups[1] || !a->groups[2]) return -7;
    if (!inMem && (!a->cmds || a->count > CPT_KERNEL_MAX)) return -7;
    if (inMem && ((a->va & 63) || a->count > kXeKernelMaxDW || !mtlVAOK(a->va, (UInt64)a->count * 4))) return -7;
    if (a->ntex > kXeMaxTextures || a->nsamp > kXeMaxSamplers) return -7;
    for (UInt32 k = 0; k < a->ntex; k++) {
        const XeTexDesc &t = a->tex[k];
        if (!t.va) continue;
        if (!t.width || !t.height || !t.layers || t.width > 16384 || t.height > 16384 || t.layers > 2048 ||
            (t.pitch & 63) || t.pitch < t.width || (t.va & 0xFFF) || (t.array && (t.qpitch & 3)) || !texMipsOK(t) ||
            !mtlVAOK(t.va, texSpan(t))) return -8;
    }
    // milestone 3f: threadgroup memory and barriers (ABI 3: in len)
    UInt32 slm = (UInt32)(a->len & kXeComputeSLM);
    bool barrier = (a->len & kXeComputeBarrier) != 0;
    if (slm > 65536) return -7;
    UInt32 slmEnc = 0;                                     // 0 = none, 1 = 1K, 2 = 2K, 3 = 4K .. 7 = 64K
    if (slm) { slmEnc = 1; while ((1024u << (slmEnc - 1)) < slm) slmEnc++; }
    // a barrier waits for every hardware thread of the group: a last group
    // that is only partly inside the grid gets its own walker with just the
    // threads it needs (the others would skip the kernel and never arrive)
    UInt32 gridX = a->ctd[6];
    UInt32 lastTPG = 0;
    if (tH == 1 && tD == 1) {
        if (barrier && gridX && gridX < a->groups[0] * tpg && gridX > (a->groups[0] - 1) * tpg)
            lastTPG = gridX - (a->groups[0] - 1) * tpg;
    } else if (barrier && (gridX < a->groups[0] * tW || gridY < a->groups[1] * tH || gridZ < a->groups[2] * tD)) {
        // ABI 8: a 2D / 3D group partly outside the grid: its outside threads skip the kernel and would
        // never reach the barrier (a GPU hang) - not supported yet
        return -16;
    }
    // our pages: kernel + IDD, CURBE (1 cross-thread register + 1 per thread)
    volatile UInt32 *kp = rcsVA + R_CPT_KERNEL * 1024;
    volatile UInt32 *dp = rcsVA + R_CPT_DATA * 1024;
    if (!inMem) for (UInt32 k = 0; k < CPT_KERNEL_MAX; k++) kp[k] = k < a->count ? a->cmds[k] : 0;
    volatile UInt32 *idd = kp + CPT_IDD_OFF / 4;
    idd[0] = inMem ? (UInt32)a->va : (UInt32)CPT_GPU_VA;   // kernel start (instruction base 0)
    if (inMem) mtlComputeBig++;
    idd[1] = 0; idd[2] = 0; idd[3] = 0; idd[4] = 0;
    idd[5] = 1u << 16;                           // per-thread constant data: 1 register
    idd[6] = threads | (slmEnc << 16) | (barrier ? 1u << 21 : 0);   // threads in a group, SLM, barrier
    idd[7] = 1;                                  // cross-thread constant data: 1 register
    if (a->ntex || a->nsamp) {                   // milestone 4: textures / samplers
        volatile UInt32 *sp = rcsVA + R_CPT_STATE * 1024;
        for (UInt32 k = 0; k < kXeMaxTextures; k++) {
            XeTexDesc none = {};
            packSurfaceState(sp + (0x40 * (1 + k)) / 4, k < a->ntex ? a->tex[k] : none);
            sp[k] = 0x40 * (1 + k);              // binding table entry: offset from the surface state base
        }
        for (UInt32 k = 0; k < kXeMaxSamplers; k++) {
            XeSampDesc s = k < a->nsamp ? a->samp[k] : XeSampDesc{};
            packSamplerState(sp + (CPT_SAMPLER_OFF + 16 * k) / 4, s, CPT_DYN(CPT_STATE_VA + CPT_BORDER_OFF));
        }
        for (UInt32 k = 0; k < 16; k++) sp[CPT_BORDER_OFF / 4 + k] = 0;
        idd[3] = (CPT_DYN(CPT_STATE_VA + CPT_SAMPLER_OFF) & ~31u) | ((a->nsamp ? (a->nsamp + 3) / 4 : 0) << 2);
        idd[4] = a->ntex & 31;                   // binding table at offset 0, prefetch count
        mtlComputeTex++;
    }
    UInt32 lastThreads = lastTPG ? (lastTPG + 7) / 8 : 0;
    if (lastTPG) {                               // descriptor 1: the partial last group
        for (UInt32 k = 0; k < 8; k++) idd[8 + k] = idd[k];
        idd[8 + 6] = lastThreads | (slmEnc << 16) | (1u << 21);
    }
    for (UInt32 k = 0; k < 8; k++) dp[k] = a->ctd[k];
    for (UInt32 t = 0; t < threads; t++) {
        dp[(1 + t) * 8] = t * 8;
        dp[(1 + t) * 8 + 1] = tW; dp[(1 + t) * 8 + 2] = tH;            // ABI 8: per-thread data g2.1..g2.5
        dp[(1 + t) * 8 + 3] = gridY; dp[(1 + t) * 8 + 4] = gridZ;
        dp[(1 + t) * 8 + 5] = tD;
        for (UInt32 k = 6; k < 8; k++) dp[(1 + t) * 8 + k] = 0;
    }
    UInt32 curbeRegs = 1 + threads;
    UInt32 curbeBytes = ((curbeRegs * 32) + 63) & ~63u;
    UInt32 lanesLast = tpg - (threads - 1) * 8;
    UInt32 rightMask = lanesLast >= 32 ? 0xFFFFFFFFu : ((1u << lanesLast) - 1);
    __asm__ volatile("mfence" ::: "memory");

    UInt32 c[128]; UInt32 n = 0;
    // new page-table entries: TLB invalidate (with CS stall + post-sync write, as i915)
    c[n++] = GFX_OP_PIPE_CONTROL(6);
    // + instruction / state / constant cache invalidate: the kernel and its
    // constants are rewritten at the same addresses for every job
    c[n++] = PC_CS_STALL | PC_QW_WRITE | PC_GLOBAL_GTT | PC_TLB_INVALIDATE |
             (1u << 11) | (1u << 10) | (1u << 3) | (1u << 2);       // + texture cache (milestone 4)
    c[n++] = R_GGTT_ADDR + R_HWSP * 4096 + 0x80 * 4; c[n++] = 0; c[n++] = 0; c[n++] = 0;
    UInt32 sba = n + 13;                                            // STATE_BASE_ADDRESS starts at kComputeCmds[13]
    for (UInt32 k = 0; k < 35; k++) c[n++] = kComputeCmds[k];       // PC, PIPELINE_SELECT GPGPU, PC, STATE_BASE_ADDRESS
    c[sba + 4] = (UInt32)CPT_STATE_VA | 0x41;                       // surface state base (modify, MOCS 4)
    c[sba + 5] = (UInt32)(CPT_STATE_VA >> 32);
    c[sba + 6] = (UInt32)CPT_DSB | 0x41;                            // dynamic state base (v0.30.2)
    c[sba + 7] = (UInt32)(CPT_DSB >> 32);
    c[n++] = MEDIA_VFE_STATE_CMD; c[n++] = 0; c[n++] = 0;
    c[n++] = (2u << 8) | (64u << 16);           // 2 URB entries, 64 threads max
    c[n++] = 0;
    c[n++] = ((curbeRegs + 1) & ~1u) | (2u << 16);   // CURBE allocation, URB entry size
    c[n++] = 0; c[n++] = 0; c[n++] = 0;
    for (UInt32 k = 44; k < 50; k++) c[n++] = kComputeCmds[k];      // PC
    c[n++] = MEDIA_CURBE_LOAD_CMD; c[n++] = 0; c[n++] = curbeBytes; c[n++] = CPT_DYN(CPT_CURBE_VA);
    c[n++] = MEDIA_IDD_LOAD_CMD; c[n++] = 0; c[n++] = lastTPG ? 64 : 32; c[n++] = CPT_DYN(CPT_GPU_VA + CPT_IDD_OFF);
    if (!lastTPG || a->groups[0] > 1) {
        c[n++] = GPGPU_WALKER_CMD; c[n++] = 0; c[n++] = 0; c[n++] = 0;
        c[n++] = (threads - 1);                  // thread width max, SIMD8
        c[n++] = 0; c[n++] = 0; c[n++] = lastTPG ? a->groups[0] - 1 : a->groups[0];
        c[n++] = 0; c[n++] = 0; c[n++] = a->groups[1];
        c[n++] = 0; c[n++] = a->groups[2];
        c[n++] = rightMask; c[n++] = 0xFFFFFFFFu;
    }
    if (lastTPG) {                               // the last group (X = groups - 1) with descriptor 1
        UInt32 ll = lastTPG - (lastThreads - 1) * 8;
        c[n++] = GPGPU_WALKER_CMD; c[n++] = 1; c[n++] = 0; c[n++] = 0;
        c[n++] = (lastThreads - 1);
        c[n++] = a->groups[0] - 1; c[n++] = 0; c[n++] = a->groups[0];
        c[n++] = 0; c[n++] = 0; c[n++] = a->groups[1];
        c[n++] = 0; c[n++] = a->groups[2];
        c[n++] = ll >= 32 ? 0xFFFFFFFFu : ((1u << ll) - 1); c[n++] = 0xFFFFFFFFu;
        mtlComputeSplit++;
    }
    if (barrier) mtlComputeBarrier++;
    c[n++] = MEDIA_STATE_FLUSH_CMD; c[n++] = 0;
    for (UInt32 k = 71; k < 77; k++) c[n++] = kComputeCmds[k];      // PC (HDC flush, CS stall)
    c[n++] = GFX_OP_PIPE_CONTROL(6) | (1u << 9);                    // v0.49.1: + L3 / DC flush (L3 caching on), HDC pipeline flush
    c[n++] = PC_CS_STALL | (1u << 27) | (1u << 5) | (1u << 7);
    c[n++] = 0; c[n++] = 0; c[n++] = 0; c[n++] = 0;

    UInt32 seq = 0, waited = 0, timeoutMS = a->timeoutMS ? a->timeoutMS : 2000;
    UInt32 resets0 = gpuResets;                                  // v0.44.0
    uint64_t t0_; clock_get_uptime(&t0_);
    for (;;) {
        IOReturn r = gpuWL->runAction(&IrisXeProbe::mtlTrySubmitRenderGated, this, c, (void *)(uintptr_t)n, &seq);
        if (r == kIOReturnSuccess) break;
        if (r != kIOReturnBusy || waited >= timeoutMS) { mtlComputeFails++; return -2; }
        waited = gpuWait(false, 0, t0_);          // v0.45.0: wake on the next job end
    }
    if (seqOut) *seqOut = seq;
    while ((SInt32)(rcsSeqDone - seq) < 0) {
        if (waited >= timeoutMS) { mtlComputeFails++; return -3; }
        waited = gpuWait(true, seq, t0_);          // v0.45.0: woken by the job-end interrupt
    }
    if (gpuResets != resets0) { mtlComputeFails++; return -9; }
    mtlComputeJobs++;
    gpuStatus(nullptr);
    return 0;
}

// ---- milestone 5a: first 3D draw ---------------------------------------------
// One job on the render engine: the 3D pipeline with the vertex shader off,
// a rectangle (RECTLIST, as Mesa blorp draws) and a triangle (TRILIST), both
// in a constant colour from our pixel shader, into a 64x64 RGBA8 linear render
// target. Then the CPU checks every pixel. Called with mtlJobLock held.
#define DRAW_COLOR 0xFF4080FFu                    // RGBA8 (1.0, 0.5, 0.25, 1.0)
static bool drawColorOK(UInt32 p) {
    int g = (int)((p >> 8) & 0xFF), b = (int)((p >> 16) & 0xFF);
    return (p & 0xFF0000FFu) == 0xFF0000FFu && g >= 0x7F && g <= 0x81 && b >= 0x3F && b <= 0x41;
}

// one draw job: clear the render target, run the commands, wait (mtlJobLock held)
int IrisXeProbe::mtlDrawJob(const UInt32 *cmds, UInt32 count, UInt32 *ms) {
    volatile UInt32 *rt = rcsVA + R_RND_RT * 1024;
    for (UInt32 k = 0; k < 4 * 1024; k++) rt[k] = 0;
    __asm__ volatile("mfence" ::: "memory");
    UInt32 seq = 0, waited = 0;
    const UInt32 timeoutMS = 2000;
    for (;;) {
        IOReturn r = gpuWL->runAction(&IrisXeProbe::mtlTrySubmitRenderGated, this, (void *)cmds,
                                      (void *)(uintptr_t)count, &seq);
        if (r == kIOReturnSuccess) break;
        if (r != kIOReturnBusy || waited >= timeoutMS) { *ms = waited; return -2; }
        IOSleep(1); waited++;
    }
    mtlLastSeq = seq; mtlLastRender = true; mtlLastDone = false;
    while ((SInt32)(rcsSeqDone - seq) < 0) {
        if (waited >= timeoutMS || gpuHung) { *ms = waited; return -3; }
        IOSleep(1); waited++;
    }
    mtlLastDone = true;
    *ms = waited;
    return 0;
}

static int absDiff(int a, int b) { return a > b ? a - b : b - a; }

int IrisXeProbe::mtlRunDraw(char *out, size_t outLen) {
    if (!gpuUp || gpuHung || !gpuWL) { snprintf(out, outLen, "not run: GPU %s", gpuHung ? "hung" : "down"); return -1; }
    if (!rcsOn || !rcsVA) { snprintf(out, outLen, "not run: render engine off"); return -6; }
    volatile UInt32 *rt = rcsVA + R_RND_RT * 1024;
    const UInt32 pitch = RENDER_RT_PITCH / 4;
    drawRuns++;
    UInt32 ms = 0;
    size_t o = 0;

    // draw A (5a): no vertex shader, constant colour. Rectangle x,y in [4,28) =
    // 576 pixels; triangle (36,36) (60,36) (36,60): 276 inside + up to 24 on the long edge
    int r = mtlDrawJob(kRenderCmds, sizeof(kRenderCmds) / 4, &ms);
    if (r) { snprintf(out, outLen, "A: %s after %u ms (GPU %s)", r == -2 ? "SUBMIT FAILED" : "TIMEOUT", ms, gpuHung ? "hung" : "busy"); return r; }
    UInt32 rect = 0, tri = 0, stray = 0, odd = 0, oddVal = 0;
    for (UInt32 y = 0; y < RENDER_RT_H; y++)
        for (UInt32 x = 0; x < RENDER_RT_W; x++) {
            UInt32 p = rt[y * pitch + x];
            if (!p) continue;
            if (!drawColorOK(p)) { if (!odd) oddVal = p; odd++; continue; }
            if (x >= 4 && x < 28 && y >= 4 && y < 28) rect++;
            else if (x >= 36 && y >= 36 && x + y <= 95) tri++;
            else stray++;
        }
    bool okA = rect == 576 && tri >= 276 && tri <= 300 && !stray && !odd;
    o += snprintf(out + o, outLen - o, "A %s: rect %u/576, tri %u, stray %u, odd %u (%08x), %u ms",
                  okA ? "DRAWN" : "WRONG", rect, tri, stray, odd, oddVal, ms);

    // draw B (5b): vertex shader, clipper + viewport, perspective-correct colours
    r = mtlDrawJob(kRender2Cmds, sizeof(kRender2Cmds) / 4, &ms);
    if (r) {
        snprintf(out + o, outLen - o, " | B: %s after %u ms (GPU %s)", r == -2 ? "SUBMIT FAILED" : "TIMEOUT", ms, gpuHung ? "hung" : "busy");
        return r;
    }
    UInt32 good = 0, bad = 0, outside = 0, badX = 0, badY = 0, badGot = 0, badWant = 0, maxd = 0;
    for (UInt32 y = 0; y < RENDER_RT_H; y++)
        for (UInt32 x = 0; x < RENDER_RT_W; x++) {
            UInt32 want = kRender2Expect[y * RENDER_RT_W + x], got = rt[y * pitch + x];
            if (want == 0xFFFFFFFFu) continue;
            bool ok;
            if (!want) { ok = !got; if (!ok) outside++; }
            else {
                UInt32 d = 0;
                for (UInt32 k = 0; k < 4; k++) {
                    UInt32 e = (UInt32)absDiff((int)((got >> (8 * k)) & 0xFF), (int)((want >> (8 * k)) & 0xFF));
                    if (e > d) d = e;
                }
                if (d > maxd && d < 256) maxd = d;
                ok = d <= 2;
                if (ok) good++;
            }
            if (!ok) { if (!bad) { badX = x; badY = y; badGot = got; badWant = want; } bad++; }
        }
    bool okB = good == RENDER2_INSIDE && !bad;
    o += snprintf(out + o, outLen - o, " | B %s: %u/%u px right (max diff %u), wrong %u (outside %u) first (%u,%u) %08x want %08x, (32,40) %08x, %u ms",
                  okB ? "DRAWN" : "WRONG", good, RENDER2_INSIDE, maxd, bad, outside, badX, badY, badGot, badWant,
                  rt[40 * pitch + 32], ms);
    bool ok = okA && okB;
    if (ok) drawOK++;
    snprintf(out + o, outLen - o, " | runs %u, ok %u", drawRuns, drawOK);
    return ok ? 0 : -4;
}

// milestone 5b: draw C - draw B's triangle with shaders from our compiler (IrisXeDrawTest.h)
#define DRAWC_VS_VA  (RENDER_PS_VA + 0xC00)       // render page 17, last KB
#define DRAWC_PS_VA  (RENDER_SURF_VA + 0x800)     // render page 19, second half
static_assert(sizeof(kRenderSurf) <= 0x800, "surface page layout");
static void r3or(UInt32 *c, UInt32 dw, UInt32 shift, UInt32 v) { c[dw] |= v << shift; }
#define R3(c, f, v) do { UInt32 p_[2] = { R3_##f }; r3or(c, p_[0], p_[1], (v)); } while (0)

int IrisXeProbe::mtlRunDrawC(const XeDrawTestCfg *g, char *out, size_t outLen) {
    if (!gpuUp || gpuHung || !gpuWL) { snprintf(out, outLen, "not run: GPU %s", gpuHung ? "hung" : "down"); return -1; }
    if (!rcsOn || !rcsVA) { snprintf(out, outLen, "not run: render engine off"); return -6; }
    if (g->version != XE_DRAWTEST_VERSION || !g->vsDW || g->vsDW > 256 || !g->psDW || g->psDW > 512 ||
        g->vsGrf > 31 || g->vsReadLen > 63 || g->psGrf > 31 || g->numAttr > 16 || g->sbeReadLen > 31 ||
        g->baryMode > 0x3F) { snprintf(out, outLen, "bad configuration"); return -7; }
    for (UInt32 k = 0; k < g->numAttr; k++) if (g->swiz[k] > 31) { snprintf(out, outLen, "bad swizzle"); return -7; }
    volatile UInt32 *vs = rcsVA + R_RND_PS * 1024 + 0xC00 / 4, *ps = rcsVA + R_RND_SURF * 1024 + 0x800 / 4;
    for (UInt32 k = 0; k < 256; k++) vs[k] = k < g->vsDW ? g->vs[k] : 0;     // zeros after: instruction prefetch
    for (UInt32 k = 0; k < 512; k++) ps[k] = k < g->psDW ? g->ps[k] : 0;
    static UInt32 c[sizeof(kRender3Cmds) / 4];                               // mtlJobLock held
    for (UInt32 k = 0; k < sizeof(kRender3Cmds) / 4; k++) c[k] = kRender3Cmds[k];
    R3(c, VS_KSP, DRAWC_VS_VA >> 6);
    R3(c, VS_GRF, g->vsGrf);
    R3(c, VS_READLEN, g->vsReadLen);
    R3(c, CLIP_NONPERSP, g->nonPersp ? 1 : 0);
    R3(c, SBE_NUMATTR, g->numAttr);
    R3(c, SBE_READLEN, g->sbeReadLen);
    R3(c, SBE_CONSTINTERP, g->constInterp);
    {
        UInt32 p0[2] = { R3_SWIZ_SRC0 };
        for (UInt32 k = 0; k < g->numAttr; k++) r3or(c, p0[0] + k / 2, p0[1] + 16 * (k % 2), g->swiz[k]);
    }
    R3(c, WM_BARY, g->baryMode);
    R3(c, WM_FORCE, g->kill ? 2 : 0);                                        // 2 = ForceON (1 is ForceOff)
    R3(c, PS_KSP, DRAWC_PS_VA >> 6);
    R3(c, PS_GRF, g->psGrf);
    R3(c, PSX_ATTR, g->numAttr ? 1 : 0);
    R3(c, PSX_KILL, g->kill ? 1 : 0);
    drawCRuns++;
    UInt32 ms = 0;
    int r = mtlDrawJob(c, sizeof(kRender3Cmds) / 4, &ms);
    if (r) { snprintf(out, outLen, "C: %s after %u ms (GPU %s)", r == -2 ? "SUBMIT FAILED" : "TIMEOUT", ms, gpuHung ? "hung" : "busy"); return r; }
    snprintf(out, outLen, "C DRAWN in %u ms (runs %u): pixels in IrisXe-DrawPixels", ms, drawCRuns);
    return 0;
}

static UInt64 mixTicks[5], mixPix[5]; static UInt32 mixJobs[5], mixN;     // v0.58.2/3: IrisXe-DrawMix
IOReturn IrisXeProbe::setProperties(OSObject *properties) {
    OSDictionary *d = OSDynamicCast(OSDictionary, properties);
    if (!d) return kIOReturnUnsupported;
    if (!mtlJobLock) return kIOReturnNotReady;
    OSData *sh = OSDynamicCast(OSData, d->getObject("IrisXe-DrawShaders"));
    if (sh) {                                                // milestone 5b: draw C
        if (sh->getLength() != sizeof(XeDrawTestCfg)) return kIOReturnBadArgument;
        XeDrawTestCfg *g = (XeDrawTestCfg *)IOMalloc(sizeof(XeDrawTestCfg));
        if (!g) return kIOReturnNoMemory;
        memcpy(g, sh->getBytesNoCopy(), sizeof(XeDrawTestCfg));
        char r[160];
        IOLockLock(mtlJobLock); mtlFinishAsync();
        int rc = mtlRunDrawC(g, r, sizeof(r));
        OSData *px = rc == 0 ? OSData::withBytes((const void *)(rcsVA + R_RND_RT * 1024), RENDER_RT_PITCH * RENDER_RT_H) : nullptr;
        IOLockUnlock(mtlJobLock);
        IOFree(g, sizeof(XeDrawTestCfg));
        setProperty("IrisXe-DrawC", r);
        if (px) { setProperty("IrisXe-DrawPixels", px); px->release(); }
        else removeProperty("IrisXe-DrawPixels");
        gpuStatus(nullptr);
        return kIOReturnSuccess;
    }
    if (OSNumber *bc = OSDynamicCast(OSNumber, d->getObject("IrisXe-BatchCfg"))) {   // v0.49.3: draws per job | 0x100 = no barrier
        UInt32 v = bc->unsigned32BitValue();
        mtlBatchSlots = (v & 0xFF) ? (v & 0xFF) : 4;   // v0.57.0: 0 = the default (4)
        mtlBatchBarrier = !(v & 0x100);
        char t[96];
        snprintf(t, sizeof(t), "%u draws per job, %s between draws", mtlBatchSlots, mtlBatchBarrier ? "flush + invalidate" : "nothing");
        setProperty("IrisXe-BatchCfg-Now", t);
        return kIOReturnSuccess;
    }
    if (d->getObject("IrisXe-DrawMixReset")) {             // v0.58.3: start IrisXe-DrawMix from zero (irisxe-metaltest --mixreset)
        if (mtlJobLock) { IOLockLock(mtlJobLock); mtlFinishAsync(); }
        for (int i = 0; i < 5; i++) { mixTicks[i] = mixPix[i] = 0; mixJobs[i] = 0; }
        mixN = 0;
        if (mtlJobLock) IOLockUnlock(mtlJobLock);
        setProperty("IrisXe-DrawMix", "reset: nothing drawn since");
        return kIOReturnSuccess;
    }
    if (OSNumber *hc = OSDynamicCast(OSNumber, d->getObject("IrisXe-HashCfg"))) {    // v0.57.8: 0 off, 1 on
        if (mtlJobLock) { IOLockLock(mtlJobLock); mtlFinishAsync(); }
        gpuHashOn = gpuHashOK && hc->unsigned32BitValue() != 0;
        if (mtlJobLock) IOLockUnlock(mtlJobLock);
        gpuHashPublish();
        return kIOReturnSuccess;
    }
    if (OSNumber *fq = OSDynamicCast(OSNumber, d->getObject("IrisXe-SetFreq"))) {   // v0.45.1: MHz (0 = back to auto)
        UInt32 mhz = fq->unsigned32BitValue(), u = mhz * 3 / 50;
        if (mhz && gpuFreqRP0) { if (u > gpuFreqRP0) u = gpuFreqRP0; if (u < gpuFreqRPn) u = gpuFreqRPn; }
        gpuFreqReq = mhz ? u : 0;
        if (gpuWL) gpuWL->runAction(OSMemberFunctionCast(IOWorkLoop::Action, this, &IrisXeProbe::gpuFreqSetGated), this);
        return kIOReturnSuccess;
    }
    if (d->getObject("IrisXe-HangTest")) {                      // v0.44.0: hang the blitter on purpose (reset test)
        static const UInt32 c[] = {
            (0x1Cu << 23) | (1u << 22) | (1u << 15) | (4u << 12) | 2,   // MI_SEMAPHORE_WAIT GGTT, poll, "==" , 4 dwords
            0xDEADBEEF,                                                 // never there
            G_GGTT_ADDR + G_HWSP * 4096 + 0x90 * 4, 0,
            MI_NOOP, MI_NOOP };
        IOLockLock(mtlJobLock); mtlFinishAsync();
        UInt32 seq = 0;
        int rc = mtlRunBlit(c, sizeof(c) / 4, 3000, &seq);
        IOLockUnlock(mtlJobLock);
        char r[96];
        snprintf(r, sizeof(r), "hang test job: %d, resets %u, GPU %s", rc, gpuResets, gpuHung ? "HUNG" : "running");
        setProperty("IrisXe-HangTestResult", r);
        return kIOReturnSuccess;
    }
    if (!d->getObject("IrisXe-DrawTest")) return kIOReturnUnsupported;
    char r[400];
    IOLockLock(mtlJobLock); mtlFinishAsync();
    mtlRunDraw(r, sizeof(r));
    IOLockUnlock(mtlJobLock);
    setProperty("IrisXe-Draw", r);
    gpuStatus(nullptr);
    return kIOReturnSuccess;
}

// ---- milestone 5c: a Metal draw (IrisXeDrawABI.h) -----------------------------
// The bundle describes the draw; we check it and build the commands from the
// draw D / clear templates (IrisXeRender.inc, made by tools/shadergen/render.c
// with Mesa's genxml packer): RD_x / RC_x = { dword, shift } of a field that is
// 0 in the template. State goes into our draw pages (DRW_VA): dynamic state at
// +0x1000 (relative to the dynamic state base 0x20000000), surface state +0x2000.
static_assert(sizeof(XeDrawTex) == sizeof(XeTexDesc) && sizeof(XeDrawSamp) == sizeof(XeSampDesc), "draw ABI");
static inline void dpatch(UInt32 *c, UInt32 base, UInt32 dw, UInt32 shift, UInt32 v) { c[base + dw] |= v << shift; }
#define DP(c, base, F, v)  do { UInt32 p_[2] = { F }; dpatch(c, base, p_[0], p_[1], (v)); } while (0)
// (sDrawBuf: declared near the top, v0.42.4)
static UInt32 sDrawTmpl[sizeof(kDrawCmds) / 4];

// v0.56.8: in a job with several draws (batching), the 2nd and later templates no longer re-send the job-wide setup:
// PIPELINE_SELECT, the L3 configuration (LRI 0xb134), the default STATE_BASE_ADDRESS, the push constant allocation
// and CS_DEBUG_MODE2 (LRI 0x20d8). Their values never change between draws; Mesa programs them once per batch, and
// re-sending them between draws in one job is the prime suspect for the desktop hangs with batching (v0.51.37,
// v0.56.7 test: the job's last PIPE_CONTROL waits for the 3D pipeline forever). The PIPE_CONTROLs around them stay.
// Template dwords 0..84 are the same in the draw and the clear template (checked at start: mtlBatchTrimOK).
static bool sJobHasTemplate;                     // a template is already in the job being built
static UInt32 sTrimmed;
static const UInt8 kTrim[][2] = { { 6, 1 }, { 13, 3 }, { 22, 22 }, { 66, 10 }, { 76, 3 } };   // { first dword, count }
static bool mtlBatchTrimOK() {
    static const UInt32 hdr[] = { 0x69041310, 0x11000001, 0x61010014, 0x79120000, 0x11000001 };
    for (UInt32 k = 0; k < 5; k++)
        if (kDrawCmds[kTrim[k][0]] != hdr[k] || kClearCmds[kTrim[k][0]] != hdr[k]) return false;
    return kDrawCmds[14] == 0xb134 && kDrawCmds[77] == 0x20d8 && kClearCmds[14] == 0xb134 && kClearCmds[77] == 0x20d8 &&
           (kDrawCmds[74] >> 16) == 0x7916 && (kClearCmds[74] >> 16) == 0x7916;
}
static void trimRepeat(UInt32 *c) {
    if (sJobHasTemplate && sBatchTrim) {
        for (UInt32 k = 0; k < 5; k++) for (UInt32 j = 0; j < kTrim[k][1]; j++) c[kTrim[k][0] + j] = 0;   // MI_NOOP
        sTrimmed++;
    }
    sJobHasTemplate = true;
}

// v0.34.0: the depth / stencil buffer packets of a draw (PFX RD) or a clear (RC) template;
// writes: depth write / stencil write enable
#define DS_BUFFERS(c, PFX, ds, dwrite, swrite) do {                                                      \
    if ((ds).depthVA) {                                                                                  \
        DP(c, 0, PFX##_DB_TYPE, GEN_SURFTYPE_2D);                                                        \
        DP(c, 0, PFX##_DB_FMT, (ds).depthFormat == 250 ? GEN_D16_UNORM : GEN_D32_FLOAT);                \
        if (dwrite) DP(c, 0, PFX##_DB_WRITE, 1);                                                         \
        DP(c, 0, PFX##_DB_PITCH, (ds).depthPitch - 1);                                                   \
        { UInt32 p_[2] = { PFX##_DB_ADDR }; c[p_[0]] = (UInt32)(ds).depthVA; c[p_[0] + 1] = (UInt32)((ds).depthVA >> 32); } \
        DP(c, 0, PFX##_DB_W, (ds).width - 1);                                                            \
        DP(c, 0, PFX##_DB_H, (ds).height - 1);                                                           \
        DP(c, 0, PFX##_DB_QPITCH, (ds).depthRows >> 2);                                                  \
    } else { DP(c, 0, PFX##_DB_TYPE, GEN_SURFTYPE_NULL); DP(c, 0, PFX##_DB_FMT, GEN_D32_FLOAT); }  /* as isl */ \
    if ((ds).stencilVA) {                                                                                \
        DP(c, 0, PFX##_SB_TYPE, GEN_SURFTYPE_2D);                                                        \
        if (swrite) DP(c, 0, PFX##_SB_WRITE, 1);                                                         \
        DP(c, 0, PFX##_SB_PITCH, (ds).stencilPitch - 1);                                                 \
        { UInt32 p_[2] = { PFX##_SB_ADDR }; c[p_[0]] = (UInt32)(ds).stencilVA; c[p_[0] + 1] = (UInt32)((ds).stencilVA >> 32); } \
        DP(c, 0, PFX##_SB_W, (ds).width - 1);                                                            \
        DP(c, 0, PFX##_SB_H, (ds).height - 1);                                                           \
        DP(c, 0, PFX##_SB_QPITCH, (ds).stencilRows >> 2);                                                \
    } else DP(c, 0, PFX##_SB_TYPE, GEN_SURFTYPE_NULL);                                                   \
} while (0)

// v0.49.0: batching. Several draws go into one GPU job; each draw gets its own copy of the draw state pages
// (a "slot", laid out as DRW_VA). The templates use offsets from the surface / dynamic state bases (their
// STATE_BASE_ADDRESS, re-sent for every draw), so moving both bases by the slot's distance from DRW_VA moves
// all of the draw's state; only absolute addresses are moved by hand (below). One draw alone: DRW_VA itself.
bool IrisXeProbe::mtlBatchTemplatesOK() {
    return kDrawCmds[TPL_SBA_SURF] == ((DRW_VA + DRW_SURF_OFF) | 0x41) && kDrawCmds[TPL_SBA_DYN] == ((UInt32)CPT_DSB | 0x41) &&
           kClearCmds[TPL_SBA_SURF] == ((DRW_VA + DRW_SURF_OFF) | 0x41) && kClearCmds[TPL_SBA_DYN] == ((UInt32)CPT_DSB | 0x41) &&
           kClearCmds[TPL_CLR_VB0] == DRW_VA + 0x400 && kClearCmds[TPL_CLR_VB1] == DRW_VA + 0x440;
}

int IrisXeProbe::mtlCheckDraw(const XeDrawDesc *d) {
    if (!d || d->version != XE_DRAW_VERSION || d->size != sizeof(XeDrawDesc)) return -8;
    auto inSpace = [this](UInt64 va, UInt64 len) { return va && mtlVAOK(va, len); };
    bool draw = d->vertexCount && d->instanceCount;
    // ---- checks
    if (d->nrt > XE_DRAW_MAX_RT || !d->width || !d->height || d->width > 16384 || d->height > 16384) return -8;
    for (UInt32 k = 0; k < d->nrt; k++) {
        const XeDrawTex &t = d->rt[k];
        if (!t.va) continue;
        if ((t.va & 0xFFF) || !t.width || !t.height || t.width > 16384 || t.height > 16384 || (t.pitch & 63) ||
            t.pitch < t.width || t.format >= 0x200 || t.array || !texMipsOK(*(const XeTexDesc *)&t) || (t.pad & (TEX_ONELEVEL | TEX_CUBE | TEX_3D)) ||   // v0.57.7: Y-tiled render targets allowed
            !inSpace(t.va, texSpan(*(const XeTexDesc *)&t))) return -8;
    }
    if (d->clearMask >> d->nrt) return -8;
    {   // v0.34.0: depth / stencil
        const XeDrawDS &z = d->ds;
        if (z.psSrcDepth > 3 || z.clearDepth > 1 || z.clearStencil > 1 || z.depthWrite > 1 || z.depthFunc > 7 || z.depthClamp > 1 ||
            z.psDepth > 1 || z.frontRef > 255 || z.backRef > 255 || z.stencilClearValue > 255) return -8;
        const XeDrawStencil *f2[2] = { &z.front, &z.back };
        for (int i = 0; i < 2; i++)
            if (f2[i]->func > 7 || f2[i]->fail > 7 || f2[i]->depthFail > 7 || f2[i]->pass > 7 ||
                f2[i]->readMask > 255 || f2[i]->writeMask > 255) return -8;
        if (z.depthVA || z.stencilVA) {
            if (!z.width || !z.height || z.width > 16384 || z.height > 16384 || d->width > z.width || d->height > z.height) return -8;
        }
        if (z.depthVA && ((z.depthVA & 0xFFF) || !z.depthPitch || (z.depthPitch & 127) || (z.depthRows & 31) ||
                          z.depthRows < z.height || z.depthSize < (UInt64)z.depthPitch * z.depthRows ||
                          (UInt64)z.depthPitch < (UInt64)z.width * (z.depthFormat == 250 ? 2 : 4) ||
                          (z.depthFormat != 250 && z.depthFormat != 252 && z.depthFormat != 260) || !inSpace(z.depthVA, z.depthSize))) return -8;
        // Gen12 stencil is Y-tiled like depth (v0.34.0 laid it out W-tiled, half the size the GPU expects: hang)
        if (z.stencilVA && ((z.stencilVA & 0xFFF) || !z.stencilPitch || (z.stencilPitch & 127) || (z.stencilRows & 31) ||
                            z.stencilRows < z.height || z.stencilPitch < z.width ||
                            z.stencilSize < (UInt64)z.stencilPitch * z.stencilRows || !inSpace(z.stencilVA, z.stencilSize))) return -8;
        if ((z.clearDepth && !z.depthVA) || (z.clearStencil && !z.stencilVA)) return -8;
    }
    if (draw) {
        if ((d->vsVA & 63) || (d->psVA & 63) || !d->vsDW || !d->psDW || d->vsDW > kXeKernelMaxDW || d->psDW > kXeKernelMaxDW ||
            !inSpace(d->vsVA, d->vsDW * 4ULL + 256) || !inSpace(d->psVA, d->psDW * 4ULL + 256)) return -8;
        if (d->vsGrf > 31 || d->vsReadLen > 63 || d->vsPush > 2 || d->psGrf > 31 || d->psPush > 2 || d->numAttr > 16 ||
            d->sbeReadLen > 31 || d->baryMode > 0x3F || d->nvb > XE_DRAW_MAX_VB || d->nve > XE_DRAW_MAX_VE ||
            d->ntex > XE_DRAW_MAX_FTEX || d->nsamp > XE_DRAW_MAX_SAMP || d->cull < 1 || d->cull > 3 ||
            d->topology < 1 || d->topology > 5 || d->vertexCount > (1u << 30) || d->instanceCount > (1u << 20)) return -8;
        for (UInt32 k = 0; k < d->numAttr; k++) if (d->swiz[k] > 31) return -8;
        for (UInt32 k = 0; k < d->nvb; k++)
            if (d->vb[k].pitch > 2048 || (d->vb[k].size && !inSpace(d->vb[k].va, d->vb[k].size))) return -8;
        for (UInt32 k = 0; k < d->nve; k++) {
            const XeDrawVE &e = d->ve[k];
            if ((d->nvb && e.vb >= d->nvb) || e.format >= 0x200 || e.offset > 2047 || e.stepRate > 0xFFFF) return -8;
            for (UInt32 c = 0; c < 4; c++) if (((e.ctrl >> (4 * c)) & 15) > 4) return -8;
        }
        if (d->sysValues && d->sysSlot >= d->nve) return -8;
        for (UInt32 k = 0; k < XE_DRAW_MAX_RT; k++) {             // v0.33.0: blending (Metal enums)
            const XeDrawBlend &b = d->blend[k];
            if (b.enable > 1 || b.src > 14 || b.dst > 14 || b.srcA > 14 || b.dstA > 14 || b.op > 4 || b.opA > 4 || b.writeMask > 15) return -8;
        }
        if (d->scissor[0] > 16384 || d->scissor[1] > 16384 || d->scissor[2] > 16384 || d->scissor[3] > 16384) return -8;
        if (d->ibType > 2 || d->pad) return -8;
        if (d->ibType) {
            UInt32 isz = d->ibType == 1 ? 2 : 4;
            if (!d->ibSize || (d->ibVA & (isz - 1)) || !inSpace(d->ibVA, d->ibSize) || d->vertexStart) return -8;
        }
        if (d->nvtex > XE_DRAW_MAX_TEX || d->nvsamp > XE_DRAW_MAX_SAMP) return -8;
        for (UInt32 k = 0; k < d->ntex + d->nvtex; k++) {           // v0.47.0: + vertex textures
            const XeDrawTex &t = k < d->ntex ? d->tex[k] : d->vtex[k - d->ntex];
            if (!t.va) continue;
            if (!t.width || !t.height || !t.layers || t.width > 16384 || t.height > 16384 || t.layers > 2048 ||
                (t.pitch & 63) || t.pitch < t.width || (t.va & 0xFFF) || (t.array && (t.qpitch & 3)) ||
                !texMipsOK(*(const XeTexDesc *)&t) || !inSpace(t.va, texSpan(*(const XeTexDesc *)&t))) return -8;
        }
    }
    return 0;
}

// state into the slot at base (CPU) / baseVA (GPU), commands into sDrawBuf at n. -7: no room left (cap)
int IrisXeProbe::mtlBuildDraw(const XeDrawDesc *d, volatile UInt32 *base, UInt32 baseVA, UInt32 &n, UInt32 cap) {
    bool draw = d->vertexCount && d->instanceCount;
    const UInt32 delta = baseVA - DRW_VA;
    // ---- state
    volatile UInt32 *dy = base + DRW_DYN_OFF / 4, *sf = base + DRW_SURF_OFF / 4;
    const UInt32 dynVA = baseVA + DRW_DYN_OFF;                 // absolute addresses: this slot
    const UInt32 relDyn = DRW_VA + DRW_DYN_OFF;                // offsets from the dynamic state base (moved with the slot)
    for (UInt32 k = 0; k < 24; k++) {                          // surface states: RT 0..7, textures 8..23 (v0.51.8: 8..15 before)
        const XeTexDesc none = {};
        const XeTexDesc *t = k < 8 ? (k < d->nrt ? (const XeTexDesc *)&d->rt[k] : &none)
                                   : (draw && k - 8 < d->ntex ? (const XeTexDesc *)&d->tex[k - 8] : &none);
        UInt32 base = (t->pad >> 8) & 0xFF;
        const UInt32 ssAt = k < 16 ? SRF_SS + 64 * k : SRF_SSX + 64 * (k - 16);
        if (k < 8) packSurfaceState(sf + ssAt / 4, *t, base);                        // render target: its level
        // v0.63.2 fill-rate test: render targets through MOCS 61 (L3 write-back, LLC bypassed: lines go to memory
        // whole, never read first) or 60 (L3 bypassed, LLC write-back) instead of 2 (both write-back)
        if (k < 8 && k < d->nrt && (mtlPSTest == 3 || mtlPSTest == 4))
            sf[ssAt / 4 + 1] = (sf[ssAt / 4 + 1] & 0x81FFFFFFu) | ((mtlPSTest == 3 ? 61u : 60u) << 25);
        else packSurfaceState(sf + ssAt / 4, *t, (t->pad & TEX_ONELEVEL) ? 0 : texLevels(*t) - 1 - base, base);
        sf[SRF_BTPS / 4 + k] = ssAt;
        if (k < 8) sf[SRF_BTCLR / 4 + 8 * k] = SRF_SS + 64 * k;
        if (k < 8) { for (UInt32 c = 0; c < 4; c++) dy[(DYN_CLEAR + 32 * k) / 4 + c] = d->clear[k][c]; }
    }
    for (UInt32 k = 0; k < 8; k++) {                           // v0.47.0: vertex textures (VS binding table 0..7)
        const XeTexDesc none = {};
        const XeTexDesc *t = draw && k < d->nvtex ? (const XeTexDesc *)&d->vtex[k] : &none;
        UInt32 base = (t->pad >> 8) & 0xFF;
        packSurfaceState(sf + (SRF_SSVS + 64 * k) / 4, *t, (t->pad & TEX_ONELEVEL) ? 0 : texLevels(*t) - 1 - base, base);
        sf[SRF_BTVS / 4 + k] = SRF_SSVS + 64 * k;
        XeSampDesc sm = draw && k < d->nvsamp ? *(const XeSampDesc *)&d->vsamp[k] : XeSampDesc{};
        packSamplerState(dy + (DYN_SAMPVS + 16 * k) / 4, sm, (UInt32)(relDyn + DYN_BORDER - CPT_DSB));
    }
    for (UInt32 k = 0; k < 16; k++) dy[DYN_BORDER / 4 + k] = 0;
    for (UInt32 k = 0; k < 8; k++) {
        XeSampDesc sm = draw && k < d->nsamp ? *(const XeSampDesc *)&d->samp[k] : XeSampDesc{};
        packSamplerState(dy + (DYN_SAMP + 16 * k) / 4, sm, (UInt32)(relDyn + DYN_BORDER - CPT_DSB));
    }
    for (UInt32 k = 0; k < 16; k++) {                          // v0.51.8: 16 buffer addresses per stage
        dy[DYN_PUSHVS / 4 + k] = draw ? d->vsBuf[k] : 0;
        dy[DYN_PUSHPS / 4 + k] = draw ? d->psBuf[k] : 0;
    }
    UInt32 psb[4] = { 0, 0, 0, 0 };                            // render target 0: src, dst, srcA, dstA (Gen)
    bool psbOn = false, indep = false;
    if (draw) {                                                // v0.33.0: BLEND_STATE, blend constant, scissor
        UInt32 hdr = kDrawBlend[0];
        for (UInt32 k = 0; k < XE_DRAW_MAX_RT; k++) {
            const XeDrawBlend &b = d->blend[k];
            UInt32 e[2] = { kDrawBlend[BLS_LENGTH + 2 * k], kDrawBlend[BLS_LENGTH + 2 * k + 1] };
            UInt32 m = k < d->nrt ? b.writeMask : 15;
            if (!(m & 8)) dpatch(e, 0, BLE_WD_R, 1);
            if (!(m & 4)) dpatch(e, 0, BLE_WD_G, 1);
            if (!(m & 2)) dpatch(e, 0, BLE_WD_B, 1);
            if (!(m & 1)) dpatch(e, 0, BLE_WD_A, 1);
            if (k < d->nrt && b.enable) {
                // Gen applies the factors for MIN / MAX too, Metal doesn't (as Mesa: force ONE)
                UInt32 s_ = b.op >= 3 ? GEN_BLENDFACTOR_ONE : kGenBlendFactor[b.src], d_ = b.op >= 3 ? GEN_BLENDFACTOR_ONE : kGenBlendFactor[b.dst];
                UInt32 sa = b.opA >= 3 ? GEN_BLENDFACTOR_ONE : kGenBlendFactor[b.srcA], da = b.opA >= 3 ? GEN_BLENDFACTOR_ONE : kGenBlendFactor[b.dstA];
                dpatch(e, 0, BLE_ENABLE, 1);
                dpatch(e, 0, BLE_SRC, s_); dpatch(e, 0, BLE_DST, d_); dpatch(e, 0, BLE_FUNC, kGenBlendOp[b.op]);
                dpatch(e, 0, BLE_SRCA, sa); dpatch(e, 0, BLE_DSTA, da); dpatch(e, 0, BLE_FUNCA, kGenBlendOp[b.opA]);
                if (s_ != sa || d_ != da || b.op != b.opA) indep = true;
                if (k == 0) { psbOn = true; psb[0] = s_; psb[1] = d_; psb[2] = sa; psb[3] = da; }
            }
            dy[(DYN_BLEND) / 4 + BLS_LENGTH + 2 * k] = e[0];
            dy[(DYN_BLEND) / 4 + BLS_LENGTH + 2 * k + 1] = e[1];
        }
        if (indep) { UInt32 h[1] = { hdr }; dpatch(h, 0, BLS_INDEP, 1); hdr = h[0]; }
        dy[DYN_BLEND / 4] = hdr;
        { UInt32 p_[2] = { CCS_BLEND_R }; dy[DYN_CC / 4 + p_[0]] = d->blendColor[0]; }
        { UInt32 p_[2] = { CCS_BLEND_G }; dy[DYN_CC / 4 + p_[0]] = d->blendColor[1]; }
        { UInt32 p_[2] = { CCS_BLEND_B }; dy[DYN_CC / 4 + p_[0]] = d->blendColor[2]; }
        { UInt32 p_[2] = { CCS_BLEND_A }; dy[DYN_CC / 4 + p_[0]] = d->blendColor[3]; }
        UInt32 x0 = d->scissor[0], y0 = d->scissor[1], x1 = d->scissor[2], y1 = d->scissor[3];
        if (x1 > d->width) x1 = d->width;
        if (y1 > d->height) y1 = d->height;
        UInt32 sr[2] = { 0, 0 };
        if (x1 <= x0 || y1 <= y0) { dpatch(sr, 0, SCR_XMIN, 1); dpatch(sr, 0, SCR_YMIN, 1); }   // min > max: nothing passes
        else { dpatch(sr, 0, SCR_XMIN, x0); dpatch(sr, 0, SCR_YMIN, y0); dpatch(sr, 0, SCR_XMAX, x1 - 1); dpatch(sr, 0, SCR_YMAX, y1 - 1); }
        dy[DYN_SCISSOR / 4] = sr[0]; dy[DYN_SCISSOR / 4 + 1] = sr[1];
    }
    for (UInt32 k = 0; k < 14; k++) {                          // SF_CLIP_VIEWPORT
        static const UInt8 at[14] = { SFVP_M00, SFVP_M11, SFVP_M22, SFVP_M30, SFVP_M31, SFVP_M32, SFVP_GXMIN, SFVP_GXMAX,
                                      SFVP_GYMIN, SFVP_GYMAX, SFVP_VXMIN, SFVP_VXMAX, SFVP_VYMIN, SFVP_VYMAX };
        dy[DYN_SFVP / 4 + at[k]] = d->vp[k];
    }
    volatile UInt32 *cv = base + 0x400 / 4;                    // clear rectangle (float3; v0.34.0: z = depth clear value)
    union { float f; UInt32 u; } fw = { (float)d->width }, fh = { (float)d->height };
    UInt32 zc = d->ds.clearDepth ? d->ds.depthClearValue : 0;
    UInt32 rect[9] = { fw.u, fh.u, zc, 0, fh.u, zc, 0, 0, zc };
    sf[SRF_BTNULL / 4] = SRF_SSNULL;                            // a null render target for depth / stencil clears
    { const XeTexDesc none = {}; packSurfaceState(sf + SRF_SSNULL / 4, none); }
    for (UInt32 k = 0; k < 9; k++) cv[k] = rect[k];
    for (UInt32 k = 0; k < 4; k++) cv[0x40 / 4 + k] = 0;
    __asm__ volatile("mfence" ::: "memory");
    for (UInt32 k = 0; k < d->nrt; k++) {                      // clears first
        if (!(d->clearMask & (1u << k)) || !d->rt[k].va) continue;
        if (n + sizeof(kClearCmds) / 4 > cap) return -7;
        UInt32 *c = sDrawBuf + n;
        for (UInt32 i = 0; i < sizeof(kClearCmds) / 4; i++) c[i] = kClearCmds[i];
        RELOC_CLEAR(c);
        DP(c, 0, RC_RECT_XMAX, d->width - 1);
        DP(c, 0, RC_RECT_YMAX, d->height - 1);
        // address / offset fields hold the address itself: value << shift must give it back (>> shift here)
        DP(c, 0, RC_CPS_ADDR, (dynVA + DYN_CLEAR + 32 * k) >> 5);
        DP(c, 0, RC_BT, (SRF_BTCLR + 32 * k) >> 5);
        { const XeDrawDS none = {}; DS_BUFFERS(c, RC, none, false, false); }
        trimRepeat(c);                                          // v0.56.8
        n += sizeof(kClearCmds) / 4;
    }
    if (d->ds.clearDepth || d->ds.clearStencil) {               // v0.34.0: depth / stencil clear: a rectangle at the clear depth
        const XeDrawDS &z = d->ds;
        if (n + sizeof(kClearCmds) / 4 > cap) return -7;
        UInt32 *c = sDrawBuf + n;
        for (UInt32 i = 0; i < sizeof(kClearCmds) / 4; i++) c[i] = kClearCmds[i];
        RELOC_CLEAR(c);
        DP(c, 0, RC_RECT_XMAX, d->width - 1);
        DP(c, 0, RC_RECT_YMAX, d->height - 1);
        DP(c, 0, RC_CPS_ADDR, (dynVA + DYN_CLEAR) >> 5);
        DP(c, 0, RC_BT, SRF_BTNULL >> 5);
        XeDrawDS b = z;                                         // only the buffers being cleared
        if (!z.clearDepth) b.depthVA = 0;
        if (!z.clearStencil) b.stencilVA = 0;
        DS_BUFFERS(c, RC, b, z.clearDepth, z.clearStencil);
        if (z.clearDepth) { DP(c, 0, RC_DS_DTEST, 1); DP(c, 0, RC_DS_DWRITE, 1); DP(c, 0, RC_DS_DFUNC, GEN_COMPARE_ALWAYS); }
        if (z.clearStencil) {
            DP(c, 0, RC_DS_STEST, 1); DP(c, 0, RC_DS_SWRITE, 1); DP(c, 0, RC_DS_2SIDED, 1);
            DP(c, 0, RC_DS_SFUNC, GEN_COMPARE_ALWAYS); DP(c, 0, RC_DS_BFUNC, GEN_COMPARE_ALWAYS);
            DP(c, 0, RC_DS_SFAIL, GEN_STENCILOP_REPLACE); DP(c, 0, RC_DS_SZFAIL, GEN_STENCILOP_REPLACE); DP(c, 0, RC_DS_SPASS, GEN_STENCILOP_REPLACE);
            DP(c, 0, RC_DS_BFAIL, GEN_STENCILOP_REPLACE); DP(c, 0, RC_DS_BZFAIL, GEN_STENCILOP_REPLACE); DP(c, 0, RC_DS_BPASS, GEN_STENCILOP_REPLACE);
            DP(c, 0, RC_DS_SMASK, 0xFF); DP(c, 0, RC_DS_SWMASK, 0xFF); DP(c, 0, RC_DS_BMASK, 0xFF); DP(c, 0, RC_DS_BWMASK, 0xFF);
            DP(c, 0, RC_DS_SREF, z.stencilClearValue); DP(c, 0, RC_DS_BREF, z.stencilClearValue);
        }
        trimRepeat(c);                                          // v0.56.8
        n += sizeof(kClearCmds) / 4;
    }
    if (draw) {
        UInt32 *t = sDrawTmpl;
        for (UInt32 i = 0; i < sizeof(kDrawCmds) / 4; i++) t[i] = kDrawCmds[i];
        RELOC_DRAW(t);
        DP(t, 0, RD_RECT_XMAX, d->width - 1);
        DP(t, 0, RD_RECT_YMAX, d->height - 1);
        if (d->sysValues & 1) { DP(t, 0, RD_SGVS_VID_EN, 1); DP(t, 0, RD_SGVS_VID_COMP, 0); DP(t, 0, RD_SGVS_VID_ELEM, d->sysSlot); }
        if (d->sysValues & 2) { DP(t, 0, RD_SGVS_IID_EN, 1); DP(t, 0, RD_SGVS_IID_COMP, 1); DP(t, 0, RD_SGVS_IID_ELEM, d->sysSlot); }
        DP(t, 0, RD_VS_KSP, (UInt32)d->vsVA >> 6);
        DP(t, 0, RD_VS_GRF, d->vsGrf);
        DP(t, 0, RD_VS_READLEN, d->vsReadLen);
        if (d->nvtex || d->nvsamp) {                           // v0.47.0: vertex textures
            DP(t, 0, RD_VS_BTCOUNT, 8);
            { UInt32 p_[2] = { RD_VS_BTCOUNT }; dpatch(t, 0, p_[0], 27, (d->nvsamp + 3) / 4); }   // Sampler Count (same dword, 29:27)
        }
        DP(t, 0, RD_CLIP_NONPERSP, d->nonPersp ? 1 : 0);
        DP(t, 0, RD_CLIP_XYTEST, d->topology >= 4 && !d->fillLines ? 1 : 0);
        DP(t, 0, RD_RASTER_CULL, d->cull);
        DP(t, 0, RD_RASTER_FRONTCCW, d->frontCCW ? 1 : 0);
        if (d->fillLines) { DP(t, 0, RD_RASTER_FILLF, 1); DP(t, 0, RD_RASTER_FILLB, 1); }
        DP(t, 0, RD_SBE_NUMATTR, d->numAttr);
        DP(t, 0, RD_SBE_READLEN, d->sbeReadLen);
        DP(t, 0, RD_SBE_CONSTINTERP, d->constInterp);
        { UInt32 p0[2] = { RD_SWIZ_SRC0 }; for (UInt32 k = 0; k < d->numAttr; k++) dpatch(t, 0, p0[0] + k / 2, p0[1] + 16 * (k % 2), d->swiz[k]); }
        DP(t, 0, RD_WM_BARY, d->baryMode);
        { UInt32 p_[2] = { RD_WM_BARY }; dpatch(t, 0, p_[0], 31, 1); }   // v0.48.3: 3DSTATE_WM Statistics Enable (PS invocations)
        DP(t, 0, RD_WM_FORCE, (d->kill & 1) ? 2 : 0);          // 2 = ForceON (v0.62.1: bit 1 of kill = helper pixels)
        DP(t, 0, RD_PS_KSP, (UInt32)d->psVA >> 6);
        DP(t, 0, RD_PS_GRF, d->psGrf);
        DP(t, 0, RD_PS_PUSH, d->psPush ? 1 : 0);
        DP(t, 0, RD_PS_SAMPCOUNT, (d->nsamp + 3) / 4);
        DP(t, 0, RD_PSX_ATTR, d->numAttr ? 1 : 0);
        DP(t, 0, RD_PSX_KILL, (d->kill & 1) ? 1 : 0);
        // v0.62.1: Vector Mask Enable - the 2x2 neighbours outside a triangle (helper pixels) run the shader too, so the
        // texture level (from the neighbours' coordinates) is right at triangle edges; without it their coordinates
        // were stale registers and window / icon edges showed dashed and diagonal lines. Set by the Metal driver
        // (kill bit 1) for fragment shaders that write no memory. Boot-arg -irisxenovmask: never.
        { static int noVM = -1; if (noVM < 0) noVM = PE_parse_boot_argn("-irisxenovmask", nullptr, 0) ? 1 : 0;
          if ((d->kill & 2) && !noVM) DP(t, 0, RD_PS_VMASK, 1); }
        // v0.63.2: fill-rate test (irisxe-metaltest only, debug.irisxe_pstest): our own constant-colour pixel shader,
        // 1 = SIMD8 (as our compiler's), 2 = SIMD16 (16 pixels per thread: a whole cache line of a Y-tiled
        // 4-byte image per render-target write)
        if (mtlPSTest == 1 || mtlPSTest == 2) {
            UInt32 pk[2] = { RD_PS_KSP };
            t[pk[0]] = mtlPSTest == 1 ? TESTPS8_VA : TESTPS16_VA;
            if (mtlPSTest == 2) { UInt32 pd[2] = { RD_PS_PUSH }; t[pd[0]] = (t[pd[0]] & ~7u) | 2u; }
            mtlPSTestDraws++;
        }
        if (d->vsPush) { DP(t, 0, RD_CVS_LEN, d->vsPush); DP(t, 0, RD_CVS_ADDR, (dynVA + DYN_PUSHVS) >> 5); }
        if (d->psPush) { DP(t, 0, RD_CPS_LEN, d->psPush); DP(t, 0, RD_CPS_ADDR, (dynVA + DYN_PUSHPS) >> 5); }
        DP(t, 0, RD_TOPO, d->topology);
        DP(t, 0, RD_PRIM_TOPO, d->topology);
        DP(t, 0, RD_PRIM_COUNT, d->vertexCount);
        DP(t, 0, RD_PRIM_START, d->vertexStart);
        DP(t, 0, RD_PRIM_INST, d->instanceCount);
        DP(t, 0, RD_PRIM_BASEINST, d->baseInstance);
        {   // v0.34.0: depth / stencil test, buffers, z clipping, depth bias, computed depth
            const XeDrawDS &z = d->ds;
            bool sw = z.stencilVA && (z.front.writeMask || z.back.writeMask);
            DS_BUFFERS(t, RD, z, z.depthVA && z.depthWrite, sw);
            if (z.depthVA) {
                DP(t, 0, RD_DS_DTEST, 1);
                if (z.depthWrite) DP(t, 0, RD_DS_DWRITE, 1);
                DP(t, 0, RD_DS_DFUNC, kGenCompare[z.depthFunc]);
            }
            if (z.stencilVA) {
                DP(t, 0, RD_DS_STEST, 1); DP(t, 0, RD_DS_2SIDED, 1);
                if (sw) DP(t, 0, RD_DS_SWRITE, 1);
                DP(t, 0, RD_DS_SFUNC, kGenCompare[z.front.func]); DP(t, 0, RD_DS_SFAIL, kGenStencilOp[z.front.fail]);
                DP(t, 0, RD_DS_SZFAIL, kGenStencilOp[z.front.depthFail]); DP(t, 0, RD_DS_SPASS, kGenStencilOp[z.front.pass]);
                DP(t, 0, RD_DS_BFUNC, kGenCompare[z.back.func]); DP(t, 0, RD_DS_BFAIL, kGenStencilOp[z.back.fail]);
                DP(t, 0, RD_DS_BZFAIL, kGenStencilOp[z.back.depthFail]); DP(t, 0, RD_DS_BPASS, kGenStencilOp[z.back.pass]);
                DP(t, 0, RD_DS_SMASK, z.front.readMask); DP(t, 0, RD_DS_SWMASK, z.front.writeMask);
                DP(t, 0, RD_DS_BMASK, z.back.readMask); DP(t, 0, RD_DS_BWMASK, z.back.writeMask);
                DP(t, 0, RD_DS_SREF, z.frontRef); DP(t, 0, RD_DS_BREF, z.backRef);
            }
            if (!z.depthClamp) { DP(t, 0, RD_RASTER_ZNEAR, 1); DP(t, 0, RD_RASTER_ZFAR, 1); }
            if (z.biasConstant || z.biasSlope) {
                DP(t, 0, RD_RASTER_DOSOLID, 1); DP(t, 0, RD_RASTER_DOWIRE, 1); DP(t, 0, RD_RASTER_DOPOINT, 1);
                { UInt32 p_[2] = { RD_RASTER_DOCONST }; t[p_[0]] = z.biasConstant; }
                { UInt32 p_[2] = { RD_RASTER_DOSCALE }; t[p_[0]] = z.biasSlope; }
                { UInt32 p_[2] = { RD_RASTER_DOCLAMP }; t[p_[0]] = z.biasClamp; }
            }
            if (z.psDepth) DP(t, 0, RD_PSX_CDEPTH, 1);
            if (z.psSrcDepth & 1) DP(t, 0, RD_PSX_SRCDEPTH, 1);      // v0.35.0: [[position]].z in the payload
            if (z.psSrcDepth & 2) DP(t, 0, RD_PSX_SRCW, 1);          // v0.51.23: [[position]].w (source W, after the depth)
        }
        {   // 3DSTATE_PS_BLEND = render target 0's blending
            if (psbOn) {
                DP(t, 0, RD_PSB_ENABLE, 1);
                DP(t, 0, RD_PSB_SRC, psb[0]); DP(t, 0, RD_PSB_DST, psb[1]);
                DP(t, 0, RD_PSB_SRCA, psb[2]); DP(t, 0, RD_PSB_DSTA, psb[3]);
            }
            if (indep) DP(t, 0, RD_PSB_INDEP, 1);
        }
        if (d->ibType) {                                       // indexed draw, Metal's restart index always on
            DP(t, 0, RD_IB_FMT, d->ibType == 1 ? GEN_INDEX_WORD : GEN_INDEX_DWORD);
            { UInt32 p_[2] = { RD_IB_ADDR }; t[p_[0]] = (UInt32)d->ibVA; t[p_[0] + 1] = (UInt32)(d->ibVA >> 32); }
            DP(t, 0, RD_IB_SIZE, d->ibSize);
            DP(t, 0, RD_PRIM_ACCESS, GEN_RANDOM);
            DP(t, 0, RD_PRIM_BASEVTX, (UInt32)d->baseVertex);
            DP(t, 0, RD_VF_CUTEN, 1);
            DP(t, 0, RD_VF_CUT, d->ibType == 1 ? 0xFFFFu : 0xFFFFFFFFu);
        }
        UInt32 nve = d->nve ? d->nve : 1;
        UInt32 need = sizeof(kDrawCmds) / 4 + 1 + 4 * d->nvb + 1 + 2 * nve + 3 * nve + 2;
        if (n + need > cap) return -7;
        UInt32 *c = sDrawBuf + n;
        UInt32 m = 0;
        for (UInt32 i = 0; i < D_AT_VB; i++) c[m++] = t[i];
        trimRepeat(c);                                          // v0.56.8
        if (d->nvb) {
            c[m++] = VB_HEADER | (1 + 4 * d->nvb - 2);
            for (UInt32 k = 0; k < d->nvb; k++) {
                UInt32 *v = c + m;
                v[0] = VBS_DW0; v[1] = v[2] = v[3] = 0;
                { UInt32 p_[2] = { VBS_INDEX }; v[p_[0]] |= k << p_[1]; }
                { UInt32 p_[2] = { VBS_PITCH }; v[p_[0]] |= d->vb[k].pitch << p_[1]; }
                { UInt32 p_[2] = { VBS_ADDR };  v[p_[0]] = (UInt32)d->vb[k].va; v[p_[0] + 1] = (UInt32)(d->vb[k].va >> 32); }
                { UInt32 p_[2] = { VBS_SIZE };  v[p_[0]] = d->vb[k].size; }
                m += 4;
            }
        }
        c[m++] = VE_HEADER | (1 + 2 * nve - 2);
        for (UInt32 k = 0; k < nve; k++) {
            UInt32 *v = c + m;
            v[0] = v[1] = 0;
            if (d->nve) {
                const XeDrawVE &e = d->ve[k];
                { UInt32 p_[2] = { VES_OFFSET }; v[p_[0]] |= e.offset << p_[1]; }
                { UInt32 p_[2] = { VES_FORMAT }; v[p_[0]] |= e.format << p_[1]; }
                { UInt32 p_[2] = { VES_VALID };  v[p_[0]] |= 1u << p_[1]; }
                { UInt32 p_[2] = { VES_VB };     v[p_[0]] |= e.vb << p_[1]; }
                { UInt32 p_[2] = { VES_C0 };     v[p_[0]] |= ((e.ctrl >> 0) & 15) << p_[1]; }
                { UInt32 p_[2] = { VES_C1 };     v[p_[0]] |= ((e.ctrl >> 4) & 15) << p_[1]; }
                { UInt32 p_[2] = { VES_C2 };     v[p_[0]] |= ((e.ctrl >> 8) & 15) << p_[1]; }
                { UInt32 p_[2] = { VES_C3 };     v[p_[0]] |= ((e.ctrl >> 12) & 15) << p_[1]; }
            } else {                                           // no inputs: one element of constants (0, 0, 0, 1)
                { UInt32 p_[2] = { VES_VALID };  v[p_[0]] |= 1u << p_[1]; }
                { UInt32 p_[2] = { VES_C0 };     v[p_[0]] |= VFCOMP_0_ << p_[1]; }
                { UInt32 p_[2] = { VES_C1 };     v[p_[0]] |= VFCOMP_0_ << p_[1]; }
                { UInt32 p_[2] = { VES_C2 };     v[p_[0]] |= VFCOMP_0_ << p_[1]; }
                { UInt32 p_[2] = { VES_C3 };     v[p_[0]] |= VFCOMP_1FP_ << p_[1]; }
            }
            m += 2;
        }
        for (UInt32 i = D_AT_VB; i < D_AT_INST; i++) c[m++] = t[i];
        for (UInt32 k = 0; k < nve; k++) {
            UInt32 *v = c + m;
            v[0] = VFI_DW0; v[1] = v[2] = 0;
            { UInt32 p_[2] = { VFI_INDEX }; v[p_[0]] |= k << p_[1]; }
            if (d->nve && d->ve[k].instanced) {
                { UInt32 p_[2] = { VFI_ENABLE }; v[p_[0]] |= 1u << p_[1]; }
                { UInt32 p_[2] = { VFI_STEP };   v[p_[0]] |= (d->ve[k].stepRate ? d->ve[k].stepRate : 1) << p_[1]; }
            }
            m += 3;
        }
        if (d->nvsamp) {                                       // v0.47.0: 3DSTATE_SAMPLER_STATE_POINTERS_VS
            c[m++] = 0x782B0000;
            c[m++] = (UInt32)(relDyn + DYN_SAMPVS - CPT_DSB);
        }
        for (UInt32 i = D_AT_INST; i < sizeof(kDrawCmds) / 4; i++) c[m++] = t[i];
        n += m;
    }
    return 0;
}

UInt32 IrisXeProbe::mtlDrawPrologue() {
    UInt32 n = 0;
    // v0.40.4: first a TLB invalidate (CS stall + post-sync write, as the compute jobs and i915 do). Draw jobs had
    // none: memory mapped after the process's first job (a new 2 MB page table: a big render target, a window's
    // drawables) was still "not present" in the render engine's cached page walk -> page fault -> GPU hang
    // (IrisXe-Hang: stuck at the job's PIPELINE_SELECT, v0.40.0-v0.40.3 irisxe-mtkdemo)
    sDrawBuf[n++] = GFX_OP_PIPE_CONTROL(6);
    sDrawBuf[n++] = PC_CS_STALL | PC_QW_WRITE | PC_GLOBAL_GTT | PC_TLB_INVALIDATE |
                    (1u << 11) | (1u << 10) | (1u << 3) | (1u << 2);   // + instruction / texture / constant / state caches
    sDrawBuf[n++] = R_GGTT_ADDR + R_HWSP * 4096 + 0x80 * 4; sDrawBuf[n++] = 0; sDrawBuf[n++] = 0; sDrawBuf[n++] = 0;
    // v0.43.1: the render workarounds, every job (a CS stall above already waited for the GPU)
    sDrawBuf[n++] = MI_LRI(sizeof(kWaRegs) / sizeof(kWaRegs[0]) + 1);
    for (const auto &w : kWaRegs) { sDrawBuf[n++] = w[0]; sDrawBuf[n++] = w[1]; }
    sDrawBuf[n++] = 0x20a0; sDrawBuf[n++] = sFFThreadMode;
    // v0.45.1: GPU timestamp at the start of the draw (HWSP dwords 0xA0/0xA1) and at its end (0xA2/0xA3)
    sDrawBuf[n++] = GFX_OP_PIPE_CONTROL(6);
    sDrawBuf[n++] = PC_GLOBAL_GTT | PC_CS_STALL | (3u << 14);  // post-sync: write the timestamp (needs a stall bit)
    sDrawBuf[n++] = R_GGTT_ADDR + R_HWSP * 4096 + 0xA0 * 4; sDrawBuf[n++] = 0; sDrawBuf[n++] = 0; sDrawBuf[n++] = 0;
    // v0.48.3: pixels shaded by this draw: PS_INVOCATION_COUNT before (HWSP 0xA4) and after (0xA6)
    sDrawBuf[n++] = (0x24u << 23) | (1u << 22) | 2;            // MI_STORE_REGISTER_MEM, GGTT
    sDrawBuf[n++] = RCS + 0x348; sDrawBuf[n++] = R_GGTT_ADDR + R_HWSP * 4096 + 0xA4 * 4; sDrawBuf[n++] = 0;
    if (gpuHashOK) {                                           // v0.57.8: pixel pipe balancing (3D pipeline selected first)
        for (int i = 0; i < 7; i++) sDrawBuf[n++] = kDrawCmds[i];   // the draw template's PIPE_CONTROL + PIPELINE_SELECT 3D
        if (gpuHashOn) for (int i = 0; i < 14; i++) sDrawBuf[n++] = gpuHashDW[i];
        sDrawBuf[n++] = (3u << 29) | (3u << 27) | (1u << 24) | (0x1Eu << 16) | 1;   // 3DSTATE_3D_MODE, 3 dwords
        sDrawBuf[n++] = (gpuHashOn ? (1u << 5) : 0) | (1u << 21);  // Subslice Hashing Table Enable (+ its mask bit)
        sDrawBuf[n++] = 0;
    }
    return n;
}
UInt32 IrisXeProbe::mtlDrawEpilogue(UInt32 n) {
    // v0.40.8: leave no push constants behind. The clear template (and draws with push constants) end with a
    // 3DSTATE_CONSTANT_PS/VS that reads a buffer; the engine saves that in its context, and restoring it at the
    // start of the NEXT job hung the GPU (irisxe-mtkdemo: job 1 fine, job 2 stuck at its first command; the
    // metaltest window test, whose jobs end with a draw without push constants, ran 300 jobs fine).
    // 3DSTATE_CONSTANT_ALL for all stages with no buffers = push constants off (as iris does for unused stages).
    sDrawBuf[n++] = 0x786d1f00; sDrawBuf[n++] = 0x00000004;
    sDrawBuf[n++] = GFX_OP_PIPE_CONTROL(6);                    // v0.45.1: end timestamp, after all rendering
    sDrawBuf[n++] = PC_GLOBAL_GTT | PC_CS_STALL | (3u << 14) | (1u << 1) | (1u << 12) | (1u << 0) |  // + scoreboard stall, RT flush, depth flush
                    (1u << 28) | (1u << 27) | (1u << 5) | (1u << 7);   // v0.49.1: tile cache, L3, DC flush (now cached; as i915)
    sDrawBuf[n++] = R_GGTT_ADDR + R_HWSP * 4096 + 0xA2 * 4; sDrawBuf[n++] = 0; sDrawBuf[n++] = 0; sDrawBuf[n++] = 0;
    sDrawBuf[n++] = (0x24u << 23) | (1u << 22) | 2;            // v0.48.3: PS_INVOCATION_COUNT after
    sDrawBuf[n++] = RCS + 0x348; sDrawBuf[n++] = R_GGTT_ADDR + R_HWSP * 4096 + 0xA6 * 4; sDrawBuf[n++] = 0;
    return n;
}

// v0.60.1: IrisXe-DrawPhases - where the CPU time of a draw job goes (averages over 200 jobs, microseconds)
//   check: the draws' checks, build: commands + state, copy: state from the cached copy into the slots,
//   submit: waiting for the engine + handing the job over, gpu: until the job-end was seen, wake: from then until
//   the waiting thread ran again, stats: DrawPerf / DrawMix bookkeeping
enum { PH_CHECK, PH_BUILD, PH_COPY, PH_SUBMIT, PH_GPU, PH_WAKE, PH_STATS, PH_N };
static UInt64 sPh[PH_N], sPhMax[PH_N];
static UInt32 sPhJobs;
static inline UInt64 phNs(uint64_t a, uint64_t b) { UInt64 ns = 0; absolutetime_to_nanoseconds(b - a, &ns); return ns; }
static inline void phAdd(int k, UInt64 ns) { sPh[k] += ns; if (ns > sPhMax[k]) sPhMax[k] = ns; }

// v0.63.1: smart flushing between the draws of a job. The full flush + invalidate + CS stall between batched draws
// (v0.49.3) drains the GPU after every draw. It is only needed when a draw reads memory that an earlier draw of the
// same job wrote (a render target or depth buffer used as a texture, vertex data or a buffer), or around clears.
// Written ranges of the job so far:
struct XeWr { UInt64 lo, hi; };
static XeWr sWr[XE_BATCH_SLOTS * (XE_DRAW_MAX_RT + 2)];
static UInt32 sWrN;
static UInt64 sFlushSkipped, sFlushDone;
static UInt64 texBytes(const XeDrawTex &t) {
    UInt64 rows = t.qpitch > t.height ? t.qpitch : t.height;
    UInt32 layers = t.layers ? t.layers : 1;
    UInt32 levels = (t.pad & 0xFF) ? (t.pad & 0xFF) : 1;
    UInt64 n = (UInt64)t.pitch * rows * layers;
    if (levels > 1 && layers == 1 && t.qpitch) n = (UInt64)t.pitch * t.qpitch;   // a mip chain lives inside qpitch rows
    return n ? n : 4096;
}
static bool wrHit(UInt64 va, UInt64 len) {
    if (!va) return false;
    UInt64 lo = va, hi = va + (len ? len : 4);
    for (UInt32 i = 0; i < sWrN; i++) if (lo < sWr[i].hi && sWr[i].lo < hi) return true;
    return false;
}
static void wrAdd(UInt64 va, UInt64 len) {
    if (!va || sWrN >= sizeof(sWr) / sizeof(sWr[0])) return;
    sWr[sWrN].lo = va; sWr[sWrN].hi = va + (len ? len : 4); sWrN++;
}
static XeWr sRd[XE_BATCH_SLOTS * (XE_DRAW_MAX_FTEX + XE_DRAW_MAX_TEX + 4)];   // ... and read (a later draw may not write them)
static UInt32 sRdN;
static bool rdHit(UInt64 va, UInt64 len) {
    if (!va) return false;
    UInt64 lo = va, hi = va + (len ? len : 4);
    for (UInt32 i = 0; i < sRdN; i++) if (lo < sRd[i].hi && sRd[i].lo < hi) return true;
    return false;
}
static void rdAdd(UInt64 va, UInt64 len) {
    if (!va) return;
    if (sRdN >= sizeof(sRd) / sizeof(sRd[0])) { sRd[0].lo = 0; sRd[0].hi = ~0ULL; sRdN = 1; return; }   // full: assume all
    sRd[sRdN].lo = va; sRd[sRdN].hi = va + (len ? len : 4); sRdN++;
}
static bool drawWritesRead(const XeDrawDesc *d) {
    for (UInt32 k = 0; k < d->nrt && k < XE_DRAW_MAX_RT; k++) if (rdHit(d->rt[k].va, texBytes(d->rt[k]))) return true;
    if (d->ds.depthVA && rdHit(d->ds.depthVA, d->ds.depthSize)) return true;
    if (d->ds.stencilVA && rdHit(d->ds.stencilVA, d->ds.stencilSize)) return true;
    return false;
}
static bool drawReadsWritten(const XeDrawDesc *d) {
    for (UInt32 i = 0; i < d->ntex && i < XE_DRAW_MAX_FTEX; i++) if (wrHit(d->tex[i].va, texBytes(d->tex[i]))) return true;
    for (UInt32 i = 0; i < d->nvtex && i < XE_DRAW_MAX_TEX; i++) if (wrHit(d->vtex[i].va, texBytes(d->vtex[i]))) return true;
    for (UInt32 i = 0; i < d->nvb && i < XE_DRAW_MAX_VB; i++) if (wrHit(d->vb[i].va, d->vb[i].size)) return true;
    for (UInt32 i = 0; i < 16; i++) {
        if (wrHit(d->vsBuf[i], d->vsLen[i])) return true;
        if (wrHit(d->psBuf[i], d->psLen[i])) return true;
    }
    if (d->ibVA && wrHit(d->ibVA, d->ibSize)) return true;
    return false;
}
static void drawNoteReads(const XeDrawDesc *d) {         // textures only: buffers are never render targets
    for (UInt32 i = 0; i < d->ntex && i < XE_DRAW_MAX_FTEX; i++) rdAdd(d->tex[i].va, texBytes(d->tex[i]));
    for (UInt32 i = 0; i < d->nvtex && i < XE_DRAW_MAX_TEX; i++) rdAdd(d->vtex[i].va, texBytes(d->vtex[i]));
}
static void drawNoteWrites(const XeDrawDesc *d) {
    for (UInt32 k = 0; k < d->nrt && k < XE_DRAW_MAX_RT; k++) wrAdd(d->rt[k].va, texBytes(d->rt[k]));
    if (d->ds.depthVA) wrAdd(d->ds.depthVA, d->ds.depthSize);
    if (d->ds.stencilVA) wrAdd(d->ds.stencilVA, d->ds.stencilSize);
}

int IrisXeProbe::mtlRunDraw3D(const XeSpaceArgs *a, UInt32 *seqOut) {
    uint64_t tIn; clock_get_uptime(&tIn);                      // v0.45.0: IrisXe-DrawPerf
    if (!gpuUp || gpuHung || !gpuWL) return -1;
    if (!rcsOn || !rcsVA || !drwVA) return -6;
    const XeDrawDesc *ds = a->draw;
    UInt32 cnt = a->count ? a->count : 1;                       // v0.49.0: a batch of draws
    if (!ds || cnt > XE_BATCH_MAX || (cnt > 1 && !mtlBatchOK)) return -8;
    for (UInt32 i = 0; i < cnt; i++) { int r = mtlCheckDraw(&ds[i]); if (r) return r; }
    uint64_t tPh; clock_get_uptime(&tPh);
    phAdd(PH_CHECK, phNs(tIn, tPh));
    UInt64 copyNs = 0;
    {
        const XeDrawDesc *d = &ds[cnt - 1];
        lastVS = d->vsVA; lastPS = d->psVA; lastVerts = d->vertexCount; lastInst = d->instanceCount;   // v0.42.2
        lastIdx = d->ibVA ? d->ibType : 0; lastRT = d->nrt; lastTex = d->ntex; lastVB = d->nvb;
    }
    // v0.68.33: a job may use at most half the render ring (16 KB). The job is copied into the ring; when it does
    // not fit before the ring's end, the end is padded and the job starts again at 0 - and a job longer than the
    // place where the GPU stopped (the old tail) then ends PAST it: the GPU runs only the part between, from the
    // middle of the job, and hangs. That was the "5+ draws per job hang" (batch test: 5 draws hung every second,
    // and some big 4-draw jobs too). At most half the ring, the restart point is always behind the job's end.
    const UInt32 capRing = R_RING_BYTES / 4 / 2 - 64, capBuf = sizeof(sDrawBuf) / 4 - 32;
    const UInt32 cap = capRing < capBuf ? capRing : capBuf;     // room for the epilogue
    UInt32 i = 0;
    while (i < cnt) {
        UInt32 n = mtlDrawPrologue();
        sJobHasTemplate = false;                                 // v0.56.8: a new job
        const UInt32 nPrologue = n, first = i;
        UInt32 drawsHere = 0, clearsHere = 0;
        UInt32 maxSlots = mtlBatchSlots >= 1 && mtlBatchSlots <= XE_BATCH_SLOTS ? mtlBatchSlots : XE_BATCH_SLOTS;
        // v0.63.1: IrisXeAccel's debug.irisxe_batch (tpg bits 15:8) - draws per job for this call
        { UInt32 want = (a->tpg >> 8) & 0xFF; if (want >= 1 && want <= XE_BATCH_SLOTS) maxSlots = want; }
        const bool smart = (a->tpg & 2) != 0;               // v0.63.1: debug.irisxe_flush=1
        mtlPSTest = (a->tpg >> 2) & 7;                       // v0.63.2: debug.irisxe_pstest (the test program only)
        sWrN = 0; sRdN = 0;
        for (UInt32 slot = 0; i < cnt && slot < maxSlots; slot++) {
            const XeDrawDesc *d = &ds[i];
            bool alone = cnt == 1;
            volatile UInt32 *base = alone ? drwVA : slotVA[slot];
            UInt32 baseVA = alone ? DRW_VA : SLOT_VA(slot);
            if (!base) return -6;
            // v0.60.1: build into the cached copy, then copy it to the slot (no reads from write-combining memory)
            const UInt32 shi = alone ? XE_BATCH_SLOTS : slot;
            UInt32 *sh = mtlShadow[shi];
            if (sh && !mtlShadowReady[shi]) {                     // first use: the slot's current contents
                for (UInt32 k = 0; k < DRW_PAGES * 1024; k++) sh[k] = base[k];
                mtlShadowReady[shi] = true;
            }
            volatile UInt32 *slotMem_ = base;
            if (sh) base = (volatile UInt32 *)sh;
            UInt32 n0 = n;
            // v0.49.3: between the draws of a job: everything the draw before wrote out, every cache fresh, as if
            // it were a job of its own (v0.49.2 batched jobs hung: all commands read, the drawing never finished;
            // one draw per job never did). IrisXe-BatchCfg bit 8 turns this off (tests).
            bool needFlush = mtlBatchBarrier;
            if (slot && smart && needFlush) {               // v0.63.1: only when this draw reads what the job wrote
                const XeDrawDesc *pd = &ds[i - 1];
                bool clears = d->clearMask || d->ds.clearDepth || d->ds.clearStencil ||
                              pd->clearMask || pd->ds.clearDepth || pd->ds.clearStencil;
                needFlush = clears || drawReadsWritten(d) || drawWritesRead(d);
                if (needFlush) { sFlushDone++; sWrN = 0; sRdN = 0; } else sFlushSkipped++;
            }
            if (slot && needFlush) {
                if (n + 12 > cap) break;
                sDrawBuf[n++] = GFX_OP_PIPE_CONTROL(6) | (1u << 9);    // + HDC pipeline flush
                sDrawBuf[n++] = PC_CS_STALL | (1u << 1) | (1u << 12) | (1u << 0) | (1u << 13) | (1u << 28) | (1u << 27) | (1u << 5) | (1u << 7);
                sDrawBuf[n++] = 0; sDrawBuf[n++] = 0; sDrawBuf[n++] = 0; sDrawBuf[n++] = 0;
                sDrawBuf[n++] = GFX_OP_PIPE_CONTROL(6);
                sDrawBuf[n++] = PC_CS_STALL | PC_QW_WRITE | PC_GLOBAL_GTT | PC_TLB_INVALIDATE |
                                (1u << 11) | (1u << 10) | (1u << 4) | (1u << 3) | (1u << 2);
                sDrawBuf[n++] = R_GGTT_ADDR + R_HWSP * 4096 + 0x80 * 4; sDrawBuf[n++] = 0; sDrawBuf[n++] = 0; sDrawBuf[n++] = 0;
            }
            int r = mtlBuildDraw(d, base, baseVA, n, cap);
            if (r == -7 && i > first) { n = n0; break; }          // full: run what we have, the rest next (barrier dropped too)
            if (r) return r == -7 ? -8 : r;
            if (sh) {
                uint64_t c0; clock_get_uptime(&c0);
                memcpy((void *)slotMem_, sh, DRW_PAGES * 4096);
                __asm__ volatile("sfence" ::: "memory");
                uint64_t c1; clock_get_uptime(&c1);
                copyNs += phNs(c0, c1);
            }
            if (d->vertexCount && d->instanceCount) drawsHere++;
            if (d->clearMask) clearsHere++;
            if (smart) { drawNoteWrites(d); drawNoteReads(d); }
            i++;
        }
        if (n == nPrologue) continue;                            // nothing to do
        n = mtlDrawEpilogue(n);
        {
            uint64_t tb; clock_get_uptime(&tb);
            UInt64 b = phNs(tPh, tb);
            phAdd(PH_BUILD, b > copyNs ? b - copyNs : 0); phAdd(PH_COPY, copyNs);
            copyNs = 0;
        }
        // v0.63.0: only the call's last job may be left running (the jobs before it share the state slots)
        bool async = (a->tpg & 1) && i >= cnt;
        int r = mtlSubmitDrawJob(a, seqOut, n, tIn, i - first, drawsHere, clearsHere, &ds[first], async);
        if (r) return r;
        clock_get_uptime(&tIn);
        tPh = tIn;
    }
    return 0;
}

int IrisXeProbe::mtlSubmitDrawJob(const XeSpaceArgs *a, UInt32 *seqOut, UInt32 n, uint64_t tIn, UInt32 nd,
                                  UInt32 drawsHere, UInt32 clearsHere, const XeDrawDesc *d, bool async) {
    UInt32 seq = 0, waited = 0, timeoutMS = a->timeoutMS ? a->timeoutMS : 2000;
    UInt32 resets0 = gpuResets;                                  // v0.44.0
    uint64_t t0_; clock_get_uptime(&t0_);
    for (;;) {
        IOReturn r = gpuWL->runAction(&IrisXeProbe::mtlTrySubmitRenderGated, this, sDrawBuf, (void *)(uintptr_t)n, &seq);
        if (r == kIOReturnSuccess) break;
        if (r != kIOReturnBusy || waited >= timeoutMS) { mtlDrawFails++; return -2; }
        waited = gpuWait(false, 0, t0_);          // v0.45.0: wake on the next job end
    }
    uint64_t tSub; clock_get_uptime(&tSub);                   // v0.60.1
    if (seqOut) *seqOut = seq;
    if (async) {                                                // v0.63.0: left running; finished by mtlFinishAsync
        if (!mtlAsyncDesc) mtlAsyncDesc = IOMalloc(sizeof(XeDrawDesc));
        if (mtlAsyncDesc) memcpy(mtlAsyncDesc, d, sizeof(XeDrawDesc));
        mtlAsyncPend = true; mtlAsyncSeq = seq; mtlAsyncN = n; mtlAsyncND = nd; mtlAsyncDraws = drawsHere;
        mtlAsyncClears = clearsHere; mtlAsyncTIn = tIn; mtlAsyncT0 = t0_; mtlAsyncTSub = tSub; mtlAsyncTimeout = timeoutMS;
        sAsyncResets = resets0;
        mtlAsyncJobs++;
        return 0;
    }
    return mtlJobFinished(seq, tIn, t0_, tSub, n, nd, drawsHere, clearsHere, d, timeoutMS, resets0);
}

// v0.63.0: an asynchronous job (mtlJobLock held): wait for its end, then its statistics. Called before anything
// else uses the render engine, the draw state slots or the page tables.
int IrisXeProbe::mtlFinishAsync() {
    if (!mtlAsyncPend) return 0;
    mtlAsyncPend = false;
    if ((SInt32)(rcsSeqDone - mtlAsyncSeq) < 0) mtlAsyncLate++;
    if ((mtlAsyncJobs & 255) == 1) {                            // IrisXe-Async: how often the next one had to wait
        char t[160];
        snprintf(t, sizeof(t), "%u draw jobs left running, %u still running when the next GPU work came", mtlAsyncJobs, mtlAsyncLate);
        setProperty("IrisXe-Async", t);
    }
    const XeDrawDesc *d = mtlAsyncDesc ? (const XeDrawDesc *)mtlAsyncDesc : nullptr;
    if (!d) {                                                   // no copy (no memory): only wait
        uint64_t t0; clock_get_uptime(&t0);
        UInt32 waited = 0;
        while ((SInt32)(rcsSeqDone - mtlAsyncSeq) < 0 && waited < mtlAsyncTimeout && gpuResets == sAsyncResets)
            waited = gpuWait(true, mtlAsyncSeq, t0);
        bool ok = (SInt32)(rcsSeqDone - mtlAsyncSeq) >= 0;
        mtlLastSeq = mtlAsyncSeq; mtlLastRender = true; mtlLastDone = ok;
        return ok ? 0 : -3;
    }
    int r = mtlJobFinished(mtlAsyncSeq, mtlAsyncTIn, mtlAsyncT0, mtlAsyncTSub, mtlAsyncN, mtlAsyncND, mtlAsyncDraws,
                           mtlAsyncClears, d, mtlAsyncTimeout, sAsyncResets);
    mtlLastSeq = mtlAsyncSeq; mtlLastRender = true; mtlLastDone = r == 0;
    return r;
}

// v0.68.37: a draw job takes ~0.1-0.2 ms on the GPU, but a thread put to sleep until the job-end interrupt woke up
// only after its workloop handler ran and the scheduler picked it again (IrisXe-DrawPhases "wake": ~1 ms). Before
// sleeping, watch the job's sequence number in the status page for up to 250 us; when it is there, finish the job
// on the workloop at once (the same code the interrupt runs). Boot-arg -irisxenospin: off.
static IOReturn checkDoneAction(OSObject *owner, void *, void *, void *, void *) {
    ((IrisXeProbe *)owner)->gpuCheckDone(false);
    return kIOReturnSuccess;
}
static UInt64 sSpinHits, sSpinMiss;
int IrisXeProbe::mtlJobFinished(UInt32 seq, uint64_t tIn, uint64_t t0_, uint64_t tSub, UInt32 n, UInt32 nd,
                                UInt32 drawsHere, UInt32 clearsHere, const XeDrawDesc *d, UInt32 timeoutMS,
                                UInt32 resets0) {
    UInt32 waited = 0;
    { uint64_t now; clock_get_uptime(&now); UInt64 ns; absolutetime_to_nanoseconds(now - t0_, &ns); waited = (UInt32)(ns / 1000000); }
    const bool wasDone = (SInt32)(rcsSeqDone - seq) >= 0;     // an async job finished long ago: no wake-up to count
    {
        static int noSpin = -1;
        if (noSpin < 0) noSpin = PE_parse_boot_argn("-irisxenospin", nullptr, 0) ? 1 : 0;
        if (!noSpin && !wasDone && rcsVA && gpuWL) {
            uint64_t s0, now, lim; clock_get_uptime(&s0);
            nanoseconds_to_absolutetime(250000, &lim);
            volatile UInt32 *hw = rcsVA + R_HWSP * 1024 + HWSP_SEQ;
            bool seen = false;
            do {
                if ((SInt32)(*hw - seq) >= 0) { seen = true; break; }
                if (gpuResets != resets0) break;
                __asm__ volatile("pause");
                clock_get_uptime(&now);
            } while (now - s0 < lim);
            if (seen && (SInt32)(rcsSeqDone - seq) < 0) gpuWL->runAction(&checkDoneAction, this);
            if (seen) sSpinHits++; else sSpinMiss++;
        }
    }
    while ((SInt32)(rcsSeqDone - seq) < 0) {
        if (waited >= timeoutMS) { mtlDrawFails++; return -3; }
        waited = gpuWait(true, seq, t0_);          // v0.45.0: woken by the job-end interrupt
    }
    uint64_t tWoke; clock_get_uptime(&tWoke);
    if (gpuResets != resets0) { mtlDrawFails++; return -9; }    // v0.44.0: it hung, the GPU was reset
    mtlDraws += drawsHere;
    mtlClears += clearsHere;
    {   // v0.60.1: IrisXe-DrawPhases
        uint64_t done = rcsDoneAt;
        if (done < tSub || done > tWoke) done = tWoke;
        phAdd(PH_SUBMIT, phNs(t0_, tSub)); phAdd(PH_GPU, phNs(tSub, done));
        phAdd(PH_WAKE, wasDone ? 0 : phNs(done, tWoke));      // v0.68.37: not the idle time after an async job
    }
    {   // v0.45.0: how long a draw takes here (build + submit + wait) and on the GPU, averaged over 100 draws
        static UInt64 sumAll, sumGPU, sumTicks, sumPix; static UInt32 cnt, maxTicks, sumJobs; static char slow[220];
        uint64_t now; clock_get_uptime(&now);
        UInt64 ns; absolutetime_to_nanoseconds(now - tIn, &ns);
        volatile UInt32 *hw = rcsVA + R_HWSP * 1024;
        UInt32 ticks = hw[0xA2] - hw[0xA0];                      // v0.45.1: what the GPU itself took
        UInt32 pix = hw[0xA6] - hw[0xA4];                         // v0.48.3
        sumAll += ns / 1000; sumGPU += gpuLastJobUS; sumTicks += ticks; sumPix += pix; sumJobs++;
        {   // v0.58.2: where the GPU's time goes, since boot: jobs by their first draw's render target (linear /
            // Y-tiled) and blending (off / on), GPU time and pixels of each kind
            // (v0.58.3: file statics, so IrisXe-DrawMixReset can clear them)
            UInt32 k = 4;                                         // 4: no render target (clears of depth only, ...)
            if (d->nrt && d->rt[0].va) k = ((d->rt[0].pad & TEX_TILEY) ? 2 : 0) + (d->blend[0].enable ? 1 : 0);
            mixTicks[k] += ticks; mixPix[k] += pix; mixJobs[k]++;
            if (++mixN % 200 == 0 && gpuTsPerMs) {
                static const char *nm[5] = { "linear opaque", "linear blend", "tiled opaque", "tiled blend", "other" };
                char mx[600]; size_t o = 0;
                for (int i = 0; i < 5; i++) {
                    UInt64 us = mixTicks[i] * 1000 / gpuTsPerMs;
                    o += snprintf(mx + o, sizeof(mx) - o, "%s%s: %u jobs, %llu ms, %llu Mpixels (%llu Mpix/s)", i ? " | " : "", nm[i],
                                  mixJobs[i], us / 1000, mixPix[i] / 1000000, us ? mixPix[i] / us : 0);
                    if (o >= sizeof(mx)) break;
                }
                setProperty("IrisXe-DrawMix", mx);
            }
        }
        if (ticks > maxTicks) {                                   // the slowest draw of these 100
            maxTicks = ticks;
            UInt32 sx = d->scissor[2] > d->scissor[0] ? d->scissor[2] - d->scissor[0] : 0, sy = d->scissor[3] > d->scissor[1] ? d->scissor[3] - d->scissor[1] : 0;
            bool bl = false; for (UInt32 k = 0; k < d->nrt && k < XE_DRAW_MAX_RT; k++) bl |= d->blend[k].enable != 0;
            snprintf(slow, sizeof(slow), "%u us, %u draws in the job, first: %u pixels, %u vertices x %u, clear 0x%x%s, scissor %ux%u of %ux%u, %u tex, %s%s%s, vs %llx ps %llx",
                     gpuTsPerMs ? (UInt32)((UInt64)ticks * 1000 / gpuTsPerMs) : 0, nd, pix, d->vertexCount, d->instanceCount, d->clearMask,
                     d->ds.clearDepth || d->ds.clearStencil ? "+ds" : "", sx, sy, d->width, d->height, d->ntex, bl ? "blend " : "",
                     d->ds.depthVA ? "depth " : "", d->ds.stencilVA ? "stencil" : "", d->vsVA, d->psVA);
        }
        cnt += nd;
        if (cnt >= 100) {
            char pr[300];
            UInt64 busyUS = gpuTsPerMs ? sumTicks * 1000 / gpuTsPerMs / cnt : 0;
            snprintf(pr, sizeof(pr), "last %u draws in %u jobs: %llu us each, job %llu us from start to interrupt, GPU drawing %llu us (%llu ticks), %llu pixels each, %u dwords last, %s",
                     cnt, sumJobs, sumAll / cnt, sumGPU / cnt, busyUS, sumTicks / cnt, sumPix / cnt, n, gpuDrawWC ? "WC" : "UC");
            setProperty("IrisXe-DrawPerf", pr);
            setProperty("IrisXe-DrawSlowest", slow);
            {   char tb[120];                                       // v0.56.8
                snprintf(tb, sizeof(tb), "%s: %u later draws in a job without the job-wide setup",
                         sBatchTrim ? "on" : "off (-irisxebatchfull or templates changed)", sTrimmed);
                setProperty("IrisXe-BatchTrim", tb); }
            sumAll = sumGPU = sumTicks = sumPix = 0; cnt = 0; maxTicks = 0; sumJobs = 0;
        }
    }
    {   // v0.60.1: IrisXe-DrawPhases
        uint64_t te; clock_get_uptime(&te);
        phAdd(PH_STATS, phNs(tWoke, te));
        if (++sPhJobs >= 200) {
            static const char *nm[PH_N] = { "check", "build", "copy", "submit", "gpu", "wake", "stats" };
            char t[700]; size_t o = snprintf(t, sizeof(t), "last %u jobs, per job (avg / max us):", sPhJobs);
            for (int k = 0; k < PH_N && o < sizeof(t); k++)
                o += snprintf(t + o, sizeof(t) - o, " %s %llu / %llu%s", nm[k], sPh[k] / sPhJobs / 1000, sPhMax[k] / 1000, k + 1 < PH_N ? "," : "");
            if (o < sizeof(t)) o += snprintf(t + o, sizeof(t) - o, " | state copies %s", mtlShadow[0] ? "on" : "off (-irisxenoshadow)");
            if (o < sizeof(t)) o += snprintf(t + o, sizeof(t) - o, " | v0.63.1 flushes between draws: %llu skipped, %llu needed", sFlushSkipped, sFlushDone);
            if (o < sizeof(t)) snprintf(t + o, sizeof(t) - o, " | v0.68.37 job end seen by spinning: %llu, slept: %llu", sSpinHits, sSpinMiss);
            setProperty("IrisXe-DrawPhases", t);
            for (int k = 0; k < PH_N; k++) sPh[k] = sPhMax[k] = 0;
            sPhJobs = 0;
        }
    }
    // v0.60.0: CS_DEBUG_MODE2 once (it was read after every job: a forcewake handshake each time), and the status
    // line every 64 jobs instead of after every one
    if (!mtlDbgMode2) { if (gpuForcewake(true)) mtlDbgMode2 = RD(0x20d8) | 0x80000000u; gpuForcewake(false); }
    static UInt32 stN;
    if ((++stN & 63) == 0) gpuStatus(nullptr);
    return 0;
}

#include "IrisXeVideo.inc"      // v0.67.0: the video engine (VCS0) test

IOReturn IrisXeProbe::mtlSpaceOp(UInt32 op, void *argp) {
    XeSpaceArgs *a = (XeSpaceArgs *)argp;
    if (!a || !mtlLock || !mtlJobLock || !gpuVA) return kIOReturnBadArgument;
    a->result = -1;
    switch (op) {
    case kXeSpaceVersion:
        a->result = IRISXE_SPACE_ABI;
        return kIOReturnSuccess;

    case kXeSpaceCreate: {
        MtlSpace *sp = (MtlSpace *)IOMallocZero(sizeof(MtlSpace));
        if (!sp) return kIOReturnNoMemory;
        IOLockLock(mtlLock);
        UInt32 id = 1;
        while (id < IRISXE_MAX_SPACES && mtlSpaces[id]) id++;
        if (id < IRISXE_MAX_SPACES) { mtlSpaces[id] = sp; mtlSpaceStat[0]++; mtlSpaceStat[3]++; }
        IOLockUnlock(mtlLock);
        if (id >= IRISXE_MAX_SPACES) { IOFree(sp, sizeof(MtlSpace)); return kIOReturnNoResources; }
        a->space = id; a->result = 0;
        gpuStatus(nullptr);
        return kIOReturnSuccess;
    }

    case kXeSpaceDestroy: {
        UInt32 id = a->space;
        if (!id || id >= IRISXE_MAX_SPACES) return kIOReturnBadArgument;
        IOLockLock(mtlJobLock); mtlFinishAsync();
        bool gpuDone = mtlLastDone || (SInt32)((mtlLastRender ? rcsSeqDone : gpuSeqDone) - mtlLastSeq) >= 0;
        IOLockLock(mtlLock);
        MtlSpace *sp = mtlSpaces[id];
        IOReturn r = kIOReturnNotFound;
        if (sp) {
            if (mtlCur == id) {
                if (gpuDone) mtlPointPD(0);
                else {
                    // a timed-out job of this space may still run: keep its
                    // tables (a small leak) rather than let the GPU walk freed memory
                    mtlSpaces[id] = nullptr; mtlSpaceStat[3]--; mtlSpaceStat[4]++;
                    sp = nullptr;
                    mtlCur = 0xFFFF;                  // nothing valid: next job re-points the PD
                }
            }
            if (sp) {
                mtlSpaces[id] = nullptr; mtlSpaceStat[1]++; mtlSpaceStat[3]--;
                if (mtlMapped >= sp->mapped) mtlMapped -= sp->mapped; else mtlMapped = 0;
            }
            r = kIOReturnSuccess;
        }
        IOLockUnlock(mtlLock);
        IOLockUnlock(mtlJobLock);
        if (sp) {
            for (UInt32 t = 0; t < MTL_SPACE_PTS; t++) if (sp->ptMem[t]) sp->ptMem[t]->release();
            for (UInt32 h = 0; h < MTL_HI_PDS; h++) if (sp->pdMem[h]) sp->pdMem[h]->release();
            IOFree(sp, sizeof(MtlSpace));
        }
        if (r == kIOReturnSuccess) { a->result = 0; gpuStatus(nullptr); mtlSpacePublish(); }
        return r;
    }

    case kXeSpaceMap:
    case kXeSpaceUnmap: {
        bool map = op == kXeSpaceMap;
        if ((a->va & 0xFFF) || !a->count || (map && !a->pa) ||
            !mtlVAOK(a->va, (UInt64)a->count * 4096)) return kIOReturnBadArgument;
        // v0.43.0: the GPU's page tables only change while no job runs (Maps' other threads map and unmap
        // memory while a draw is on the GPU): wait for the job lock
        // v0.68.41: only for UNMAPPING. A new mapping only fills entries no queued or running job uses (every draw's
        // memory is mapped before its job is queued, and every job starts with a TLB invalidate), so it no longer
        // waits for the GPU - with the GPU worker, Maps' thread waited here for the worker's job before nearly every
        // draw (memory lookups 4.5 ms per command buffer). mtlLock still orders it with space switches.
        // Boot-arg -irisxemapwait: wait as before.
        static int mapWait = -1;
        if (mapWait < 0) mapWait = PE_parse_boot_argn("-irisxemapwait", nullptr, 0) ? 1 : 0;
        const bool needJobLock = !map || mapWait;
        if (needJobLock) {
            if (!IOLockTryLock(mtlJobLock)) { mapsDuringJob++; IOLockLock(mtlJobLock); }
            mtlFinishAsync();                              // v0.63.0: never change tables under a running async job
        }
        IOLockLock(mtlLock);
        bool ok = true;
        for (UInt32 i = 0; i < a->count; i++) {
            UInt64 v = a->va + (UInt64)i * 4096;
            volatile UInt32 *pt = mtlTable(a->space, v, map);
            if (!pt) { if (map) { ok = false; break; } continue; }
            UInt32 k = (UInt32)((v >> 12) & 511);
            MtlSpace *sp = a->space && a->space < IRISXE_MAX_SPACES ? mtlSpaces[a->space] : nullptr;
            // v0.68.36: an unmapped page of a process space points at the scratch page again
            UInt64 e = map ? ((a->pa[i] & 0x0000007FFFFFF000ULL) | 3) : (sp && scratchReady() ? sScratchPTE : 0);
            UInt64 pg = v >> 12, bit = 1ULL << (pg & 63);
            bool was = sp ? (sp->present[pg >> 6] & bit) != 0 : (pt[k * 2] & 1);   // v0.57.6: no uncached read
            pt[k * 2] = (UInt32)e; pt[k * 2 + 1] = (UInt32)(e >> 32);
            if (sp) { if (map) sp->present[pg >> 6] |= bit; else sp->present[pg >> 6] &= ~bit; }
            bool hiPg = v >= IRISXE_HI_VA;
            if (map && !was) { mtlMapped++; if (sp) { sp->mapped++; if (hiPg) sp->hiMapped++;
                                                      if (sp->mapped > sp->peakMapped) sp->peakMapped = sp->mapped; } }
            if (!map && was) { if (mtlMapped) mtlMapped--; if (sp && sp->mapped) sp->mapped--; if (sp && hiPg && sp->hiMapped) sp->hiMapped--; }
        }
        __asm__ volatile("mfence" ::: "memory");
        if (!ok) mtlMapFails++;
        IOLockUnlock(mtlLock);
        if (needJobLock) IOLockUnlock(mtlJobLock);
        if (!ok || (++mtlSpacePubTick & 255) == 0) mtlSpacePublish();
        a->result = ok ? 0 : -1;
        return ok ? kIOReturnSuccess : kIOReturnNoMemory;
    }

    case kXeSpaceVcsTest: {                                // v0.67.0: the video engine, one test job (polled)
        static char rep[512];
        IOLockLock(mtlJobLock);                            // no Metal job submits meanwhile (forcewake)
        mtlFinishAsync();
        rep[0] = 0;
        IOReturn r = vcsTest(rep, sizeof(rep));
        IOLockUnlock(mtlJobLock);
        setProperty("IrisXe-VCS", rep);
        IOLog("IrisXeGPU: VCS test: %s\n", rep);
        if (a->va && a->len) strlcpy((char *)(uintptr_t)a->va, rep, (size_t)a->len);
        a->result = r == kIOReturnSuccess ? 0 : r == kIOReturnTimeout ? -3 : r == kIOReturnUnsupported ? -4 : -1;
        return kIOReturnSuccess;
    }
    case kXeSpaceWaitSeq: {                                // v0.63.0: IrisXeAccel's completion thread (no lock)
        UInt32 seq = a->count, resets0 = gpuResets, tmo = a->timeoutMS ? a->timeoutMS : 2000, waited = 0;
        uint64_t t0; clock_get_uptime(&t0);
        while ((SInt32)(rcsSeqDone - seq) < 0) {
            if (gpuResets != resets0) { a->result = -9; return kIOReturnSuccess; }
            if (waited >= tmo) { a->result = -3; return kIOReturnTimeout; }
            waited = gpuWait(true, seq, t0);
        }
        a->result = 0;
        return kIOReturnSuccess;
    }

    case kXeSpaceIsMapped: {
        if (!a->len || !mtlVAOK(a->va, a->len)) return kIOReturnNotFound;
        IOLockLock(mtlLock);
        bool ok = true;
        MtlSpace *sp = a->space && a->space < IRISXE_MAX_SPACES ? mtlSpaces[a->space] : nullptr;
        UInt64 pg = a->va >> 12, last = (a->va + a->len - 1) >> 12;
        if (sp) {       // v0.57.6: the cached copy, 64 pages per word (was: one uncached RAM read per page)
            while (ok && pg <= last) {
                if ((pg & 63) == 0 && pg + 63 <= last) { ok = sp->present[pg >> 6] == ~0ULL; pg += 64; continue; }
                ok = (sp->present[pg >> 6] >> (pg & 63)) & 1; pg++;
            }
        } else if (a->space) ok = false;
        else for (; ok && pg <= last; pg++) {
            volatile UInt32 *pt = mtlTable(0, pg << 12, false);
            ok = pt && (pt[(pg & 511) * 2] & 1);
        }
        IOLockUnlock(mtlLock);
        a->result = ok ? 0 : -1;
        return ok ? kIOReturnSuccess : kIOReturnNotFound;
    }

    case kXeSpaceRun:
    case kXeSpaceDraw:
    case kXeSpaceCompute: {
        IOLockLock(mtlJobLock); mtlFinishAsync();
        int err = 0;
        if (!mtlSwitchSpace(a->space, &err)) {
            IOLockUnlock(mtlJobLock);
            a->result = err;
            return err == -4 ? kIOReturnBusy : kIOReturnNotFound;
        }
        UInt32 seq = 0;
        mtlLastDone = false;
        bool render = op == kXeSpaceCompute || op == kXeSpaceDraw;
        int r = op == kXeSpaceDraw ? mtlRunDraw3D(a, &seq) : render ? mtlRunCompute(a, &seq) : mtlRunBlit(a->cmds, a->count, a->timeoutMS, &seq);
        if (seq) { mtlLastSeq = seq; mtlLastRender = render; }
        mtlLastDone = r == 0 || seq == 0;              // not submitted at all = nothing running
        if (op == kXeSpaceDraw && mtlAsyncPend) { mtlLastDone = false; a->va = mtlAsyncSeq; }   // v0.63.0: still running
        else if (op == kXeSpaceDraw) a->va = 0;
        IOLockUnlock(mtlJobLock);
        a->result = r;
        return r == 0 ? kIOReturnSuccess : kIOReturnError;
    }
    }
    return kIOReturnUnsupported;
}

// ---- v0.59.0: direct scanout --------------------------------------------------------------------------------
#define PLANE_SURFLIVE_1_A  0x701AC
#define PIPEDSL_A           0x70000                       // scanline (bits 12:0)
#define PIPE_FRMCOUNT_A     0x70040                       // +1 at every vertical blank of pipe A
static_assert(XE_SCAN_GGTT + XE_SCAN_SLOTS * XE_SCAN_SLOT_SIZE <= G_GGTT_ADDR, "scanout slots below our GGTT pages");
void IrisXeProbe::scanPublish() {
    char t[200];
    snprintf(t, sizeof(t), "slots mapped %u, flips %u, back to own framebuffer %u | own framebuffer 0x%x, now showing 0x%x | gen %u",
             scanMaps, scanFlips, scanHomes, scanHomeSurf, gpuMMIO ? (RD(PLANE_SURFLIVE_1_A) & ~0xFFFu) : 0, scanGen);
    setProperty("IrisXe-Scanout", t);
}

IOReturn IrisXeProbe::scanOp(void *argp) {
    XeScanArgs *a = (XeScanArgs *)argp;
    if (!a) return kIOReturnBadArgument;
    a->result = -2;
    if (!gpuMMIO || !gpuUp) return kIOReturnNotReady;
    a->result = -1;
    switch (a->op) {
    case kXeScanVersion:
        a->result = XE_SCAN_ABI; a->gen = scanGen;
        return kIOReturnSuccess;
    case kXeScanMap: {
        if (a->slot >= XE_SCAN_SLOTS || !a->pa || !a->count || a->count > XE_SCAN_SLOT_SIZE / 4096) return kIOReturnBadArgument;
        if (!scanHomeSaved) {      // the panel's own framebuffer and the cache bits its GGTT entry uses
            scanHomeSurf = RD(TGL_PLANE_SURF_1_A) & ~0xFFFu;
            scanHomeStride = RD(TGL_PLANE_STRIDE_1_A);
            UInt32 lo = RD(GGTT_BASE + (scanHomeSurf >> 12) * 8);
            if (lo & 1) scanPteFlags = lo & 0xFFF;
            scanHomeSaved = true;
        }
        UInt32 base = XE_SCAN_GGTT + a->slot * XE_SCAN_SLOT_SIZE, idx = base >> 12;
        for (UInt32 i = 0; i < a->count; i++) {
            UInt64 pte = (a->pa[i] & 0x0000007FFFFFF000ULL) | scanPteFlags | 1;
            WR(GGTT_BASE + (idx + i) * 8,     (UInt32)pte);
            WR(GGTT_BASE + (idx + i) * 8 + 4, (UInt32)(pte >> 32));
        }
        (void)RD(GGTT_BASE + (idx + a->count - 1) * 8);
        WR(GFX_FLSH_CNTL, 1);
        (void)RD(GFX_FLSH_CNTL);
        scanMaps++;
        a->addr = base; a->gen = scanGen; a->result = 0;
        scanPublish();
        return kIOReturnSuccess;
    }
    case kXeScanFlip: {
        if (a->gen != scanGen) { a->result = -3; return kIOReturnNotReady; }
        if (!scanHomeSaved || a->addr < XE_SCAN_GGTT || a->addr >= XE_SCAN_GGTT + XE_SCAN_SLOTS * XE_SCAN_SLOT_SIZE ||
            (a->addr & 0x3FFFF) || !a->stride || (a->stride & 63) || a->stride / 64 > 0x7FF) return kIOReturnBadArgument;
        UInt32 st = (RD(TGL_PLANE_STRIDE_1_A) & ~0x7FFu) | (a->stride / 64);
        if (st != RD(TGL_PLANE_STRIDE_1_A)) WR(TGL_PLANE_STRIDE_1_A, st);
        WR(TGL_PLANE_SURF_1_A, a->addr);                  // armed: taken at the next vertical blank
        scanFlips++;
        if ((scanFlips & 1023) == 1) scanPublish();
        a->result = 0;
        return kIOReturnSuccess;
    }
    case kXeScanLive:
        a->addr = RD(PLANE_SURFLIVE_1_A) & ~0xFFFu; a->result = 0;
        return kIOReturnSuccess;
    case kXeScanFrame:
        a->addr = RD(PIPE_FRMCOUNT_A); a->stride = RD(PIPEDSL_A) & 0x1FFF; a->result = 0;
        return kIOReturnSuccess;
    case kXeScanHome:
        if (scanHomeSaved) {
            WR(TGL_PLANE_STRIDE_1_A, scanHomeStride);
            WR(TGL_PLANE_SURF_1_A, scanHomeSurf);
            scanHomes++;
            scanPublish();
        }
        a->addr = scanHomeSurf; a->result = 0;
        return kIOReturnSuccess;
    }
    return kIOReturnUnsupported;
}

IOReturn IrisXeProbe::callPlatformFunction(const OSSymbol *fn, bool wait, void *p1, void *p2, void *p3, void *p4) {
    if (fn && fn->isEqualTo("IrisXeMapPages"))
        return mtlMapPages((UInt64)(uintptr_t)p1, (const UInt64 *)p2, (UInt32)(uintptr_t)p3) ? kIOReturnSuccess : kIOReturnBadArgument;
    if (fn && fn->isEqualTo("IrisXeUnmapPages")) {
        mtlUnmapPages((UInt64)(uintptr_t)p1, (UInt32)(uintptr_t)p2);
        return kIOReturnSuccess;
    }
    if (fn && fn->isEqualTo("IrisXeCheckMapped")) {     // every page of [va, va+len) has a PTE?
        UInt64 va = (UInt64)(uintptr_t)p1, len = (UInt64)(uintptr_t)p2;
        if (!mtlPT || !len || va + len > MTL_VA_LIMIT) return kIOReturnNotFound;
        for (UInt64 pg = va >> 12; pg <= (va + len - 1) >> 12; pg++)
            if (!(mtlPT[pg * 2] & 1)) return kIOReturnNotFound;
        return kIOReturnSuccess;
    }
    if (fn && fn->isEqualTo("IrisXeScanout"))           // v0.59.0 (IrisXeSpaceABI.h)
        return scanOp(p1);
    if (fn && fn->isEqualTo("IrisXeSpace"))             // milestone 2e (IrisXeSpaceABI.h)
        return mtlSpaceOp((UInt32)(uintptr_t)p1, p2);
    if (fn && fn->isEqualTo("IrisXeRunBlit")) {         // old call: shared space 0
        XeSpaceArgs a = {};
        a.space = 0; a.cmds = (const UInt32 *)p1; a.count = (UInt32)(uintptr_t)p2; a.timeoutMS = (UInt32)(uintptr_t)p3;
        mtlSpaceOp(kXeSpaceRun, &a);
        int r = a.result;
        if (p4) *(int *)p4 = r;
        return r == 0 ? kIOReturnSuccess : kIOReturnError;
    }
    return IOService::callPlatformFunction(fn, wait, p1, p2, p3, p4);
}
