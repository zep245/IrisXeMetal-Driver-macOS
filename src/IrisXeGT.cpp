// IrisXeGT - GPU engines, step 2: run our first job on the blitter engine
// (BCS0) with execlist submission, no GuC firmware (same as Linux i915 on TGL).
//
// What it does (boot-arg -irisxeblit):
//   1. allocates 128 KB: engine status page, a context image, a zero PML4 page
//      and a 64 KB command ring; maps them into the GPU address space (GGTT)
//   2. builds the context image (two MI_LOAD_REGISTER_IMM blocks, layout from
//      i915 gt/intel_lrc.c gen12_xcs_offsets) pointing at our ring
//   3. fills the ring with commands: MI_STORE_DATA_IMM (GGTT, privileged) that
//      paint a 64x64 green square into the boot framebuffer, then a "done"
//      marker written into our status page
//   4. switches the engine to execlist mode, submits the context through the
//      ELSQ, waits for the marker, and reports everything in IrisXe-Blit
//
// Commands go through the GGTT only (privileged context), so no page tables
// are needed yet; the PML4 points at an empty page.

#include "IrisXeProbe.hpp"
#include "TGLRegs.h"
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <libkern/libkern.h>
#include <string.h>

#define BCS_BASE            0x22000
#define RING_TAIL           0x30
#define RING_HEAD           0x34
#define RING_START          0x38
#define RING_CTL            0x3C
#define RING_IPEHR          0x68
#define RING_ACTHD          0x74
#define RING_HWS_PGA        0x80
#define RING_HWSTAM         0x98
#define RING_MI_MODE        0x9C
#define RING_ESR            0xB8
#define RING_EL_STATUS_LO   0x234
#define RING_EL_STATUS_HI   0x238
#define RING_MODE_GEN7      0x29C
#define RING_CSB_PTR        0x3A0
#define RING_EL_SQ          0x510
#define RING_EL_CONTROL     0x550

#define MASKED_ON(b)        (((b) << 16) | (b))
#define MASKED_OFF(b)       ((b) << 16)
#define STOP_RING           (1u << 8)
#define GFX_DISABLE_LEGACY  (1u << 3)

#define MI_INSTR(op, f)     (((op) << 23) | (f))
#define MI_NOOP             0
#define MI_LRI(n)           MI_INSTR(0x22, 2 * (n) - 1)
#define MI_LRI_FORCE_POSTED (1u << 12)
#define MI_LRI_CS_MMIO      (1u << 19)
#define MI_BB_END           MI_INSTR(0x0A, 0)
#define MI_SDI(len)         MI_INSTR(0x20, (len))
#define MI_USE_GGTT         (1u << 22)
#define MI_SDI_QWORD        (1u << 21)

#define CTX_CTRL_INHIBIT_SYN_CTX_SWITCH   (1u << 3)
#define CTX_CTRL_ENGINE_CTX_RESTORE_INHIBIT (1u << 0)

#define GGTT_BASE           0x800000
#define GFX_FLSH_CNTL       0x101008

#define GT_BYTES            (128 * 1024)
#define GT_PAGES            (GT_BYTES / 4096)
#define PG_HWSP             0
#define PG_LRC              1          // 1 ppHWSP page + 3 state pages
#define PG_PML4             5
#define PG_RING             6          // 16 pages = 64 KB
#define RING_BYTES          (16 * 4096)
#define GT_GGTT_ADDR        0xF0400000u   // IrisXeFB's cursor sits at 0xF0000000

#define HWSP_DONE_DW        0x40
#define DONE_MAGIC          0x1E15C0DEu
#define SQUARE              64
#define SQUARE_X            96
#define SQUARE_Y            96
#define SQUARE_COLOR        0xFF00E060u

// i915 gen12_xcs_offsets: 13 + 9 registers (offsets from the engine base)
static const UInt16 kXcsLRI1[] = { 0x244, 0x034, 0x030, 0x038, 0x03c, 0x168, 0x140,
                                   0x110, 0x1c0, 0x1c4, 0x1c8, 0x180, 0x2b4 };
