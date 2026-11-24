// IrisXeVideo.inc - v0.67.0: the video engine (VCS0 = the media engine's VDBox 0), step V1: a one-shot test.
// Included by IrisXeGPU.cpp (uses its RD / WR, engine and context definitions).
//
// Hardware video decoding (H.264 first) will run Apple's Ice Lake VA driver's command buffers on this engine
// (research rounds 33 / 34). Before that, this checks that the engine exists, wakes up and runs a job:
//   1. the fuse register says whether VDBox 0 is present
//   2. media forcewake (VDBox 0 power domain) is acknowledged
//   3. the engine runs a tiny ring (MI_STORE_DATA_IMM of a magic number into our status page) from its own
//      execlist context (gen12 "xcs" context layout, the same as the blitter's, i915 gen12_xcs_offsets) in
//      IrisXeCore's shared GPU address space; polled, no interrupts (the engine's interrupt stays masked)
// Run with sudo sysctl debug.irisxe_vcstest=1 (IrisXeAccel) -> kXeSpaceVcsTest; report in IrisXe-VCS.
#define VCS0                0x1C0000
#define V_BYTES             (16 * 4096)
#define V_HWSP              0
#define V_LRC               1      // ppHWSP + register state (+ spare)
#define V_RING              8      // 2 pages
#define V_RING_BYTES        (2 * 4096)
#define V_GGTT_ADDR         0xF0800000u
#define V_MAGIC             0x5EC0DEB0u
#define FW_MEDIA_VDBOX0     0xA540         // FORCEWAKE_MEDIA_VDBOX_GEN11(0)
#define FW_ACK_VDBOX0       0x0D50         // FORCEWAKE_ACK_MEDIA_VDBOX_GEN11(0)
#define GT_VDBOX_DISABLE    0x9140         // GEN11_GT_VEBOX_VDBOX_DISABLE: bits 7:0 VDBox disabled, 23:16 VEBox
#define R_ACTHD             0x74
#define R_EL_STATUS         0x234          // RING_EXECLIST_STATUS_LO

