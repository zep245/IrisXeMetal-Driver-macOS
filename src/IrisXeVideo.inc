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

// ---------------------------------------------------------------- v0.69.0: video jobs on VCS0
// The VA driver's decode jobs (IrisXeAccel copies each patched command stream into a batch buffer in the process's
// GPU space) run on VCS0 here, one at a time, synchronously (the caller holds mtlJobLock, the GPU points at the
// process's space). Per job (research round 49 / i915 gen12 xcs rules):
//   ring: MI_FLUSH_DW invalidate TLB + video caches (the page tables may have changed) -> MI_BATCH_BUFFER_START
//         (PPGTT, or GGTT for the self-test) -> MI_FLUSH_DW (writes back) -> MI_STORE_DATA_IMM sequence number
//   a fresh context image every job (restore inhibit, as the render engine), polled status page, no interrupts.
// A job that does not end within its timeout: VCS0 alone is reset (GDRST bit 5, GEN11_GRDOM_MEDIA), the desktop
// (render engine) is not touched.
#define V_BATCH             10     // self-test batch page
#define V_LOOP              11     // self-test hang batch page (jumps to itself)
#define HWSP_VSCRATCH       0x80   // MI_FLUSH_DW post-sync writes go here (store index)
#define MI_BB_START_GEN8    MI_INSTR(0x31, 1)
#define MI_BB_PPGTT         (1u << 8)
#define MI_FLUSH_STORE_IDX  (1u << 21)
#define MI_FLUSH_INV_TLB    (1u << 18)
#define MI_FLUSH_OP_STOREDW (1u << 14)
#define MI_FLUSH_INV_BSD    (1u << 7)
#define MI_FLUSH_USE_GTT    (1u << 2)
#define GDRST               0x941C
#define GRDOM_VCS0          (1u << 5)
#define VDBOX0_CGCTL3F10    (VCS0 + 0x3F10)   // Wa_14011060649: IECPUNIT_CLKGATE_DIS (bit 22)
#define GEN10_DFR_CHICKEN   0x9550            // Wa_14011059788: DFR_DISABLE (bit 9)

static UInt32 sVSeq, sVJobs, sVTimeouts, sVResets, sVResetFails, sVLastUs, sVMaxUs;
static UInt64 sVTotalUs;
static bool sVWaDone;
static char sVLast[400];

bool IrisXeProbe::vcsPrepare() {
    if (!gpuUp || !gpuMMIO || !gpuVA) return false;
    if (!vcsMem) {
        vcsMem = IOBufferMemoryDescriptor::withOptions(kIODirectionInOut | kIOMemoryPhysicallyContiguous | kIOMapInhibitCache, V_BYTES, 4096);
        if (!vcsMem) return false;
        vcsVA = (volatile UInt32 *)vcsMem->getBytesNoCopy();
        IOByteCount seg = 0;
        vcsPhys = vcsMem->getPhysicalSegment(0, &seg, kIOMemoryMapperNone);
        if (!vcsVA || !vcsPhys || seg < V_BYTES) { OSSafeReleaseNULL(vcsMem); vcsVA = nullptr; return false; }
        for (UInt32 i = 0; i < V_BYTES / 4; i++) vcsVA[i] = 0;
    }
    // GGTT entries every time (cheap; after a system wake or a full GPU reset they may be gone)
    UInt32 gidx = V_GGTT_ADDR >> 12;
    for (UInt32 i = 0; i < V_BYTES / 4096; i++) {
        UInt64 pte = (vcsPhys + (UInt64)i * 4096) | 1;
        WR(GGTT_BASE + (gidx + i) * 8, (UInt32)pte);
        WR(GGTT_BASE + (gidx + i) * 8 + 4, (UInt32)(pte >> 32));
    }
    (void)RD(GGTT_BASE + gidx * 8);
    WR(GFX_FLSH_CNTL, 1);
    (void)RD(GFX_FLSH_CNTL);
    return true;
}

static void vcsNote(IrisXeProbe *p, const char *what) {
    snprintf(sVLast, sizeof(sVLast), "%s", what);
}

void IrisXeProbe::vcsPublish() {
    char t[400];
    snprintf(t, sizeof(t), "%u jobs, %u timeouts, %u VCS0 resets (%u failed) | last %u us, max %u us, average %llu us | %s",
             sVJobs, sVTimeouts, sVResets, sVResetFails, sVLastUs, sVMaxUs, sVJobs ? sVTotalUs / sVJobs : 0ULL, sVLast);
    setProperty("IrisXe-VideoEngine", t);
}