static const UInt16 kXcsLRI2[] = { 0x3a8, 0x28c, 0x288, 0x284, 0x280, 0x27c, 0x278,
                                   0x274, 0x270 };
// dword indices of the values inside the state page (intel_lrc_reg.h)
#define CTX_CONTEXT_CONTROL 0x03
#define CTX_RING_HEAD       0x05
#define CTX_RING_TAIL       0x07
#define CTX_RING_START      0x09
#define CTX_RING_CTL        0x0B
#define CTX_PDP0_UDW        0x31
#define CTX_PDP0_LDW        0x33

void IrisXeProbe::gtBlitTest(volatile UInt8 *mmio, UInt64 mmioLen) {
    #define RD(off) (*(volatile UInt32 *)(mmio + (off)))
    #define WR(off, v) (*(volatile UInt32 *)(mmio + (off)) = (v))
    char rep[256] = "";
    OSDictionary *d = OSDictionary::withCapacity(24);
    if (!d) return;
    auto put = [&](const char *k, UInt32 v) {
        OSNumber *n = OSNumber::withNumber(v, 32);
        if (n) { d->setObject(k, n); n->release(); }
    };
    auto puts = [&](const char *k, const char *v) {
        OSString *s = OSString::withCString(v);
        if (s) { d->setObject(k, s); s->release(); }
    };
    auto finish = [&](const char *result) {
        puts("Result", result);
        setProperty("IrisXe-Blit", d);
        d->release();
    };

    if (mmioLen < GGTT_BASE + (GT_GGTT_ADDR >> 12) * 8 + GT_PAGES * 8) { finish("BAR too small"); return; }

    // --- memory (uncached for the CPU, so GPU writes are visible directly) ---
    gtMem = IOBufferMemoryDescriptor::withOptions(
        kIODirectionInOut | kIOMemoryPhysicallyContiguous | kIOMapInhibitCache, GT_BYTES, 4096);
    if (!gtMem) { finish("no memory"); return; }
    volatile UInt32 *va = (volatile UInt32 *)gtMem->getBytesNoCopy();
    IOByteCount seg = 0;
    UInt64 phys = gtMem->getPhysicalSegment(0, &seg, kIOMemoryMapperNone);
    if (!va || !phys || seg < GT_BYTES) { finish("memory not contiguous"); return; }
    for (UInt32 i = 0; i < GT_BYTES / 4; i++) va[i] = 0;
    auto pageVA = [&](UInt32 pg) { return va + pg * 1024; };
    auto ggtt   = [&](UInt32 pg) { return GT_GGTT_ADDR + pg * 4096; };

    // --- GGTT: our 32 pages at GT_GGTT_ADDR ---
    UInt32 idx = GT_GGTT_ADDR >> 12;
    for (UInt32 i = 0; i < GT_PAGES; i++) {
        UInt64 pte = (phys + (UInt64)i * 4096) | 1;     // present
        WR(GGTT_BASE + (idx + i) * 8,     (UInt32)pte);
        WR(GGTT_BASE + (idx + i) * 8 + 4, (UInt32)(pte >> 32));
    }
    (void)RD(GGTT_BASE + idx * 8);
    WR(GFX_FLSH_CNTL, 1);
    (void)RD(GFX_FLSH_CNTL);

    // --- the job: green square into the boot framebuffer, then the marker ---
    volatile UInt32 *ring = pageVA(PG_RING);
    UInt32 n = 0;
    UInt32 planeCtl = RD(TGL_PLANE_CTL_1_A);
    UInt32 surf     = RD(TGL_PLANE_SURF_1_A) & ~0xFFFu;
    UInt32 stride   = (RD(TGL_PLANE_STRIDE_1_A) & 0x7FF) * 64;
    bool linear = (planeCtl & TGL_PLANE_CTL_ENABLE) && !(planeCtl & TGL_PLANE_CTL_TILING_MASK) && stride;
    if (linear) {
        for (UInt32 y = 0; y < SQUARE; y++) {
            for (UInt32 x = 0; x < SQUARE; x += 2) {
                UInt32 a = surf + (SQUARE_Y + y) * stride + (SQUARE_X + x) * 4;
                ring[n++] = MI_SDI(3) | MI_USE_GGTT | MI_SDI_QWORD;
                ring[n++] = a;
                ring[n++] = 0;
                ring[n++] = SQUARE_COLOR;
                ring[n++] = SQUARE_COLOR;
            }
        }
    }
    ring[n++] = MI_SDI(2) | MI_USE_GGTT;
    ring[n++] = ggtt(PG_HWSP) + HWSP_DONE_DW * 4;
    ring[n++] = 0;
    ring[n++] = DONE_MAGIC;
    while (n & 1) ring[n++] = MI_NOOP;                  // tail must be 8-byte aligned
    UInt32 tail = n * 4;

    // --- context image: state page right after the per-process HWSP page ---
    volatile UInt32 *regs = pageVA(PG_LRC + 1);
    UInt32 i = 1;                                       // NOP(1)
    regs[i++] = MI_LRI(13) | MI_LRI_FORCE_POSTED | MI_LRI_CS_MMIO;
    for (UInt16 off : kXcsLRI1) { regs[i] = BCS_BASE + off; i += 2; }
    i += 5;                                             // NOP(5)
    regs[i++] = MI_LRI(9) | MI_LRI_FORCE_POSTED | MI_LRI_CS_MMIO;
    for (UInt16 off : kXcsLRI2) { regs[i] = BCS_BASE + off; i += 2; }
    regs[i] = MI_BB_END | 1;
    regs[CTX_CONTEXT_CONTROL] = MASKED_ON(CTX_CTRL_INHIBIT_SYN_CTX_SWITCH) |
                                MASKED_OFF(CTX_CTRL_ENGINE_CTX_RESTORE_INHIBIT) |
                                CTX_CTRL_ENGINE_CTX_RESTORE_INHIBIT;   // fresh context
    regs[CTX_RING_HEAD]  = 0;
    regs[CTX_RING_TAIL]  = tail;
    regs[CTX_RING_START] = ggtt(PG_RING);
    regs[CTX_RING_CTL]   = ((RING_BYTES - 4096) & 0x1FF000) | 1;       // size | valid
    UInt64 pml4 = phys + PG_PML4 * 4096;
    regs[CTX_PDP0_UDW]   = (UInt32)(pml4 >> 32);
    regs[CTX_PDP0_LDW]   = (UInt32)pml4;
    __asm__ volatile("mfence" ::: "memory");

    // descriptor: LRCA | 64-bit legacy addressing | valid | privileged,
    // upper: sw context id 1, engine class 3 (copy) instance 0
    UInt32 descLo = ggtt(PG_LRC) | (3u << 3) | (1u << 8) | 1u;
    UInt32 descHi = (1u << 5) | (3u << 29);

    // --- engine up: keep forcewake for the whole test ---
    static const UInt32 fwReq[] = { 0xA278, 0xA188 }, fwAck[] = { 0x0D84, 0x130044 };
    bool fwOK = true;
    for (UInt32 k = 0; k < 2; k++) {
        WR(fwReq[k], MASKED_ON(1u));
        UInt32 t = 0;
        while (!(RD(fwAck[k]) & 1) && t < 50000) { IODelay(10); t += 10; }
        if (!(RD(fwAck[k]) & 1)) fwOK = false;
    }
    if (!fwOK) {
        for (UInt32 k = 0; k < 2; k++) WR(fwReq[k], MASKED_OFF(1u));
        finish("forcewake failed");
        return;
    }

    WR(BCS_BASE + RING_HWSTAM, 0xFFFFFFFF);
    WR(BCS_BASE + RING_MODE_GEN7, MASKED_ON(GFX_DISABLE_LEGACY));
    WR(BCS_BASE + RING_MI_MODE, MASKED_OFF(STOP_RING));
    WR(BCS_BASE + RING_HWS_PGA, ggtt(PG_HWSP));
    (void)RD(BCS_BASE + RING_HWS_PGA);
    put("ModeAfterEnable", RD(BCS_BASE + RING_MODE_GEN7));

    // submit: ELSQ port 1 empty, port 0 = our context, then LOAD
    WR(BCS_BASE + RING_EL_SQ + 8,  0);
    WR(BCS_BASE + RING_EL_SQ + 12, 0);
    WR(BCS_BASE + RING_EL_SQ + 0,  descLo);
    WR(BCS_BASE + RING_EL_SQ + 4,  descHi);
    WR(BCS_BASE + RING_EL_CONTROL, 1);

    volatile UInt32 *hwsp = pageVA(PG_HWSP);
    UInt32 waited = 0;
    while (hwsp[HWSP_DONE_DW] != DONE_MAGIC && waited < 200000) { IODelay(20); waited += 20; }
    bool done = (hwsp[HWSP_DONE_DW] == DONE_MAGIC);
    IODelay(200);                                       // let the context switch out

    put("WaitedUS", waited);
    put("TailBytes", tail);
    put("SquareDrawn", linear ? 1 : 0);
    put("HEAD", RD(BCS_BASE + RING_HEAD));
    put("TAIL", RD(BCS_BASE + RING_TAIL));
    put("CTL", RD(BCS_BASE + RING_CTL));
    put("START", RD(BCS_BASE + RING_START));
    put("ACTHD", RD(BCS_BASE + RING_ACTHD));
    put("IPEHR", RD(BCS_BASE + RING_IPEHR));
    put("ESR", RD(BCS_BASE + RING_ESR));
    put("EL_STATUS_LO", RD(BCS_BASE + RING_EL_STATUS_LO));
    put("EL_STATUS_HI", RD(BCS_BASE + RING_EL_STATUS_HI));
    put("CSB_PTR", RD(BCS_BASE + RING_CSB_PTR));
    put("MI_MODE", RD(BCS_BASE + RING_MI_MODE));
    char csb[200] = "";
    for (UInt32 k = 0; k < 6; k++) {                    // first 6 context-status events
        char e[32];
        snprintf(e, sizeof(e), "%08x:%08x ", hwsp[0x10 + 2 * k], hwsp[0x11 + 2 * k]);
        strlcat(csb, e, sizeof(csb));
    }
    puts("CSB", csb);
    put("CSB_WRITE", hwsp[0x2F]);

    for (UInt32 k = 0; k < 2; k++) WR(fwReq[k], MASKED_OFF(1u));

    // memory stays allocated: the engine still points at the status page
    snprintf(rep, sizeof(rep), "%s after %u us (%u commands)",
             done ? "GPU RAN OUR JOB" : "no marker (job did not finish)",
             waited, n / 5);
    finish(rep);
    #undef RD
    #undef WR
}