IOReturn IrisXeProbe::vcsTest(char *rep, UInt32 repLen) {
    UInt32 o = 0;
    auto say = [&](const char *fmt, auto... args) {
        if (o + 1 < repLen) o += snprintf(rep + o, repLen - o, fmt, args...);
        if (o >= repLen) o = repLen - 1;
    };
    vcsTests++;
    if (!gpuUp || !gpuMMIO || !gpuVA) { say("%s", "not started: GPU core not up"); return kIOReturnNotReady; }
    if (!vcsMem) {
        vcsMem = IOBufferMemoryDescriptor::withOptions(kIODirectionInOut | kIOMemoryPhysicallyContiguous | kIOMapInhibitCache, V_BYTES, 4096);
        if (!vcsMem) { say("%s", "no memory"); return kIOReturnNoMemory; }
        vcsVA = (volatile UInt32 *)vcsMem->getBytesNoCopy();
        IOByteCount seg = 0;
        vcsPhys = vcsMem->getPhysicalSegment(0, &seg, kIOMemoryMapperNone);
        if (!vcsVA || !vcsPhys || seg < V_BYTES) { OSSafeReleaseNULL(vcsMem); vcsVA = nullptr; say("%s", "memory not contiguous"); return kIOReturnNoMemory; }
    }
    for (UInt32 i = 0; i < V_BYTES / 4; i++) vcsVA[i] = 0;
    // GGTT: our 16 pages at V_GGTT_ADDR
    UInt32 gidx = V_GGTT_ADDR >> 12;
    for (UInt32 i = 0; i < V_BYTES / 4096; i++) {
        UInt64 pte = (vcsPhys + (UInt64)i * 4096) | 1;
        WR(GGTT_BASE + (gidx + i) * 8, (UInt32)pte);
        WR(GGTT_BASE + (gidx + i) * 8 + 4, (UInt32)(pte >> 32));
    }
    (void)RD(GGTT_BASE + gidx * 8);
    WR(GFX_FLSH_CNTL, 1);
    (void)RD(GFX_FLSH_CNTL);
    // context image (gen12 xcs layout, as the blitter's) and ring
    volatile UInt32 *regs = vcsVA + (V_LRC + 1) * 1024;
    UInt32 i = 1;
    regs[i++] = MI_LRI(13) | MI_LRI_POSTED | MI_LRI_CS_MMIO;
    for (UInt16 off : kLRI1) { regs[i] = VCS0 + off; i += 2; }
    i += 5;
    regs[i++] = MI_LRI(9) | MI_LRI_POSTED | MI_LRI_CS_MMIO;
    for (UInt16 off : kLRI2) { regs[i] = VCS0 + off; i += 2; }
    regs[i] = MI_BB_END | 1;
    regs[CTX_CONTEXT_CONTROL] = MASKED_ON(CTX_INHIBIT_SYN) | MASKED_ON(CTX_RESTORE_INHIBIT);
    regs[CTX_RING_START] = V_GGTT_ADDR + V_RING * 4096;
    regs[CTX_RING_CTL]   = ((V_RING_BYTES - 4096) & 0x1FF000) | 1;
    UInt64 pml4 = gpuPhys + (UInt64)G_PML4 * 4096;           // IrisXeCore's shared GPU address space
    regs[CTX_PDP0_UDW]   = (UInt32)(pml4 >> 32);
    regs[CTX_PDP0_LDW]   = (UInt32)pml4;
    volatile UInt32 *ring = vcsVA + V_RING * 1024;
    UInt32 t = 0;
    ring[t++] = MI_SDI(2) | MI_USE_GGTT;
    ring[t++] = V_GGTT_ADDR + V_HWSP * 4096 + HWSP_SEQ * 4;
    ring[t++] = 0;
    ring[t++] = V_MAGIC;
    ring[t++] = MI_FLUSH_DW_4; ring[t++] = 0; ring[t++] = 0; ring[t++] = 0;
    while (t & 1) ring[t++] = MI_NOOP;
    regs[CTX_RING_HEAD] = 0;
    regs[CTX_RING_TAIL] = t * 4;
    __asm__ volatile("mfence" ::: "memory");

    // power: GT + render forcewake (fuse register) and the VDBox 0 media domain
    if (!gpuForcewake(true)) { gpuForcewake(false); say("%s", "GT forcewake failed"); return kIOReturnError; }
    UInt32 fuse = RD(GT_VDBOX_DISABLE);
    say("fuse 0x%08x: VDBox 0 %s", fuse, (fuse & 1) ? "DISABLED" : "present");
    if (fuse & 1) { gpuForcewake(false); return kIOReturnUnsupported; }
    WR(FW_MEDIA_VDBOX0, MASKED_ON(1u));
    UInt32 w = 0;
    while (!(RD(FW_ACK_VDBOX0) & 1) && w < 50000) { IODelay(10); w += 10; }
    bool fwOK = RD(FW_ACK_VDBOX0) & 1;
    say(" | media forcewake %s (%u us)", fwOK ? "ok" : "NO ACK", w);
    if (!fwOK) { WR(FW_MEDIA_VDBOX0, MASKED_OFF(1u)); gpuForcewake(false); return kIOReturnError; }
    say(" | before: mode 0x%08x head 0x%x tail 0x%x acthd 0x%x esr 0x%x", RD(VCS0 + R_MODE_GEN7), RD(VCS0 + R_HEAD),
        RD(VCS0 + R_TAIL), RD(VCS0 + R_ACTHD), RD(VCS0 + R_ESR));
    // engine into execlist mode, status page, submit
    WR(VCS0 + R_HWSTAM, 0xFFFFFFFF);
    WR(VCS0 + R_MODE_GEN7, MASKED_ON(1u << 3));               // GEN11_GFX_DISABLE_LEGACY_MODE
    WR(VCS0 + R_MI_MODE, MASKED_OFF(1u << 8));                // STOP_RING off
    WR(VCS0 + R_HWS_PGA, V_GGTT_ADDR + V_HWSP * 4096);
    (void)RD(VCS0 + R_HWS_PGA);
    UInt32 lo = (V_GGTT_ADDR + V_LRC * 4096) | (3u << 3) | (1u << 8) | 1u;   // valid, legacy 64-bit PPGTT, privileged
    UInt32 hi = (6u << 5) | (1u << 29);                       // sw context id 6; engine class 1 = video decode, instance 0
    uint64_t t0; clock_get_uptime(&t0);
    WR(VCS0 + R_EL_SQ + 8, 0);
    WR(VCS0 + R_EL_SQ + 12, 0);
    WR(VCS0 + R_EL_SQ + 0, lo);
    WR(VCS0 + R_EL_SQ + 4, hi);
    WR(VCS0 + R_EL_CONTROL, 1);
    UInt32 us = 0;
    while (vcsVA[V_HWSP * 1024 + HWSP_SEQ] != V_MAGIC && us < 200000) { IODelay(20); us += 20; }
    bool ran = vcsVA[V_HWSP * 1024 + HWSP_SEQ] == V_MAGIC;
    uint64_t t1; clock_get_uptime(&t1);
    UInt64 ns; absolutetime_to_nanoseconds(t1 - t0, &ns);
    say(" | job %s (%llu us)", ran ? "RAN: the engine wrote the magic number" : "did NOT run in 200 ms", ns / 1000);
    say(" | after: head 0x%x tail 0x%x acthd 0x%x esr 0x%x el-status 0x%08x", RD(VCS0 + R_HEAD), RD(VCS0 + R_TAIL),
        RD(VCS0 + R_ACTHD), RD(VCS0 + R_ESR), RD(VCS0 + R_EL_STATUS));
    if (!ran) {                                               // stop the context so the engine is idle again
        WR(VCS0 + R_MI_MODE, MASKED_ON(1u << 8));
        IODelay(1000);
    }
    WR(FW_MEDIA_VDBOX0, MASKED_OFF(1u));
    gpuForcewake(false);
    if (ran) vcsOK++;
    say(" | tests %u, ok %u", vcsTests, vcsOK);
    return ran ? kIOReturnSuccess : kIOReturnTimeout;
}