// VCS0 alone: stop request, GDRST media domain, wait for it to clear (media forcewake held by the caller)
bool IrisXeProbe::vcsReset() {
    WR(VCS0 + 0xd0, MASKED_ON(1u << 0));                       // RING_RESET_CTL: REQUEST_RESET
    for (UInt32 t = 0; t < 70 && !(RD(VCS0 + 0xd0) & (1u << 1)); t++) IODelay(10);
    WR(GDRST, GRDOM_VCS0);
    UInt32 t = 0;
    for (; t < 50000 && (RD(GDRST) & GRDOM_VCS0); t++) IODelay(10);
    bool ok = !(RD(GDRST) & GRDOM_VCS0);
    WR(VCS0 + 0xd0, MASKED_OFF(1u << 0));
    sVResets++;
    if (!ok) sVResetFails++;
    return ok;
}

// one job: batch at bbAddr (PPGTT address in the current space, or a GGTT address). 0 ended, -3 timeout (VCS0 reset),
// -9 timeout and the reset failed, -1 not ready, -2 no forcewake, -4 no VDBox 0
int IrisXeProbe::vcsRunJob(UInt64 bbAddr, bool ppgtt, UInt32 timeoutMS, UInt32 *usOut) {
    if (usOut) *usOut = 0;
    if (!vcsPrepare()) return -1;
    // context image (gen12 xcs layout) and ring, fresh for every job
    for (UInt32 i = V_LRC * 1024; i < (V_RING + 2) * 1024; i++) vcsVA[i] = 0;
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
    UInt64 pml4 = gpuPhys + (UInt64)G_PML4 * 4096;           // the shared tables (pointing at the current space)
    regs[CTX_PDP0_UDW]   = (UInt32)(pml4 >> 32);
    regs[CTX_PDP0_LDW]   = (UInt32)pml4;
    const UInt32 seq = ++sVSeq;
    volatile UInt32 *ring = vcsVA + V_RING * 1024;
    UInt32 t = 0;
    const UInt32 flushAddr = (HWSP_VSCRATCH * 4) | MI_FLUSH_USE_GTT;
    ring[t++] = MI_FLUSH_DW_4 | MI_FLUSH_STORE_IDX | MI_FLUSH_OP_STOREDW | MI_FLUSH_INV_TLB | MI_FLUSH_INV_BSD;
    ring[t++] = flushAddr; ring[t++] = 0; ring[t++] = 0;
    ring[t++] = MI_BB_START_GEN8 | (ppgtt ? MI_BB_PPGTT : 0);
    ring[t++] = (UInt32)bbAddr & ~3u;
    ring[t++] = (UInt32)(bbAddr >> 32);
    ring[t++] = MI_FLUSH_DW_4 | MI_FLUSH_STORE_IDX | MI_FLUSH_OP_STOREDW | MI_FLUSH_INV_TLB;
    ring[t++] = flushAddr; ring[t++] = 0; ring[t++] = 0;
    ring[t++] = MI_SDI(2) | MI_USE_GGTT;
    ring[t++] = V_GGTT_ADDR + V_HWSP * 4096 + HWSP_SEQ * 4;
    ring[t++] = 0;
    ring[t++] = seq;
    while (t & 1) ring[t++] = MI_NOOP;
    regs[CTX_RING_HEAD] = 0;
    regs[CTX_RING_TAIL] = t * 4;
    __asm__ volatile("mfence" ::: "memory");

    if (!gpuForcewake(true)) { gpuForcewake(false); vcsNote(this, "GT forcewake failed"); return -2; }
    if (RD(GT_VDBOX_DISABLE) & 1) { gpuForcewake(false); vcsNote(this, "no VDBox 0 (fuse)"); return -4; }
    WR(FW_MEDIA_VDBOX0, MASKED_ON(1u));
    UInt32 w = 0;
    while (!(RD(FW_ACK_VDBOX0) & 1) && w < 50000) { IODelay(10); w += 10; }
    if (!(RD(FW_ACK_VDBOX0) & 1)) { WR(FW_MEDIA_VDBOX0, MASKED_OFF(1u)); gpuForcewake(false); vcsNote(this, "media forcewake: no ack"); return -2; }
    {                                                          // v0.69.4: no HCP/MFX power gating for the VDBoxes
        UInt32 pg = RD(0xA210);                                //   (see gpuInit: the MFX unit never woke up)
        if (pg & 0x198u) WR(0xA210, pg & ~0x198u);
    }
    if (!sVWaDone) {                                           // i915 gen12 workarounds for VDBox 0 (once per boot)
        WR(VDBOX0_CGCTL3F10, RD(VDBOX0_CGCTL3F10) | (1u << 22));
        WR(GEN10_DFR_CHICKEN, RD(GEN10_DFR_CHICKEN) | (1u << 9));
        sVWaDone = true;
    }
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
    // wait: spin the first 2 ms (most frames), then sleep in 1 ms steps
    volatile UInt32 *done = vcsVA + V_HWSP * 1024 + HWSP_SEQ;
    UInt64 ns = 0;
    const UInt64 limit = (UInt64)(timeoutMS ? timeoutMS : 2000) * 1000000ULL;
    for (;;) {
        if (*done == seq) break;
        uint64_t tn; clock_get_uptime(&tn);
        absolutetime_to_nanoseconds(tn - t0, &ns);
        if (ns >= limit) break;
        if (ns < 2000000ULL) IODelay(10); else IOSleep(1);
    }
    bool ran = *done == seq;
    uint64_t t1; clock_get_uptime(&t1);
    absolutetime_to_nanoseconds(t1 - t0, &ns);
    UInt32 us = (UInt32)(ns / 1000);
    if (usOut) *usOut = us;
    int r = 0;
    if (!ran) {
        sVTimeouts++;
        // v0.69.3: + the command the engine is on (IPEHR = its header), INSTDONE / INSTPS, the MFX status registers
        // (CodecHAL: error flags +0x800, CRC +0x850, macroblock count +0x868) and the power-gating enables (0xA210)
        snprintf(sVLast, sizeof(sVLast), "TIMEOUT after %u us: head 0x%x tail 0x%x acthd 0x%x ipehr 0x%08x ipeir 0x%x "
                 "instdone 0x%08x instps 0x%08x esr 0x%x el-status 0x%08x | mfx err 0x%x crc 0x%x mbs 0x%x | pg 0x%x", us,
                 RD(VCS0 + R_HEAD), RD(VCS0 + R_TAIL), RD(VCS0 + R_ACTHD), RD(VCS0 + 0x68), RD(VCS0 + 0x64),
                 RD(VCS0 + 0x6c), RD(VCS0 + 0x70), RD(VCS0 + R_ESR), RD(VCS0 + R_EL_STATUS),
                 RD(VCS0 + 0x800), RD(VCS0 + 0x850), RD(VCS0 + 0x868), RD(0xA210));
        IOLog("IrisXeGPU: video job %s\n", sVLast);
        r = vcsReset() ? -3 : -9;
    } else {
        sVJobs++; sVLastUs = us; sVTotalUs += us; if (us > sVMaxUs) sVMaxUs = us;
    }
    WR(FW_MEDIA_VDBOX0, MASKED_OFF(1u));
    gpuForcewake(false);
    return r;
}