// ===========================================================================
// Step 3 (boot-arg -irisxeppgtt): real GPU memory + real blitter commands.
//   * builds 4-level GPU page tables (PPGTT): GPU address 0x10000000 maps the
//     boot framebuffer (physical pages taken from its GGTT entries)
//   * XY_COLOR_BLT fills a 128x128 blue square through those page tables
//   * XY_FAST_COPY_BLT copies it next to itself (what Linux uses on TGL)
//   * MI_USER_INTERRUPT: the engine raises its "job finished" interrupt; we
//     check it in the GT interrupt registers (master interrupt stays off, so
//     nothing is delivered to the CPU yet)

#define XY_COLOR_BLT_CMD        ((2u << 29) | (0x50u << 22))
#define XY_FAST_COPY_BLT_CMD    ((2u << 29) | (0x42u << 22))
#define BLT_WRITE_RGBA          ((1u << 20) | (2u << 20))
#define BLT_DEPTH_32            (3u << 24)
#define BLT_ROP_COLOR_COPY      (0xF0u << 16)
#define MI_FLUSH_DW_4           (MI_INSTR(0x26, 1) + 1)   // 4-dword form
#define MI_USER_INTERRUPT       MI_INSTR(0x02, 0)

#define P3_BYTES        (96 * 1024)
#define P3_PAGES        (P3_BYTES / 4096)
#define P3_HWSP         0
#define P3_LRC          1        // 4 pages
#define P3_PML4         5
#define P3_PDP          6
#define P3_PD           7
#define P3_PT           8        // 8 pages -> 16 MB mapped
#define P3_RING         16       // 4 pages
#define P3_RING_BYTES   (4 * 4096)
#define P3_GGTT_ADDR    0xF0500000u
#define FB_GPU_VA       0x10000000ULL
#define PDE_RW_P        3ULL