// v0.69.0 stage A: `rounds` tiny jobs (GGTT batch: store a number into the status page), then (hang = true) a job
// that never ends (a batch jumping to itself) must time out, reset VCS0 and the next job must run again
int IrisXeProbe::vcsSelfTest(UInt32 rounds, bool hang, char *rep, UInt32 repLen) {
    UInt32 o = 0;
    auto say = [&](const char *fmt, auto... args) {
        if (o + 1 < repLen) o += snprintf(rep + o, repLen - o, fmt, args...);
        if (o >= repLen) o = repLen - 1;
    };
    if (!vcsPrepare()) { say("%s", "not ready (GPU core not up / no memory)"); return -1; }
    if (rounds > 1000) rounds = 1000;
    UInt32 ok = 0, bad = 0, maxUs = 0; UInt64 sumUs = 0;
    int first = 0;
    for (UInt32 k = 0; k < rounds; k++) {
        volatile UInt32 *b = vcsVA + V_BATCH * 1024;
        const UInt32 magic = 0x5E1F0000u | (k & 0xFFFF);
        vcsVA[V_HWSP * 1024 + HWSP_SEQ + 4] = 0;
        b[0] = MI_SDI(2) | MI_USE_GGTT;
        b[1] = V_GGTT_ADDR + V_HWSP * 4096 + (HWSP_SEQ + 4) * 4;
        b[2] = 0; b[3] = magic;
        b[4] = MI_BB_END; b[5] = MI_NOOP;
        __asm__ volatile("mfence" ::: "memory");
        UInt32 us = 0;
        int r = vcsRunJob(V_GGTT_ADDR + V_BATCH * 4096, false, 200, &us);
        if (r == 0 && vcsVA[V_HWSP * 1024 + HWSP_SEQ + 4] == magic) { ok++; sumUs += us; if (us > maxUs) maxUs = us; }
        else { bad++; if (!first) first = r ? r : -7; if (r == -1 || r == -2 || r == -4 || r == -9) break; }
    }
    say("jobs %u/%u ok (average %llu us, max %u us)", ok, rounds, ok ? sumUs / ok : 0ULL, maxUs);
    if (bad) say(" | first failure %d: %s", first, sVLast);
    int res = bad ? -5 : 0;
    if (hang && !bad) {
        volatile UInt32 *l = vcsVA + V_LOOP * 1024;
        l[0] = MI_NOOP; l[1] = MI_NOOP;
        l[2] = MI_BB_START_GEN8; l[3] = V_GGTT_ADDR + V_LOOP * 4096; l[4] = 0; l[5] = MI_NOOP;
        __asm__ volatile("mfence" ::: "memory");
        uint64_t t0; clock_get_uptime(&t0);
        UInt32 us = 0;
        int r = vcsRunJob(V_GGTT_ADDR + V_LOOP * 4096, false, 300, &us);
        uint64_t t1; clock_get_uptime(&t1);
        UInt64 ns; absolutetime_to_nanoseconds(t1 - t0, &ns);
        say(" | forced hang: %s, recovered in %llu ms", r == -3 ? "timed out, VCS0 reset ok" : r == -9 ? "RESET FAILED" : "did not hang?!",
            ns / 1000000ULL);
        // the engine must work again
        volatile UInt32 *b = vcsVA + V_BATCH * 1024;
        vcsVA[V_HWSP * 1024 + HWSP_SEQ + 4] = 0;
        b[3] = 0x5E1FAFAFu;
        __asm__ volatile("mfence" ::: "memory");
        int r2 = vcsRunJob(V_GGTT_ADDR + V_BATCH * 4096, false, 200, &us);
        bool after = r2 == 0 && vcsVA[V_HWSP * 1024 + HWSP_SEQ + 4] == 0x5E1FAFAFu;
        say(" | job after the reset: %s", after ? "ok" : "FAILED");
        if (r != -3 || !after) res = -6;
    }
    // v0.69.3 stage A2: the first commands of a real H.264 decode job (v0.69.2 record), one step more each time, as
    // GGTT batches (no addresses in them): where does the video engine stop? v0.69.2's decode jobs all stopped at
    // dword 17 of the batch, right after PIPE_MODE_SELECT + MFX_WAIT.
    if (hang && !bad) {
        static const UInt32 kSteps[][12] = {
            { 1, 0x13000082u, 0, 0, 0 },                                         // MI_FLUSH_DW (+ invalidate BSD)
            { 2, 0x68000100u },                                                  // MFX_WAIT
            { 3, 0x70000003u, 0x202, 0, 0, 0 },                                  // MFX_PIPE_MODE_SELECT: AVC, VLD decode
            { 4, 0x70000003u, 0x202, 0, 0, 0, 0x68000100u },                     // + MFX_WAIT
            { 5, 0x70000003u, 0x202, 0, 0, 0, 0x68000100u, 0x70010004u, 0, 0x077c27f0u, 0x08001e1bu, 0x13eu },  // + SURFACE_STATE
        };
        static const UInt32 kLen[] = { 4, 1, 5, 6, 11 };
        say("%s", " | MFX steps:");
        for (UInt32 k = 0; k < 5; k++) {
            volatile UInt32 *b = vcsVA + 12 * 1024;
            UInt32 n = 0;
            for (UInt32 j = 0; j < kLen[k]; j++) b[n++] = kSteps[k][1 + j];
            if (kLen[k] == 11) b[n++] = 0;                                       // SURFACE_STATE dword 5
            b[n++] = MI_BB_END;
            if (n & 1) b[n++] = MI_NOOP;
            __asm__ volatile("mfence" ::: "memory");
            UInt32 us = 0;
            int r = vcsRunJob(V_GGTT_ADDR + 12 * 4096, false, 100, &us);
            say(" %u:%s", k + 1, r == 0 ? "ok" : r == -3 ? "HANG" : "fail");
            if (r == 0) say("(%u us)", us);
            else { say(" [%s]", sVLast); break; }
        }
    }
    say(" | totals: %u jobs, %u timeouts, %u resets", sVJobs, sVTimeouts, sVResets);
    vcsNote(this, res ? "self-test FAILED" : "self-test passed");
    vcsPublish();
    return res;
}