#define GT_INTR_ENABLE_RC   0x190030   // GEN11_RENDER_COPY_INTR_ENABLE
#define GT_INTR_MASK_BCS    0x1900A0   // GEN11_BCS_RSVD_INTR_MASK
#define GT_INTR_DW0         0x190018
#define GT_INTR_IDENTITY0   0x190060
#define GT_IIR_SELECTOR0    0x190070

void IrisXeProbe::gtPPGTTTest(volatile UInt8 *mmio, UInt64 mmioLen) {
    #define RD(off) (*(volatile UInt32 *)(mmio + (off)))
    #define WR(off, v) (*(volatile UInt32 *)(mmio + (off)) = (v))
    OSDictionary *d = OSDictionary::withCapacity(32);
    if (!d) return;
    auto put = [&](const char *k, UInt32 v) {
        OSNumber *n = OSNumber::withNumber(v, 32);
        if (n) { d->setObject(k, n); n->release(); }
    };
    auto puts = [&](const char *k, const char *v) {
        OSString *s = OSString::withCString(v);
        if (s) { d->setObject(k, s); s->release(); }
    };
    auto finish = [&](const char *result) {
        puts("Result", result);
        setProperty("IrisXe-PPGTT", d);
        d->release();
    };

    UInt32 planeCtl = RD(TGL_PLANE_CTL_1_A);
    UInt32 surf     = RD(TGL_PLANE_SURF_1_A) & ~0xFFFu;
    UInt32 stride   = (RD(TGL_PLANE_STRIDE_1_A) & 0x7FF) * 64;
    UInt32 fbH      = (RD(TGL_PIPESRC_A) & 0xFFFF) + 1;
    if (!(planeCtl & TGL_PLANE_CTL_ENABLE) || (planeCtl & TGL_PLANE_CTL_TILING_MASK) || !stride) {
        finish("boot screen is not a linear surface"); return;
    }
    UInt32 fbPages = (stride * fbH + 4095) / 4096;
    if (fbPages > 8 * 512) fbPages = 8 * 512;
    put("FbPages", fbPages);
    if (mmioLen < GGTT_BASE + ((P3_GGTT_ADDR >> 12) + P3_PAGES) * 8) { finish("BAR too small"); return; }

    p3Mem = IOBufferMemoryDescriptor::withOptions(
        kIODirectionInOut | kIOMemoryPhysicallyContiguous | kIOMapInhibitCache, P3_BYTES, 4096);
    if (!p3Mem) { finish("no memory"); return; }
    volatile UInt32 *va = (volatile UInt32 *)p3Mem->getBytesNoCopy();
    IOByteCount seg = 0;
    UInt64 phys = p3Mem->getPhysicalSegment(0, &seg, kIOMemoryMapperNone);
    if (!va || !phys || seg < P3_BYTES) { finish("memory not contiguous"); return; }
    for (UInt32 i = 0; i < P3_BYTES / 4; i++) va[i] = 0;
    auto pageVA = [&](UInt32 pg) { return va + pg * 1024; };
    auto pagePA = [&](UInt32 pg) { return phys + (UInt64)pg * 4096; };
    auto ggtt   = [&](UInt32 pg) { return P3_GGTT_ADDR + pg * 4096; };
    auto put64  = [&](volatile UInt32 *tbl, UInt32 idx, UInt64 v) {
        tbl[idx * 2] = (UInt32)v; tbl[idx * 2 + 1] = (UInt32)(v >> 32);
    };

    // GGTT for our own pages (context, ring, status page)
    UInt32 gidx = P3_GGTT_ADDR >> 12;
    for (UInt32 i = 0; i < P3_PAGES; i++) {
        UInt64 pte = pagePA(i) | 1;
        WR(GGTT_BASE + (gidx + i) * 8,     (UInt32)pte);
        WR(GGTT_BASE + (gidx + i) * 8 + 4, (UInt32)(pte >> 32));
    }
    (void)RD(GGTT_BASE + gidx * 8);
    WR(GFX_FLSH_CNTL, 1);
    (void)RD(GFX_FLSH_CNTL);

    // PPGTT: PML4[0] -> PDP[0] -> PD[128..135] -> 8 page tables -> framebuffer
    put64(pageVA(P3_PML4), 0, pagePA(P3_PDP) | PDE_RW_P);
    put64(pageVA(P3_PDP),  (UInt32)((FB_GPU_VA >> 30) & 511), pagePA(P3_PD) | PDE_RW_P);
    UInt32 pd0 = (UInt32)((FB_GPU_VA >> 21) & 511);
    for (UInt32 t = 0; t < 8; t++)
        put64(pageVA(P3_PD), pd0 + t, pagePA(P3_PT + t) | PDE_RW_P);
    UInt32 fbG = surf >> 12;
    UInt32 firstPTE[2] = {};
    for (UInt32 p = 0; p < fbPages; p++) {
        UInt32 lo = RD(GGTT_BASE + (fbG + p) * 8), hi = RD(GGTT_BASE + (fbG + p) * 8 + 4);
        UInt64 g = ((UInt64)hi << 32) | lo;
        UInt64 addr = g & 0x0000007FFFFFF000ULL;
        UInt64 pte = addr | (g & 0x98) | PDE_RW_P;           // keep the PAT bits (3,4,7)
        put64(pageVA(P3_PT + p / 512), p % 512, pte);
        if (p == 0) { firstPTE[0] = (UInt32)pte; firstPTE[1] = (UInt32)(pte >> 32); }
    }
    char tmp[96];
    snprintf(tmp, sizeof(tmp), "%08x%08x", firstPTE[1], firstPTE[0]);
    puts("FbPTE0", tmp);

    // the job
    volatile UInt32 *ring = pageVA(P3_RING);
    UInt32 n = 0;
    const UInt32 x1 = 200, y1 = 96, sz = 128, x2 = x1 + sz + 32;
    UInt64 fbva = FB_GPU_VA;
    ring[n++] = XY_COLOR_BLT_CMD | BLT_WRITE_RGBA | (7 - 2);       // blue square
    ring[n++] = BLT_DEPTH_32 | BLT_ROP_COLOR_COPY | stride;
    ring[n++] = (y1 << 16) | x1;
    ring[n++] = ((y1 + sz) << 16) | (x1 + sz);
    ring[n++] = (UInt32)fbva;
    ring[n++] = (UInt32)(fbva >> 32);
    ring[n++] = 0xFF2060FFu;
    ring[n++] = MI_NOOP;
    ring[n++] = MI_FLUSH_DW_4; ring[n++] = 0; ring[n++] = 0; ring[n++] = 0;
    ring[n++] = XY_FAST_COPY_BLT_CMD | (10 - 2);                    // copy it right
    ring[n++] = BLT_DEPTH_32 | stride;
    ring[n++] = (y1 << 16) | x2;
    ring[n++] = ((y1 + sz) << 16) | (x2 + sz);
    ring[n++] = (UInt32)fbva;
    ring[n++] = (UInt32)(fbva >> 32);
    ring[n++] = (y1 << 16) | x1;
    ring[n++] = stride;
    ring[n++] = (UInt32)fbva;
    ring[n++] = (UInt32)(fbva >> 32);
    ring[n++] = MI_FLUSH_DW_4; ring[n++] = 0; ring[n++] = 0; ring[n++] = 0;
    ring[n++] = MI_SDI(2) | MI_USE_GGTT;                            // done marker
    ring[n++] = ggtt(P3_HWSP) + HWSP_DONE_DW * 4;
    ring[n++] = 0;
    ring[n++] = DONE_MAGIC;
    ring[n++] = MI_USER_INTERRUPT;
    while (n & 1) ring[n++] = MI_NOOP;
    UInt32 tail = n * 4;

    // context image with the page tables
    volatile UInt32 *regs = pageVA(P3_LRC + 1);
    UInt32 i = 1;
    regs[i++] = MI_LRI(13) | MI_LRI_FORCE_POSTED | MI_LRI_CS_MMIO;
    for (UInt16 off : kXcsLRI1) { regs[i] = BCS_BASE + off; i += 2; }
    i += 5;
    regs[i++] = MI_LRI(9) | MI_LRI_FORCE_POSTED | MI_LRI_CS_MMIO;
    for (UInt16 off : kXcsLRI2) { regs[i] = BCS_BASE + off; i += 2; }
    regs[i] = MI_BB_END | 1;
    regs[CTX_CONTEXT_CONTROL] = MASKED_ON(CTX_CTRL_INHIBIT_SYN_CTX_SWITCH) |
                                MASKED_OFF(CTX_CTRL_ENGINE_CTX_RESTORE_INHIBIT) |
                                CTX_CTRL_ENGINE_CTX_RESTORE_INHIBIT;
    regs[CTX_RING_HEAD]  = 0;
    regs[CTX_RING_TAIL]  = tail;
    regs[CTX_RING_START] = ggtt(P3_RING);
    regs[CTX_RING_CTL]   = ((P3_RING_BYTES - 4096) & 0x1FF000) | 1;
    regs[CTX_PDP0_UDW]   = (UInt32)(pagePA(P3_PML4) >> 32);
    regs[CTX_PDP0_LDW]   = (UInt32)pagePA(P3_PML4);
    __asm__ volatile("mfence" ::: "memory");
    UInt32 descLo = ggtt(P3_LRC) | (3u << 3) | (1u << 8) | 1u;
    UInt32 descHi = (2u << 5) | (3u << 29);                          // sw ctx id 2, copy class

    static const UInt32 fwReq[] = { 0xA278, 0xA188 }, fwAck[] = { 0x0D84, 0x130044 };
    bool fwOK = true;
    for (UInt32 k = 0; k < 2; k++) {
        WR(fwReq[k], MASKED_ON(1u));
        UInt32 t = 0;
        while (!(RD(fwAck[k]) & 1) && t < 50000) { IODelay(10); t += 10; }
        if (!(RD(fwAck[k]) & 1)) fwOK = false;
    }
    if (!fwOK) {
        for (UInt32 k = 0; k < 2; k++) WR(fwReq[k], MASKED_OFF(1u));
        finish("forcewake failed"); return;
    }

    // interrupts: enable + unmask BCS0 user interrupt (no MSI: master stays off)
    put("IntrDW0Before", RD(GT_INTR_DW0));
    WR(GT_INTR_ENABLE_RC, (1u << 16) | 1u);
    WR(GT_INTR_MASK_BCS, ~(1u << 16));

    WR(BCS_BASE + RING_HWSTAM, 0xFFFFFFFF);
    WR(BCS_BASE + RING_MODE_GEN7, MASKED_ON(GFX_DISABLE_LEGACY));
    WR(BCS_BASE + RING_MI_MODE, MASKED_OFF(STOP_RING));
    WR(BCS_BASE + RING_HWS_PGA, ggtt(P3_HWSP));
    (void)RD(BCS_BASE + RING_HWS_PGA);

    WR(BCS_BASE + RING_EL_SQ + 8,  0);
    WR(BCS_BASE + RING_EL_SQ + 12, 0);
    WR(BCS_BASE + RING_EL_SQ + 0,  descLo);
    WR(BCS_BASE + RING_EL_SQ + 4,  descHi);
    WR(BCS_BASE + RING_EL_CONTROL, 1);

    volatile UInt32 *hwsp = pageVA(P3_HWSP);
    UInt32 waited = 0;
    while (hwsp[HWSP_DONE_DW] != DONE_MAGIC && waited < 200000) { IODelay(20); waited += 20; }
    bool done = (hwsp[HWSP_DONE_DW] == DONE_MAGIC);
    IODelay(200);

    // did the interrupt fire?
    UInt32 dw0 = RD(GT_INTR_DW0);
    put("IntrDW0", dw0);
    char irq[160] = "";
    bool userIrq = false;
    for (UInt32 bit = 0; bit < 32; bit++) {
        if (!(dw0 & (1u << bit))) continue;
        WR(GT_IIR_SELECTOR0, 1u << bit);
        UInt32 ident = 0, t = 0;
        do { ident = RD(GT_INTR_IDENTITY0); if (ident & (1u << 31)) break; IODelay(1); } while (++t < 100);
        WR(GT_INTR_IDENTITY0, ident);                   // clear identity
        WR(GT_INTR_DW0, 1u << bit);                     // clear the bank bit
        if (((ident >> 16) & 7) == 3 && (ident & 1)) userIrq = true;   // copy class, user interrupt
        char e[48];
        snprintf(e, sizeof(e), "bit%u: class=%u inst=%u iir=%04x ", bit,
                 (ident >> 16) & 7, (ident >> 20) & 0x3F, ident & 0xFFFF);
        strlcat(irq, e, sizeof(irq));
    }
    puts("Interrupt", irq[0] ? irq : "none latched");

    WR(GT_INTR_ENABLE_RC, 0);
    WR(GT_INTR_MASK_BCS, ~0u);

    put("WaitedUS", waited);
    put("HEAD", RD(BCS_BASE + RING_HEAD));
    put("TAIL", RD(BCS_BASE + RING_TAIL));
    put("TailBytes", tail);
    put("ESR", RD(BCS_BASE + RING_ESR));
    put("IPEHR", RD(BCS_BASE + RING_IPEHR));
    put("ACTHD", RD(BCS_BASE + RING_ACTHD));
    put("EL_STATUS_LO", RD(BCS_BASE + RING_EL_STATUS_LO));
    put("EL_STATUS_HI", RD(BCS_BASE + RING_EL_STATUS_HI));
    put("FAULT", RD(0xCEC4));            // GEN12_RING_FAULT_REG: bit0 = page fault seen
    put("FAULT_TLB0", RD(0xCEB8));
    put("FAULT_TLB1", RD(0xCEBC));
    char csb[160] = "";
    for (UInt32 k = 0; k < 4; k++) {
        char e[32];
        snprintf(e, sizeof(e), "%08x:%08x ", hwsp[0x10 + 2 * k], hwsp[0x11 + 2 * k]);
        strlcat(csb, e, sizeof(csb));
    }
    puts("CSB", csb);

    for (UInt32 k = 0; k < 2; k++) WR(fwReq[k], MASKED_OFF(1u));

    char rep[160];
    snprintf(rep, sizeof(rep), "%s after %u us; interrupt %s",
             done ? "BLITTER JOB DONE (fill + copy via page tables)" : "no marker (job did not finish)",
             waited, userIrq ? "OK" : "not seen");
    finish(rep);
    #undef RD
    #undef WR
}
